#include "platform.h"

#include "toolchain.h"

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

  std::span<const std::string_view> TargetAbis() const noexcept override {
    static constexpr std::array abis{std::string_view{"arm64"}, std::string_view{"x86_64"}};
    return abis;
  }

  bool SupportsCurrentHost() const noexcept override {
    return CurrentHostId() == "macos";
  }

  std::vector<EnvironmentDiagnostic> DiagnoseEnvironment(const EnvironmentRequest& request) const override {
    if (!SupportsCurrentHost() || request.operation == EnvironmentOperation::Devices) {
      return PlatformDriver::DiagnoseEnvironment(request);
    }
    const auto generator =
        request.generator.empty() ? ReadEnvironmentVariable("CMAKE_GENERATOR").value_or("") : request.generator;
    std::vector<EnvironmentDiagnostic> checks;
    auto sdk = detail::CheckTool("macos_sdk", "macOS SDK", {});
    const auto selected = RunProcessCapture({"/usr/bin/xcode-select", {"-p"}, {}});
    if (selected.exit_code == 0) {
      const auto result = RunProcessCapture({"/usr/bin/xcrun", {"--sdk", "macosx", "--show-sdk-path"}, {}});
      const auto end = result.output.find_last_not_of("\r\n");
      sdk.path = result.output.substr(0, end + 1);
      sdk.detail = sdk.path.string();
      if (result.exit_code == 0 && std::filesystem::is_directory(sdk.path))
        sdk.status = EnvironmentDiagnosticStatus::Ready;
      const auto compiler = RunProcessCapture({"/usr/bin/xcrun", {"--sdk", "macosx", "--find", "clang++"}, {}});
      checks.push_back(
          {compiler.exit_code == 0 ? EnvironmentDiagnosticStatus::Ready : EnvironmentDiagnosticStatus::Missing,
           "compiler", "Apple C++ compiler", compiler.output});
    }
    checks.push_back(std::move(sdk));
    // Command Line Tools suffice for the ordinary CMake build with a non-Xcode generator.
    if (generator == "Xcode" || selected.output.find("/Contents/Developer") != std::string::npos) {
      auto xcode = detail::CheckTool("xcodebuild", "Xcode", "/usr/bin/xcodebuild", "15.0", {}, "-version");
      if (xcode.status == EnvironmentDiagnosticStatus::Ready) {
        const auto initialized = RunProcessCapture({"/usr/bin/xcodebuild", {"-checkFirstLaunchStatus"}, {}});
        checks.push_back(
            {initialized.exit_code == 0 ? EnvironmentDiagnosticStatus::Ready : EnvironmentDiagnosticStatus::Missing,
             "xcode_initialization", "Xcode first-launch setup", initialized.output});
      }
      checks.push_back(std::move(xcode));
    }
    if (request.operation == EnvironmentOperation::Package) {
      checks.push_back(
          detail::CheckTool("hdiutil", "hdiutil", FindExecutable("hdiutil").value_or(std::filesystem::path{})));
    }
    return checks;
  }

  std::vector<SetupAction> PlanSetup(std::span<const EnvironmentDiagnostic> checks,
                                     const EnvironmentRequest&) const override {
    if (detail::NeedsSetup(checks, "macos_sdk") || detail::NeedsSetup(checks, "xcodebuild") ||
        detail::NeedsSetup(checks, "compiler")) {
      return {{"Install and select Xcode or the Command Line Tools from https://developer.apple.com/xcode/",
               std::nullopt, "select_apple_tools"}};
    }
    if (detail::NeedsSetup(checks, "xcode_initialization")) {
      return {{"Open the selected Xcode and complete its first-launch setup and license",
               std::nullopt,
               "initialize_xcode",
               {},
               {"xcode_initialization"}}};
    }
    return PlatformDriver::PlanSetup(checks);
  }

  std::vector<GeneratedFile> CreateShell(const ProjectTemplateContext& context) const override {
    std::vector<GeneratedFile> files = RenderTemplateTree("platform/desktop/app", context);
    std::vector<GeneratedFile> platform_files = RenderTemplateTree("platform/macos/app", context);
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
        std::string_view{"Info.plist.in"},
        std::string_view{"AppIcon.icns"},
    };
    return detail::ValidateRequiredFiles(shell_root, required);
  }

  void ValidateBuildAbis(const PlatformCommandContext& context) const override {
    if (context.abis.size() > 1 && !std::filesystem::is_regular_file(context.huxerui_home / "CMakeLists.txt")) {
      throw std::invalid_argument(
          "HuxerUI universal macOS builds require a source checkout via --sdk and universal dependencies"
      );
    }
  }

  std::vector<ProcessCommand> BuildCommands(const PlatformCommandContext& context) const override {
    if (context.package && SupportsCurrentHost() && !FindExecutable("hdiutil")) {
      throw std::runtime_error("HuxerUI macOS packaging requires hdiutil on PATH");
    }
    auto commands = detail::DesktopBuildCommands(context);
    if (!context.abis.empty()) {
      commands.front().arguments.push_back("-DCMAKE_OSX_ARCHITECTURES=" + detail::JoinTargetAbis(context.abis, ';'));
    }
    return commands;
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
