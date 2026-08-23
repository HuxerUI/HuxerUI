#include "platform.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

#include "template.h"

namespace huxerui::cli {
namespace {

constexpr std::string_view emscripten_version = "4.0.19";

class WebDriver final : public PlatformDriver {
public:
  std::string_view Id() const noexcept override {
    return "web";
  }

  bool SupportsCurrentHost() const noexcept override {
    return CurrentHostId() == "windows" || CurrentHostId() == "macos" || CurrentHostId() == "linux";
  }

  std::span<const std::string_view> RequiredTools() const noexcept override {
    static constexpr std::array tools{
        std::string_view{"emcmake"},
        std::string_view{"emcc"},
        std::string_view{"emrun"},
    };
    return tools;
  }

  std::vector<EnvironmentDiagnostic> DiagnoseEnvironment() const override {
    std::vector<EnvironmentDiagnostic> diagnostics = PlatformDriver::DiagnoseEnvironment();
    const auto emcc = std::find_if(diagnostics.begin(), diagnostics.end(), [](const EnvironmentDiagnostic& diagnostic) {
      return diagnostic.id == "emcc" && diagnostic.status == EnvironmentDiagnosticStatus::Ready;
    });
    if (emcc == diagnostics.end()) {
      return diagnostics;
    }

    const ProcessResult result = RunProcessCapture({emcc->detail, {"--version"}, {}});
    if (result.exit_code != 0 || result.output.find(emscripten_version) == std::string::npos) {
      emcc->status = EnvironmentDiagnosticStatus::Missing;
      emcc->label = "Emscripten " + std::string(emscripten_version);
      emcc->detail = result.output;
    }
    return diagnostics;
  }

  std::vector<SetupAction> PlanSetup(std::span<const EnvironmentDiagnostic> diagnostics) const override {
    if (std::none_of(diagnostics.begin(), diagnostics.end(), [](const EnvironmentDiagnostic& diagnostic) {
          return diagnostic.status == EnvironmentDiagnosticStatus::Missing;
        })) {
      return {};
    }
    const std::optional<std::filesystem::path> emsdk = FindExecutable("emsdk");
    if (!emsdk) {
      return {
          {"Install emsdk from https://emscripten.org/docs/getting_started/downloads.html and load its environment",
           std::nullopt}
      };
    }
    return {
        {"Install Emscripten " + std::string(emscripten_version),
         ProcessCommand{emsdk->string(), {"install", std::string(emscripten_version)}, {}}},
        {"Activate Emscripten " + std::string(emscripten_version),
         ProcessCommand{emsdk->string(), {"activate", std::string(emscripten_version)}, {}}},
        {"Load the activated emsdk environment in this shell", std::nullopt},
    };
  }

  std::vector<GeneratedFile> CreateShell(const ProjectTemplateContext& context) const override {
    return RenderTemplateTree("platform/web/app", context);
  }

  std::vector<Diagnostic> Diagnose(const std::filesystem::path& shell_root) const override {
    static constexpr std::array required{
        std::string_view{"huxerui.cmake"},
        std::string_view{"index.html.in"},
    };
    return detail::ValidateRequiredFiles(shell_root, required);
  }

  std::vector<ProcessCommand> BuildCommands(const PlatformCommandContext& context) const override {
    const std::string configuration = detail::ProfileConfiguration(context.profile);
    std::vector<std::string> configure_arguments{
        "cmake",
        "-S",
        context.project_root.string(),
        "-B",
        context.build_directory.string(),
        "-DCMAKE_BUILD_TYPE=" + configuration,
    };
    if (!context.cmake_generator.empty()) {
      configure_arguments.insert(configure_arguments.begin() + 1, {"-G", context.cmake_generator});
    }
    return {
        {"emcmake", std::move(configure_arguments), context.project_root},
        {"cmake",
         {"--build", context.build_directory.string(), "--config", configuration, "--parallel"},
         context.project_root},
    };
  }

  std::vector<ProcessCommand> RunCommands(const PlatformCommandContext& context) const override {
    const std::string plan = detail::ReadFile(detail::AppIntegrationPlan(context));
    const std::string target = detail::JsonString(plan, "target");
    const std::filesystem::path artifact = detail::JsonString(plan, "artifact");
    if (!std::filesystem::is_regular_file(artifact)) {
      throw std::runtime_error("Web build artifact is missing: " + artifact.string());
    }
    const std::filesystem::path entry = artifact.parent_path() / (target + ".html");
    if (!std::filesystem::is_regular_file(entry)) {
      throw std::runtime_error("Web entry file is missing: " + entry.string());
    }
    std::vector<std::string> arguments;
    if (CurrentHostId() == "windows") {
      arguments = {"--browser", "explorer.exe"};
    }
    arguments.push_back(entry.string());
    return {{"emrun", std::move(arguments), artifact.parent_path()}};
  }

  std::vector<PackageArtifact> PackageArtifacts(const PlatformCommandContext& context) const override {
    const std::string plan = detail::ReadFile(detail::AppIntegrationPlan(context));
    const std::string target = detail::JsonString(plan, "target");
    const std::filesystem::path artifact = detail::JsonString(plan, "artifact");
    std::vector<PackageArtifact> artifacts;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(artifact.parent_path())) {
      if (entry.is_regular_file() && entry.path().filename().string().starts_with(target + ".")) {
        artifacts.push_back({entry.path(), entry.path().filename()});
      }
    }
    std::ranges::sort(artifacts, {}, &PackageArtifact::destination);
    return artifacts;
  }
};

} // namespace

namespace detail {

const PlatformDriver& WebPlatformDriver() noexcept {
  static const WebDriver driver;
  return driver;
}

} // namespace detail
} // namespace huxerui::cli
