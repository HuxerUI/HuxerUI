#include "platform.h"

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

std::filesystem::path AppImageTool() {
  const std::optional<std::filesystem::path> tool = FindExecutable("appimagetool");
  if (!tool) {
    throw std::runtime_error("HuxerUI Linux packaging requires appimagetool on PATH");
  }
  return *tool;
}

std::filesystem::path LinuxPackageArtifact(const PlatformCommandContext& context, std::string_view plan) {
  const std::string target = detail::JsonString(plan, "target");
  const std::string version = detail::JsonString(plan, "version");
  auto root = context.project_root / ".huxerui/package/linux" / context.profile;
  if (!context.abis.empty()) {
    root /= context.abis.front();
  }
  return root / (target + "-" + version + ".AppImage");
}

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

  std::vector<EnvironmentDiagnostic> DiagnoseEnvironment() const override {
    std::vector<EnvironmentDiagnostic> diagnostics = PlatformDriver::DiagnoseEnvironment();
    if (!SupportsCurrentHost() || !FindExecutable("pkg-config")) {
      return diagnostics;
    }
    struct PackageRequirement {
      std::string_view name;
      std::string_view minimum_version;
    };
    static constexpr std::array packages{
        PackageRequirement{"gtk4", "4.14"},
        PackageRequirement{"epoxy", "1.5"},
        PackageRequirement{"gio-2.0", {}},
        PackageRequirement{"pangoft2", {}},
        PackageRequirement{"fontconfig", {}},
        PackageRequirement{"libsoup-3.0", "3.0"},
    };
    for (const PackageRequirement& package : packages) {
      const ProcessResult version_result =
          RunProcessCapture({"pkg-config", {"--modversion", std::string(package.name)}, {}});
      std::string version = version_result.output;
      while (!version.empty() && std::isspace(static_cast<unsigned char>(version.back()))) {
        version.pop_back();
      }
      bool ready = version_result.exit_code == 0;
      if (ready && !package.minimum_version.empty()) {
        const ProcessResult requirement_result = RunProcessCapture(
            {"pkg-config", {"--atleast-version=" + std::string(package.minimum_version), std::string(package.name)}, {}}
        );
        ready = requirement_result.exit_code == 0;
      }
      const std::string requirement = package.minimum_version.empty()
                                          ? std::string(package.name)
                                          : std::string(package.name) + " >= " + std::string(package.minimum_version);
      diagnostics.push_back({
          ready ? EnvironmentDiagnosticStatus::Ready : EnvironmentDiagnosticStatus::Missing,
          "pkg:" + std::string(package.name),
          requirement + " development package",
          version_result.exit_code == 0 ? std::move(version) : std::string{},
      });
    }
    return diagnostics;
  }

  std::vector<SetupAction> PlanSetup(std::span<const EnvironmentDiagnostic> diagnostics) const override {
    const bool missing_tool =
        std::any_of(diagnostics.begin(), diagnostics.end(), [](const EnvironmentDiagnostic& diagnostic) {
          return diagnostic.status == EnvironmentDiagnosticStatus::Missing && !diagnostic.id.starts_with("pkg:");
        });
    const bool missing_package =
        std::any_of(diagnostics.begin(), diagnostics.end(), [](const EnvironmentDiagnostic& diagnostic) {
          return diagnostic.status == EnvironmentDiagnosticStatus::Missing && diagnostic.id.starts_with("pkg:");
        });
    std::vector<SetupAction> actions;
    if (missing_tool) {
      actions.push_back({
          "Install pkg-config with the host distribution package manager",
          std::nullopt,
      });
    }
    if (missing_package) {
      actions.push_back({
          "Install the required GTK 4.14, libepoxy, GIO, and libsoup 3 development packages",
          std::nullopt,
      });
    }
    return actions;
  }

  std::vector<GeneratedFile> CreateShell(const ProjectTemplateContext& context) const override {
    std::vector<GeneratedFile> files = RenderTemplateTree("platform/desktop/app", context);
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
    if (!std::filesystem::is_regular_file(context.huxerui_home / "CMakeLists.txt") ||
        !std::filesystem::is_regular_file(LinuxToolchain(context))) {
      throw std::invalid_argument(
          "HuxerUI Linux cross-builds require a source checkout via --sdk and CMAKE_TOOLCHAIN_FILE for " +
          context.abis.front());
    }
  }

  void VerifyBuildAbis(const PlatformCommandContext& context) const override {
    if (context.abis.empty()) {
      return;
    }
    const auto artifact = detail::JsonString(detail::ReadFile(detail::AppIntegrationPlan(context)), "artifact");
    std::ifstream executable(std::filesystem::path(artifact), std::ios::binary);
    std::array<unsigned char, 20> header{};
    executable.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    const unsigned char machine = context.abis.front() == "aarch64" ? 0xb7 : 0x3e;
    if (!executable || header[0] != 0x7f || header[1] != 'E' || header[2] != 'L' || header[3] != 'F' ||
        header[4] != 2 || header[5] != 1 || header[18] != machine || header[19] != 0) {
      throw std::runtime_error("HuxerUI Linux build artifact does not match ABI " + context.abis.front() +
                               ": " + artifact);
    }
  }

  std::vector<ProcessCommand> BuildCommands(const PlatformCommandContext& context) const override {
    if (context.package && SupportsCurrentHost()) {
      (void)AppImageTool();
    }
    auto commands = detail::DesktopBuildCommands(context);
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
    const std::filesystem::path appimagetool = AppImageTool();
    const std::filesystem::path artifact = LinuxPackageArtifact(context, plan);
    const std::filesystem::path root = artifact.parent_path();
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
