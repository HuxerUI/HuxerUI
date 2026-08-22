#include "platform.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <stdexcept>

#include "template.h"

namespace huxerui::cli {
namespace {

class MacOSDriver final : public PlatformDriver {
public:
  std::string_view Id() const noexcept override {
    return "macos";
  }

  bool SupportsCurrentHost() const noexcept override {
    return CurrentHostId() == "macos";
  }

  std::span<const std::string_view> RequiredTools() const noexcept override {
    static constexpr std::array tools{std::string_view{"xcodebuild"}};
    return tools;
  }

  std::vector<SetupAction> PlanSetup(std::span<const EnvironmentDiagnostic> diagnostics) const override {
    return std::any_of(
               diagnostics.begin(),
               diagnostics.end(),
               [](const EnvironmentDiagnostic& diagnostic) {
                 return diagnostic.status == EnvironmentDiagnosticStatus::Missing;
               }
           )
               ? std::vector<SetupAction>{{"Install Xcode and select it with xcode-select", std::nullopt}}
               : std::vector<SetupAction>{};
  }

  std::vector<GeneratedFile> CreateShell(const ProjectTemplateContext& context) const override {
    std::vector<GeneratedFile> files = RenderTemplateTree("platform/desktop/app", context);
    std::vector<GeneratedFile> platform_files = RenderTemplateTree("platform/macos/app", context);
    files.insert(
        files.end(),
        std::make_move_iterator(platform_files.begin()),
        std::make_move_iterator(platform_files.end())
    );
    return files;
  }

  std::vector<Diagnostic> Diagnose(const std::filesystem::path& shell_root) const override {
    static constexpr std::array required{
        std::string_view{"main.cpp"},
        std::string_view{"huxerui.cmake"},
        std::string_view{"Info.plist.in"},
    };
    return detail::ValidateRequiredFiles(shell_root, required);
  }

  std::vector<ProcessCommand> BuildCommands(const PlatformCommandContext& context) const override {
    return detail::DesktopBuildCommands(context);
  }

  std::vector<ProcessCommand> RunCommands(const PlatformCommandContext& context) const override {
    const std::string plan = detail::ReadFile(detail::AppIntegrationPlan(context));
    const std::string bundle = detail::JsonString(plan, "bundle");
    if (!std::filesystem::is_directory(bundle)) {
      throw std::runtime_error("macOS application bundle is missing: " + bundle);
    }
    return {{"open", {bundle}, context.project_root}};
  }

  std::vector<PackageArtifact> PackageArtifacts(const PlatformCommandContext& context) const override {
    const std::filesystem::path bundle =
        detail::JsonString(detail::ReadFile(detail::AppIntegrationPlan(context)), "bundle");
    return {{bundle, bundle.filename()}};
  }
};

} // namespace

namespace detail {

const PlatformDriver& MacOSPlatformDriver() noexcept {
  static const MacOSDriver driver;
  return driver;
}

} // namespace detail
} // namespace huxerui::cli
