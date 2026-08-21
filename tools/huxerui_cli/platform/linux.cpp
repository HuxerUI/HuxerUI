#include "platform.h"

#include <array>
#include <cctype>
#include <stdexcept>
#include <utility>

#include "template.h"

namespace huxerui::cli {
namespace {

class LinuxDriver final : public PlatformDriver {
public:
  std::string_view Id() const noexcept override {
    return "linux";
  }

  bool SupportsCurrentHost() const noexcept override {
    return CurrentHostId() == "linux";
  }

  std::span<const std::string_view> RequiredTools() const noexcept override {
    static constexpr std::array tools{
        std::string_view{"pkg-config"},
        std::string_view{"meson"},
        std::string_view{"ninja"},
        std::string_view{"gperf"},
        std::string_view{"git"},
    };
    return tools;
  }

  std::vector<EnvironmentDiagnostic> DiagnoseEnvironment() const override {
    std::vector<EnvironmentDiagnostic> diagnostics = PlatformDriver::DiagnoseEnvironment();
    if (!SupportsCurrentHost() || !FindExecutable("pkg-config")) {
      return diagnostics;
    }
    static constexpr std::array packages{
        std::string_view{"x11"},
        std::string_view{"xext"},
        std::string_view{"xkbcommon"},
        std::string_view{"xrandr"},
        std::string_view{"egl"},
        std::string_view{"glesv2"},
        std::string_view{"gio-2.0"},
        std::string_view{"libsoup-3.0"},
    };
    for (const std::string_view package : packages) {
      const ProcessResult result = RunProcessCapture({"pkg-config", {"--modversion", std::string(package)}, {}});
      std::string version = result.output;
      while (!version.empty() && std::isspace(static_cast<unsigned char>(version.back()))) {
        version.pop_back();
      }
      diagnostics.push_back({
          result.exit_code == 0 ? EnvironmentDiagnosticStatus::Ready : EnvironmentDiagnosticStatus::Missing,
          "pkg:" + std::string(package),
          std::string(package) + " development package",
          result.exit_code == 0 ? std::move(version) : std::string{},
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
          "Install pkg-config, Meson, Ninja, gperf, and Git with the host distribution package manager",
          std::nullopt,
      });
    }
    if (missing_package) {
      actions.push_back({
          "Install the required X11, xkbcommon, EGL, GLES2, GIO, and libsoup 3 development packages",
          std::nullopt,
      });
    }
    return actions;
  }

  std::vector<GeneratedFile> CreateShell(const ProjectTemplateContext& context) const override {
    return RenderTemplateTree("platform/desktop/app", context);
  }

  std::vector<GeneratedFile> CreateModulePackage(const ProjectTemplateContext&) const override {
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
