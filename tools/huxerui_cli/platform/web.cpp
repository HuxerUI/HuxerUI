#include "platform.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

#include "template.h"

namespace huxerui::cli {
namespace {

constexpr std::string_view emscripten_version = "4.0.19";
constexpr std::string_view termux_web_server_script =
    "import http.server,signal,subprocess,sys,urllib.parse;"
    "server=http.server.ThreadingHTTPServer(('127.0.0.1',0),http.server.SimpleHTTPRequestHandler);"
    "url='http://127.0.0.1:{}/{}'.format(server.server_port,urllib.parse.quote(sys.argv[1]));"
    "print('Serving HuxerUI Web at '+url,flush=True);"
    "subprocess.run(['termux-open',url],check=True);"
    "signal.signal(signal.SIGINT,lambda *_:sys.exit(0));"
    "server.serve_forever()";

std::string ResolveCMakeGenerator(const PlatformCommandContext& context) {
  if (!context.cmake_generator.empty()) {
    return context.cmake_generator;
  }

  const std::filesystem::path cache = context.build_directory / "CMakeCache.txt";
  if (!std::filesystem::is_regular_file(cache)) {
    return ReadEnvironmentVariable("CMAKE_GENERATOR").value_or(std::string{});
  }

  constexpr std::string_view marker = "CMAKE_GENERATOR:INTERNAL=";
  const std::string content = detail::ReadFile(cache);
  const std::size_t start = content.find(marker);
  if (start == std::string::npos) {
    throw std::runtime_error("HuxerUI Web CMake cache is missing CMAKE_GENERATOR: " + cache.string());
  }
  const std::size_t value_start = start + marker.size();
  const std::size_t end = content.find_first_of("\r\n", value_start);
  const std::string generator =
      content.substr(value_start, end == std::string::npos ? std::string::npos : end - value_start);
  if (generator.empty()) {
    throw std::runtime_error("HuxerUI Web CMake cache has an empty CMAKE_GENERATOR: " + cache.string());
  }
  return generator;
}

class WebDriver final : public PlatformDriver {
public:
  std::string_view Id() const noexcept override {
    return "web";
  }

  bool SupportsCurrentHost() const noexcept override {
    return CurrentHostId() == "windows" || CurrentHostId() == "macos" || CurrentHostId() == "linux" ||
           CurrentHostId() == "android";
  }

  std::span<const std::string_view> RequiredTools() const noexcept override {
    static constexpr std::array desktop_tools{
        std::string_view{"emcmake"},
        std::string_view{"emcc"},
        std::string_view{"emrun"},
    };
    static constexpr std::array android_tools{
        std::string_view{"emcmake"},
        std::string_view{"emcc"},
        std::string_view{"python"},
        std::string_view{"termux-open"},
    };
    return CurrentHostId() == "android" ? std::span<const std::string_view>(android_tools)
                                        : std::span<const std::string_view>(desktop_tools);
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
    if (CurrentHostId() == "android") {
      std::vector<SetupAction> actions;
      const bool needs_emscripten = std::any_of(
          diagnostics.begin(), diagnostics.end(), [](const EnvironmentDiagnostic& diagnostic) {
            return diagnostic.status == EnvironmentDiagnosticStatus::Missing &&
                   (diagnostic.id == "emcmake" || diagnostic.id == "emcc");
          }
      );
      if (needs_emscripten) {
        actions.push_back({
            "Install a Termux Emscripten " + std::string(emscripten_version) + " toolchain and add it to PATH",
            std::nullopt,
        });
      }
      const bool needs_python = std::any_of(
          diagnostics.begin(), diagnostics.end(), [](const EnvironmentDiagnostic& diagnostic) {
            return diagnostic.status == EnvironmentDiagnosticStatus::Missing && diagnostic.id == "python";
          }
      );
      if (needs_python) {
        actions.push_back({
            "Install Python for the Termux Web development server",
            ProcessCommand{"pkg", {"install", "-y", "python"}, {}},
        });
      }
      const bool needs_termux_tools = std::any_of(
          diagnostics.begin(), diagnostics.end(), [](const EnvironmentDiagnostic& diagnostic) {
            return diagnostic.status == EnvironmentDiagnosticStatus::Missing && diagnostic.id == "termux-open";
          }
      );
      if (needs_termux_tools) {
        actions.push_back({
            "Install Termux system integration tools",
            ProcessCommand{"pkg", {"install", "-y", "termux-tools"}, {}},
        });
      }
      return actions;
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
    const std::string generator = ResolveCMakeGenerator(context);
    if (!generator.empty()) {
      configure_arguments.insert(configure_arguments.begin() + 1, {"-G", generator});
    }
    return {
        {"emcmake", std::move(configure_arguments), context.project_root},
        {"cmake",
         {"--build", context.build_directory.string(), "--config", configuration, "--parallel"},
         context.project_root},
    };
  }

  std::vector<ProcessCommand> RunCommands(const PlatformCommandContext& context) const override {
    return detail::WebRunCommands(context, CurrentHostId());
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

std::vector<ProcessCommand> WebRunCommands(const PlatformCommandContext& context, std::string_view host_id) {
  const std::string plan = ReadFile(AppIntegrationPlan(context));
  const std::string target = JsonString(plan, "target");
  const std::filesystem::path artifact = JsonString(plan, "artifact");
  if (!std::filesystem::is_regular_file(artifact)) {
    throw std::runtime_error("Web build artifact is missing: " + artifact.string());
  }
  const std::filesystem::path entry = artifact.parent_path() / (target + ".html");
  if (!std::filesystem::is_regular_file(entry)) {
    throw std::runtime_error("Web entry file is missing: " + entry.string());
  }
  if (host_id == "android") {
    return {{
        "python",
        {"-c", std::string(termux_web_server_script), entry.filename().string()},
        artifact.parent_path(),
    }};
  }
  std::vector<std::string> arguments;
  if (host_id == "windows") {
    arguments = {"--browser", "explorer.exe"};
  }
  arguments.push_back(entry.string());
  return {{"emrun", std::move(arguments), artifact.parent_path()}};
}

const PlatformDriver& WebPlatformDriver() noexcept {
  static const WebDriver driver;
  return driver;
}

} // namespace detail
} // namespace huxerui::cli
