#include "platform.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <stdexcept>

#include "template.h"

namespace huxerui::cli {
namespace {

std::filesystem::path MacOSPackageArtifact(const PlatformCommandContext& context, std::string_view plan) {
  const std::string target = detail::JsonString(plan, "target");
  const std::string version = detail::JsonString(plan, "version");
  return context.project_root / ".huxerui/package/macos" / context.profile / (target + "-" + version + ".dmg");
}

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
    return std::any_of(diagnostics.begin(), diagnostics.end(), [](const EnvironmentDiagnostic& diagnostic) {
             return diagnostic.status == EnvironmentDiagnosticStatus::Missing;
           })
               ? std::vector<SetupAction>{{"Install Xcode and select it with xcode-select", std::nullopt}}
               : std::vector<SetupAction>{};
  }

  std::vector<GeneratedFile> CreateShell(const ProjectTemplateContext& context) const override {
    std::vector<GeneratedFile> files = RenderTemplateTree("platform/desktop/app", context);
    std::vector<GeneratedFile> platform_files = RenderTemplateTree("platform/macos/app", context);
    files.insert(files.end(), std::make_move_iterator(platform_files.begin()),
                 std::make_move_iterator(platform_files.end()));
    return files;
  }

  std::vector<GeneratedFile> CreateLibraryPackage(const LibraryTemplateContext&) const override {
    return {{"src/.gitkeep", {}}};
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
    if (context.package && SupportsCurrentHost() && !FindExecutable("hdiutil")) {
      throw std::runtime_error("HuxerUI macOS packaging requires hdiutil on PATH");
    }
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
    const std::string plan = detail::ReadFile(detail::AppIntegrationPlan(context));
    const std::filesystem::path artifact = MacOSPackageArtifact(context, plan);
    return {{artifact, artifact.filename()}};
  }

  std::vector<ProcessCommand> PackageCommands(const PlatformCommandContext& context) const override {
    const std::string plan = detail::ReadFile(detail::AppIntegrationPlan(context));
    const std::string name = detail::JsonString(plan, "name");
    const std::string install_component = detail::JsonString(plan, "installComponent");
    const std::filesystem::path artifact = MacOSPackageArtifact(context, plan);
    const std::filesystem::path root = artifact.parent_path();
    const std::filesystem::path staging = root / "staging";
    std::vector<ProcessCommand> commands =
        detail::DesktopPackageStageCommands(context, staging, install_component);
    commands.push_back({"cmake", {"-E", "create_symlink", "/Applications", (staging / "Applications").string()}, root});
    commands.push_back(
        {"hdiutil",
         {"create", "-volname", name, "-srcfolder", staging.string(), "-format", "UDZO", "-ov", artifact.string()},
         root});
    return commands;
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
