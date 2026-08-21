#include "platform.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <iterator>
#include <stdexcept>
#include <utility>

#include "template.h"

namespace huxerui::cli {
namespace {

std::filesystem::path VisualStudioLocator() {
  const std::optional<std::string> program_files = ReadEnvironmentVariable("ProgramFiles(x86)");
  if (!program_files) {
    return {};
  }
  const std::filesystem::path locator =
      std::filesystem::path(*program_files) / "Microsoft Visual Studio/Installer/vswhere.exe";
  return std::filesystem::is_regular_file(locator) ? locator : std::filesystem::path{};
}

class WindowsDriver final : public PlatformDriver {
public:
  std::string_view Id() const noexcept override {
    return "windows";
  }

  bool SupportsCurrentHost() const noexcept override {
    return CurrentHostId() == "windows";
  }

  std::span<const std::string_view> RequiredTools() const noexcept override {
    return {};
  }

  std::vector<EnvironmentDiagnostic> DiagnoseEnvironment() const override {
    if (!SupportsCurrentHost()) {
      return PlatformDriver::DiagnoseEnvironment();
    }
    const std::filesystem::path locator = VisualStudioLocator();
    if (locator.empty()) {
      return {{EnvironmentDiagnosticStatus::Missing, "msvc", "MSVC C++ build tools", {}}};
    }
    const ProcessResult result = RunProcessCapture({
        locator.string(),
        {"-latest",
         "-products",
         "*",
         "-requires",
         "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
         "-property",
         "installationPath"},
        {},
    });
    std::string installation = result.output;
    while (!installation.empty() && std::isspace(static_cast<unsigned char>(installation.back()))) {
      installation.pop_back();
    }
    return {{
        result.exit_code == 0 && !installation.empty() ? EnvironmentDiagnosticStatus::Ready
                                                       : EnvironmentDiagnosticStatus::Missing,
        "msvc",
        "MSVC C++ build tools",
        std::move(installation),
    }};
  }

  std::vector<SetupAction> PlanSetup(std::span<const EnvironmentDiagnostic> diagnostics) const override {
    return std::any_of(
               diagnostics.begin(),
               diagnostics.end(),
               [](const EnvironmentDiagnostic& diagnostic) {
                 return diagnostic.id == "msvc" && diagnostic.status == EnvironmentDiagnosticStatus::Missing;
               }
           )
               ? std::vector<SetupAction>{{
                     "Install Visual Studio Build Tools with the Desktop development with C++ workload",
                     std::nullopt,
                 }}
               : std::vector<SetupAction>{};
  }

  std::vector<GeneratedFile> CreateShell(const ProjectTemplateContext& context) const override {
    std::vector<GeneratedFile> files = RenderTemplateTree("platform/desktop/app", context);
    std::vector<GeneratedFile> platform_files = RenderTemplateTree("platform/windows/app", context);
    files.insert(
        files.end(),
        std::make_move_iterator(platform_files.begin()),
        std::make_move_iterator(platform_files.end())
    );
    return files;
  }

  std::vector<GeneratedFile> CreateLibraryPackage(const ProjectTemplateContext&) const override {
    return {{"src/.gitkeep", {}}};
  }

  std::vector<Diagnostic> Diagnose(const std::filesystem::path& shell_root) const override {
    static constexpr std::array required{
        std::string_view{"main.cpp"},
        std::string_view{"huxerui.cmake"},
        std::string_view{"app.manifest"},
    };
    return detail::ValidateRequiredFiles(shell_root, required);
  }

  std::vector<ProcessCommand> BuildCommands(const PlatformCommandContext& context) const override {
    return detail::DesktopBuildCommands(context);
  }

  std::vector<ProcessCommand> RunCommands(const PlatformCommandContext& context) const override {
    const std::string artifact = detail::JsonString(detail::ReadFile(detail::AppIntegrationPlan(context)), "artifact");
    if (!std::filesystem::is_regular_file(artifact)) {
      throw std::runtime_error("Windows build artifact is missing: " + artifact);
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
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(artifact.parent_path())) {
      if (entry.is_regular_file() && entry.path().extension() == ".dll") {
        artifacts.push_back({entry.path(), entry.path().filename()});
      }
    }
    std::ranges::sort(artifacts.begin() + 1, artifacts.end(), {}, &PackageArtifact::destination);
    return artifacts;
  }
};

} // namespace

namespace detail {

const PlatformDriver& WindowsPlatformDriver() noexcept {
  static const WindowsDriver driver;
  return driver;
}

} // namespace detail
} // namespace huxerui::cli
