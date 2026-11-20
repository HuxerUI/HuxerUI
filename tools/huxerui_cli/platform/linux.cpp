#include "platform.h"

#include "toolchain.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <utility>

#include "template.h"

namespace huxerui::cli {
namespace {

std::string_view NativeLinuxAbi() {
#if defined(__aarch64__) || defined(_M_ARM64)
  return "aarch64";
#elif defined(__x86_64__) || defined(_M_X64)
  return "x86_64";
#else
  return {};
#endif
}

std::filesystem::path LinuxToolchain(const PlatformCommandContext& context) {
  const auto toolchain = ReadEnvironmentVariable("CMAKE_TOOLCHAIN_FILE");
  if (!toolchain || toolchain->empty() || context.abis.front() == NativeLinuxAbi()) {
    return {};
  }
  return (context.project_root / *toolchain).lexically_normal();
}

std::filesystem::path LinuxPackageTool(std::string_view name, const ProcessEnvironment& environment) {
  const std::optional<std::filesystem::path> tool = FindExecutable(name, environment);
  if (!tool) {
    throw std::runtime_error("HuxerUI Linux packaging requires " + std::string(name) + " on PATH");
  }
  return *tool;
}

bool IsLinuxSystemPackage(const PlatformCommandContext& context) {
  return context.package_format == "deb" || context.package_format == "rpm";
}

std::filesystem::path LinuxPackageArtifact(const PlatformCommandContext& context, std::string_view plan) {
  const std::string version = detail::JsonString(plan, "version");
  auto root = context.project_root / ".huxerui/package/linux" / context.profile;
  if (!context.abis.empty()) {
    root /= context.abis.front();
  }
  if (IsLinuxSystemPackage(context)) {
    const std::string id = detail::JsonString(plan, "bundleIdentifier");
    return root / (id + "-" + version + "." + *context.package_format);
  }
  return root / (detail::JsonString(plan, "target") + "-" + version + ".AppImage");
}

std::filesystem::path ManagedPatchelf() {
  const auto root = detail::ManagedToolDirectory();
  if (root.empty())
    return {};
  return root / "patchelf/0.19.2" / detail::HostArchitecture() / "bin/patchelf";
}

struct LinuxPackageRequirement {
  std::string_view name;
  std::string_view minimum;
  std::string_view apt_package;
};

constexpr std::array development_packages{
    LinuxPackageRequirement{"gtk4", "4.14", "libgtk-4-dev"},
    LinuxPackageRequirement{"epoxy", "1.5", "libepoxy-dev"},
    LinuxPackageRequirement{"gio-2.0", {}, "libglib2.0-dev"},
    LinuxPackageRequirement{"pangoft2", {}, "libpango1.0-dev"},
    LinuxPackageRequirement{"fontconfig", {}, "libfontconfig1-dev"},
    LinuxPackageRequirement{"libsoup-3.0", "3.0", "libsoup-3.0-dev"},
};

class LinuxDriver final : public PlatformDriver {
public:
  std::string_view Id() const noexcept override {
    return "linux";
  }

  std::span<const std::string_view> TargetAbis() const noexcept override {
    static constexpr std::array abis{std::string_view{"aarch64"}, std::string_view{"x86_64"}};
    return abis;
  }

  bool SupportsCurrentHost() const noexcept override {
    return CurrentHostId() == "linux";
  }

  std::span<const std::string_view> RequiredTools() const noexcept override {
    static constexpr std::array tools{std::string_view{"pkg-config"}};
    return tools;
  }

  ProcessEnvironment ResolveEnvironment(const EnvironmentRequest&) const override {
    ProcessEnvironment environment;
    const auto patchelf = ManagedPatchelf();
    if (!patchelf.empty() && std::filesystem::is_regular_file(patchelf)) {
      const auto system = FindExecutable("patchelf").value_or(std::filesystem::path{});
      if (detail::CheckTool("patchelf", "patchelf", system, "0.19.0").status != EnvironmentDiagnosticStatus::Ready) {
        detail::PrependPath(environment, patchelf.parent_path());
      }
    }
    return environment;
  }

  std::vector<EnvironmentDiagnostic> DiagnoseEnvironment(const EnvironmentRequest& request) const override {
    auto diagnostics = PlatformDriver::DiagnoseEnvironment(request);
    if (!SupportsCurrentHost() || request.operation == EnvironmentOperation::Devices)
      return diagnostics;
    const auto cxx = ReadEnvironmentVariable("CXX").value_or("c++");
    const auto compiler = std::filesystem::path(cxx).has_parent_path()
                              ? std::filesystem::path(cxx)
                              : FindExecutable(cxx).value_or(std::filesystem::path{});
    diagnostics.push_back(detail::CheckTool(
        "compiler", "C++ compiler", std::filesystem::is_regular_file(compiler) ? compiler : std::filesystem::path{}));
    const auto generator =
        request.generator.empty() ? ReadEnvironmentVariable("CMAKE_GENERATOR").value_or("") : request.generator;
    const auto build_tool =
        generator.starts_with("Ninja") || (generator.empty() && FindExecutable("ninja")) ? "ninja" : "make";
    diagnostics.push_back(
        detail::CheckTool(build_tool, build_tool, FindExecutable(build_tool).value_or(std::filesystem::path{})));
    const auto pkg_config = FindExecutable("pkg-config");
    for (const auto& package : development_packages) {
      EnvironmentDiagnostic check{EnvironmentDiagnosticStatus::Blocked,
                                  "pkg:" + std::string(package.name),
                                  std::string(package.name) + " development package",
                                  {},
                                  {},
                                  std::string(package.minimum)};
      if (pkg_config) {
        // A missing package is this check's result, not a message for the terminal.
        const auto result = RunProcessCapture(
            {pkg_config->string(), {"--silence-errors", "--modversion", std::string(package.name)}, {}});
        check.version = detail::ParseToolVersion(result.output);
        check.status =
            result.exit_code == 0 && (package.minimum.empty() || detail::VersionAtLeast(check.version, package.minimum))
                ? EnvironmentDiagnosticStatus::Ready
                : EnvironmentDiagnosticStatus::Missing;
      }
      diagnostics.push_back(std::move(check));
    }
    if (request.operation == EnvironmentOperation::Package) {
      const auto environment = ResolveEnvironment(request);
      diagnostics.push_back(detail::CheckTool("patchelf", "patchelf",
                                              FindExecutable("patchelf", environment).value_or(std::filesystem::path{}),
                                              "0.19.0", environment));
      const auto format = request.package_format;
      const auto tool = format == "deb" ? "dpkg-shlibdeps" : format == "rpm" ? "rpmbuild" : "appimagetool";
      diagnostics.push_back(detail::CheckTool(tool, tool, FindExecutable(tool).value_or(std::filesystem::path{})));
      if (format == "deb" || format == "rpm") {
        const auto cmake = request.cmake_path ? *request.cmake_path : detail::ResolveCMake().path;
        const auto cpack = cmake.empty() ? std::filesystem::path{} : cmake.parent_path() / "cpack";
        diagnostics.push_back(detail::CheckTool(
            "cpack", "CPack", std::filesystem::is_regular_file(cpack) ? cpack : std::filesystem::path{}));
      }
    }
    // Every Linux requirement is a compiler, a development package, or a packaging tool the build takes from the host.
    for (auto& check : diagnostics)
      check.host_toolchain = true;
    return diagnostics;
  }

  std::vector<SetupAction> PlanSetup(std::span<const EnvironmentDiagnostic> checks,
                                     const EnvironmentRequest&) const override {
    if (!SupportsCurrentHost())
      return {};
    std::vector<SetupAction> actions;
    const bool ubuntu = detail::IsUbuntuNoble();
    const auto apt = [&](std::string id, std::vector<std::string> packages, std::vector<std::string> provides,
                         std::vector<std::string> prerequisites = {}) {
      const std::string description = "Install " + detail::JoinTargetAbis(packages, ' ');
      if (!ubuntu) {
        actions.push_back({description + " or the equivalent packages for this distribution", std::nullopt,
                           std::move(id), std::move(prerequisites), std::move(provides)});
        return;
      }
      actions.push_back({description + " with apt", std::nullopt, std::move(id), std::move(prerequisites),
                         std::move(provides),
                         [packages = std::move(packages)] { return detail::InstallUbuntuPackages(packages); }, true});
    };
    std::vector<std::string> host_packages;
    std::vector<std::string> host_requirements;
    for (const auto& [id, package] : {std::pair{"pkg-config", "pkg-config"},
                                      {"compiler", "build-essential"},
                                      {"make", "make"},
                                      {"ninja", "ninja-build"},
                                      {"dpkg-shlibdeps", "dpkg-dev"},
                                      {"rpmbuild", "rpm"}}) {
      if (!detail::NeedsSetup(checks, id))
        continue;
      if (std::string_view(id) == "compiler" && ReadEnvironmentVariable("CXX")) {
        actions.push_back({"Correct CXX to select an installed C++ compiler", std::nullopt, "select_compiler"});
        continue;
      }
      host_packages.emplace_back(package);
      host_requirements.emplace_back(id);
    }
    if (!host_packages.empty())
      apt("install_host_tools", std::move(host_packages), std::move(host_requirements));
    std::vector<std::string> packages;
    std::vector<std::string> needed;
    // Without pkg-config the development checks are blocked. Keep their packages in the plan for the recheck.
    for (const auto& package : development_packages) {
      const auto id = "pkg:" + std::string(package.name);
      if (detail::NeedsSetup(checks, id) || detail::NeedsSetup(checks, "pkg-config")) {
        packages.emplace_back(package.apt_package);
        needed.push_back(id);
      }
    }
    if (!packages.empty())
      apt("install_development_packages", std::move(packages), std::move(needed), {"pkg-config"});
    if (detail::NeedsSetup(checks, "patchelf")) {
      const auto path = ManagedPatchelf();
      if (!path.empty() && detail::HostArchitecture() != "unknown") {
        const bool arm = detail::HostArchitecture() == "arm64";
        actions.push_back(detail::InstallTool(
            "install_patchelf", "Install patchelf 0.19.2",
            {std::string("https://github.com/NixOS/patchelf/releases/download/0.19.2/patchelf-0.19.2-") +
                 (arm ? "aarch64" : "x86_64") + ".tar.gz",
             arm ? "ba6850c1a6f4cbdb050e1cc22fa5239159ad31c43606846e49c408d335f594ff"
                 : "2abdc34fbe949c995a1baa4ec310896a085588a45a99cd0d726851bdfac2a4bb",
             "", path.parent_path().parent_path()},
            {}, {"patchelf"}));
      }
    }
    if (detail::NeedsSetup(checks, "appimagetool")) {
      actions.push_back({"Install appimagetool from https://github.com/AppImage/appimagetool/releases", std::nullopt,
                         "install_appimagetool"});
    }
    return actions;
  }

  std::vector<GeneratedFile> CreateShell(const ProjectTemplateContext& context) const override {
    std::vector<GeneratedFile> files = RenderTemplateTree(DesktopEntryTemplate(context), context);
    std::vector<GeneratedFile> platform_files = RenderTemplateTree("platform/linux/app", context);
    files.insert(files.end(), std::make_move_iterator(platform_files.begin()),
                 std::make_move_iterator(platform_files.end()));
    return files;
  }

  GeneratedTree CreateLibraryPackage(const LibraryTemplateContext&,
      const std::filesystem::path& library_root) const override {
    return {library_root / "platform" / Id(), {{"src/.gitkeep", {}}}};
  }

  std::vector<Diagnostic> Diagnose(const std::filesystem::path& shell_root) const override {
    static constexpr std::array required{
        std::string_view{"main.cpp"},
        std::string_view{"huxerui.cmake"},
        std::string_view{"package/AppRun"},
    };
    return detail::ValidateRequiredFiles(shell_root, required);
  }

  void ValidateBuildAbis(const PlatformCommandContext& context) const override {
    if (context.abis.empty() || context.abis.front() == NativeLinuxAbi()) {
      return;
    }
    if (IsLinuxSystemPackage(context)) {
      throw std::invalid_argument("HuxerUI DEB and RPM packages require a native Linux build");
    }
    if (!std::filesystem::is_regular_file(context.huxerui_home / "CMakeLists.txt") ||
        !std::filesystem::is_regular_file(LinuxToolchain(context))) {
      throw std::invalid_argument(
          "HuxerUI Linux cross-builds require a source checkout via --sdk and CMAKE_TOOLCHAIN_FILE for " +
          context.abis.front());
    }
  }

  void VerifyBuildAbis(const PlatformCommandContext& context) const override {
    if (context.abis.empty() && !IsLinuxSystemPackage(context)) {
      return;
    }
    const std::string_view abi = context.abis.empty() ? NativeLinuxAbi() : std::string_view(context.abis.front());
    const auto artifact = detail::JsonString(detail::ReadFile(detail::AppIntegrationPlan(context)), "artifact");
    std::ifstream executable(std::filesystem::path(artifact), std::ios::binary);
    std::array<unsigned char, 20> header{};
    executable.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    const unsigned char machine = abi == "aarch64" ? 0xb7 : 0x3e;
    if (!executable || header[0] != 0x7f || header[1] != 'E' || header[2] != 'L' || header[3] != 'F' ||
        header[4] != 2 || header[5] != 1 || header[18] != machine || header[19] != 0) {
      throw std::runtime_error("HuxerUI Linux build artifact does not match ABI " + std::string(abi) + ": " + artifact);
    }
  }

  std::vector<ProcessCommand> BuildCommands(const PlatformCommandContext& context) const override {
    const bool system_package = IsLinuxSystemPackage(context);
    if (context.package && SupportsCurrentHost()) {
      if (system_package) {
        (void)LinuxPackageTool("cpack", context.environment);
        (void)LinuxPackageTool(context.package_format == "deb" ? "dpkg-shlibdeps" : "rpmbuild", context.environment);
      } else {
        (void)LinuxPackageTool("appimagetool", context.environment);
      }
    }
    auto commands = detail::DesktopBuildCommands(context);
    if (context.package) {
      commands.front().arguments.push_back(
          std::string("-DHUXERUI_LINUX_SYSTEM_PACKAGE=") + (system_package ? "ON" : "OFF"));
    }
    if (!context.abis.empty()) {
      commands.front().arguments.push_back("-DCMAKE_TOOLCHAIN_FILE=" + LinuxToolchain(context).string());
    }
    return commands;
  }

  std::vector<ProcessCommand> RunCommands(const PlatformCommandContext& context) const override {
    const std::string artifact = detail::JsonString(detail::ReadFile(detail::AppIntegrationPlan(context)), "artifact");
    if (!std::filesystem::is_regular_file(artifact)) {
      throw std::runtime_error("HuxerUI Linux build artifact is missing: " + artifact);
    }
    return {{artifact, {}, std::filesystem::path(artifact).parent_path()}};
  }

  std::vector<PackageArtifact> PackageArtifacts(const PlatformCommandContext& context) const override {
    const std::string plan = detail::ReadFile(detail::AppIntegrationPlan(context));
    const std::filesystem::path artifact = LinuxPackageArtifact(context, plan);
    return {{artifact, artifact.filename()}};
  }

  std::vector<ProcessCommand> PackageCommands(const PlatformCommandContext& context) const override {
    const std::string plan = detail::ReadFile(detail::AppIntegrationPlan(context));
    const std::string install_component = detail::JsonString(plan, "installComponent");
    const std::filesystem::path artifact = LinuxPackageArtifact(context, plan);
    const std::filesystem::path root = artifact.parent_path();
    if (IsLinuxSystemPackage(context)) {
      const auto config = context.build_directory / "huxerui-package/LinuxCPack.cmake";
      if (!std::filesystem::is_regular_file(config)) {
        throw std::runtime_error(
            "HuxerUI DEB and RPM packaging requires the updated platform/linux/huxerui.cmake install rules");
      }
      const std::filesystem::path stage = root / *context.package_format;
      auto commands = detail::DesktopPackageStageCommands(context, stage, install_component);
      const bool deb = context.package_format == "deb";
      commands.push_back({
          LinuxPackageTool("cpack", context.environment).string(),
          {"-G", deb ? "DEB" : "RPM", "--config", config.string(), "-B", root.string(), "-D",
           "CPACK_INSTALLED_DIRECTORIES=" + stage.string() + ";/", "-D",
           "CPACK_PACKAGE_VERSION=" + detail::JsonString(plan, "version"), "-D",
           "CPACK_PACKAGE_FILE_NAME=" + artifact.stem().string()},
          context.project_root,
      });
      return commands;
    }
    const std::filesystem::path appimagetool = LinuxPackageTool("appimagetool", context.environment);
    const std::filesystem::path staging = root / "AppDir";
    std::vector<ProcessCommand> commands =
        detail::DesktopPackageStageCommands(context, staging, install_component);
    if (context.abis.empty()) {
      commands.push_back({appimagetool.string(), {staging.string(), artifact.string()}, root});
    } else {
      commands.push_back({"cmake",
                          {"-E", "env", "ARCH=" + context.abis.front(), appimagetool.string(),
                           staging.string(), artifact.string()}, root});
    }
    return commands;
  }
};

} // namespace

namespace detail {

const PlatformDriver& LinuxPlatformDriver() noexcept {
  static const LinuxDriver driver;
  return driver;
}

} // namespace detail
} // namespace huxerui::cli
