#include "platform.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

#include "template.h"

namespace huxerui::cli {

namespace detail {

EnvironmentDiagnostic
LinuxCmakePackageDiagnostic(std::string_view name, std::string_view minimum_version, const ProcessResult& result) {
  const bool available = result.exit_code == 0;
  std::string label(name);
  if (!minimum_version.empty()) {
    label += " ";
    label += minimum_version;
    label += " or newer";
  }
  label += " CMake package";
  return {
      available ? EnvironmentDiagnosticStatus::Ready : EnvironmentDiagnosticStatus::Missing,
      "cmake:" + std::string(name),
      std::move(label),
      available ? "available" : std::string{},
  };
}

EnvironmentDiagnostic LinuxPkgConfigDiagnostic(
    std::string_view name,
    std::string_view minimum_version,
    const ProcessResult& version_result,
    int requirement_exit_code
) {
  std::string version = version_result.output;
  while (!version.empty() && std::isspace(static_cast<unsigned char>(version.back()))) {
    version.pop_back();
  }
  const bool available = version_result.exit_code == 0 && requirement_exit_code == 0;
  std::string label(name);
  if (!minimum_version.empty()) {
    label += " ";
    label += minimum_version;
    label += " or newer";
  }
  label += " development package";
  return {
      available ? EnvironmentDiagnosticStatus::Ready : EnvironmentDiagnosticStatus::Missing,
      "pkg:" + std::string(name),
      std::move(label),
      available ? std::move(version) : std::string{},
  };
}

} // namespace detail

namespace {

class CmakeProbeDirectory final {
public:
  CmakeProbeDirectory() : path_(Create()) {}

  ~CmakeProbeDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path& Path() const noexcept {
    return path_;
  }

private:
  static std::filesystem::path Create() {
    std::string pattern = (std::filesystem::temp_directory_path() / "huxerui-cmake-probe-XXXXXX").string();
    std::vector<char> writable_pattern(pattern.begin(), pattern.end());
    writable_pattern.push_back('\0');
    char* created = ::mkdtemp(writable_pattern.data());
    if (created == nullptr) {
      throw std::filesystem::filesystem_error(
          "HuxerUI CLI could not create the Linux CMake package probe directory",
          std::error_code(errno, std::generic_category())
      );
    }
    return created;
  }

  std::filesystem::path path_;
};

class LinuxDriver final : public PlatformDriver {
public:
  std::string_view Id() const noexcept override {
    return "linux";
  }

  bool SupportsCurrentHost() const noexcept override {
    return CurrentHostId() == "linux";
  }

  std::span<const std::string_view> RequiredTools() const noexcept override {
    static constexpr std::array tools{std::string_view{"cmake"}, std::string_view{"pkg-config"}};
    return tools;
  }

  std::vector<EnvironmentDiagnostic> DiagnoseEnvironment() const override {
    std::vector<EnvironmentDiagnostic> diagnostics = PlatformDriver::DiagnoseEnvironment();
    if (!SupportsCurrentHost()) {
      return diagnostics;
    }
    if (FindExecutable("cmake")) {
      struct CmakePackageRequirement {
        std::string_view name;
        std::string_view minimum_version;
      };
      static constexpr std::array cmake_packages{
          CmakePackageRequirement{"SDL3", "3.4"},
          CmakePackageRequirement{"SDL3_image", "3.4"},
          CmakePackageRequirement{"SDL3_ttf", "3.2"},
      };
      for (const CmakePackageRequirement& package : cmake_packages) {
        CmakeProbeDirectory probe_directory;
        std::ofstream project(probe_directory.Path() / "CMakeLists.txt");
        project << "cmake_minimum_required(VERSION 3.20)\n"
                   "project(HuxerUIDependencyProbe LANGUAGES NONE)\n"
                   "find_package("
                << package.name << " " << package.minimum_version << " REQUIRED CONFIG)\n";
        project.close();
        const ProcessResult result = RunProcessCapture({
            "cmake",
            {"-S", probe_directory.Path().string(), "-B", (probe_directory.Path() / "build").string()},
            probe_directory.Path(),
        });
        diagnostics.push_back(detail::LinuxCmakePackageDiagnostic(package.name, package.minimum_version, result));
      }
    }
    if (!FindExecutable("pkg-config")) {
      return diagnostics;
    }
    struct PackageRequirement {
      std::string_view name;
      std::string_view minimum_version;
    };
    static constexpr std::array packages{
        PackageRequirement{"gio-2.0", {}},
        PackageRequirement{"libsoup-3.0", "3.0"},
    };
    for (const PackageRequirement& package : packages) {
      const ProcessResult version_result =
          RunProcessCapture({"pkg-config", {"--modversion", std::string(package.name)}, {}});
      int requirement_exit_code = version_result.exit_code;
      if (!package.minimum_version.empty()) {
        const ProcessResult requirement_result = RunProcessCapture({
            "pkg-config",
            {
                "--atleast-version=" + std::string(package.minimum_version),
                std::string(package.name),
            },
            {},
        });
        requirement_exit_code = requirement_result.exit_code;
      }
      diagnostics.push_back(
          detail::LinuxPkgConfigDiagnostic(package.name, package.minimum_version, version_result, requirement_exit_code)
      );
    }
    return diagnostics;
  }

  std::vector<SetupAction> PlanSetup(std::span<const EnvironmentDiagnostic> diagnostics) const override {
    const bool missing_tool =
        std::any_of(diagnostics.begin(), diagnostics.end(), [](const EnvironmentDiagnostic& diagnostic) {
          return diagnostic.status == EnvironmentDiagnosticStatus::Missing && !diagnostic.id.starts_with("pkg:") &&
                 !diagnostic.id.starts_with("cmake:");
        });
    const bool missing_package =
        std::any_of(diagnostics.begin(), diagnostics.end(), [](const EnvironmentDiagnostic& diagnostic) {
          return diagnostic.status == EnvironmentDiagnosticStatus::Missing &&
                 (diagnostic.id.starts_with("pkg:") || diagnostic.id.starts_with("cmake:"));
        });
    std::vector<SetupAction> actions;
    if (missing_tool) {
      actions.push_back({
          "Install CMake and pkg-config with the host distribution package manager",
          std::nullopt,
      });
    }
    if (missing_package) {
      actions.push_back({
          "Install the required SDL3, SDL3_image, SDL3_ttf, GIO, and libsoup 3 development packages",
          std::nullopt,
      });
    }
    return actions;
  }

  std::vector<GeneratedFile> CreateShell(const ProjectTemplateContext& context) const override {
    return RenderTemplateTree("platform/desktop/app", context);
  }

  std::vector<GeneratedFile> CreateLibraryPackage(const ProjectTemplateContext&) const override {
    return {{"src/.gitkeep", {}}};
  }

  std::vector<Diagnostic> Diagnose(const std::filesystem::path& shell_root) const override {
    static constexpr std::array required{std::string_view{"main.cpp"}};
    return detail::ValidateRequiredFiles(shell_root, required);
  }

  std::vector<ProcessCommand> BuildCommands(const PlatformCommandContext& context) const override {
    return detail::DesktopBuildCommands(context);
  }

  std::vector<ProcessCommand> RunCommands(const PlatformCommandContext& context) const override {
    const std::string artifact = detail::JsonString(detail::ReadFile(detail::AppIntegrationPlan(context)), "artifact");
    if (!std::filesystem::is_regular_file(artifact)) {
      throw std::runtime_error("HuxerUI Linux build artifact is missing: " + artifact);
    }
    return {{artifact, {}, std::filesystem::path(artifact).parent_path()}};
  }

  std::vector<PackageArtifact> PackageArtifacts(const PlatformCommandContext& context) const override {
    const std::filesystem::path artifact =
        detail::JsonString(detail::ReadFile(detail::AppIntegrationPlan(context)), "artifact");
    std::vector<PackageArtifact> artifacts{{artifact, artifact.filename()}};
    const std::filesystem::path resources = artifact.parent_path() / (artifact.stem().string() + ".resources");
    if (std::filesystem::is_directory(resources)) {
      artifacts.push_back({resources, resources.filename()});
    }
    return artifacts;
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
