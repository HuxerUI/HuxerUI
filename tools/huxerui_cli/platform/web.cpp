#include "platform.h"

#include "toolchain.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

#include "template.h"

namespace huxerui::cli {
namespace {

constexpr std::string_view emscripten_version = "4.0.19";
constexpr std::string_view emsdk_version = "6.0.5";
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

std::string WebVersion(const EnvironmentRequest& request) {
  if (request.sdk_home.empty() || IsSourceHome(request.sdk_home))
    return std::string(emscripten_version);
  // The packaged archive is tied to the Emscripten version that built it.
  const auto path = request.sdk_home / "share/huxerui/platform/web";
  std::string version;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(path, error)) {
    const auto name = entry.path().filename().string();
    if (!name.starts_with("emscripten-") || !std::filesystem::is_regular_file(entry.path() / "libhuxerui.a"))
      continue;
    if (!version.empty())
      throw std::runtime_error("the selected SDK has multiple Emscripten archives: " + path.string());
    version = name.substr(std::string_view("emscripten-").size());
    if (detail::ParseToolVersion(version) != version) {
      throw std::runtime_error("the selected SDK has an invalid Emscripten archive version: " + name);
    }
  }
  if (version.empty())
    throw std::runtime_error("the selected SDK does not contain a Web archive: " + path.string());
  return version;
}

std::filesystem::path ManagedEmsdk(std::string_view version) {
  const auto root = detail::ManagedToolDirectory();
  return root.empty() ? std::filesystem::path{} : root / "emsdk" / version;
}

std::filesystem::path ManagedPython() {
  const auto root = detail::ManagedToolDirectory();
  return root.empty() ? std::filesystem::path{} : root / "python" / "3.12.10" / "x86_64" / "python.exe";
}

EnvironmentDiagnostic WebPython() {
  const auto tool = CurrentHostId() == "windows" ? "python" : "python3";
  auto executable = FindExecutable(tool).value_or(std::filesystem::path{});
  if (CurrentHostId() == "macos" && executable == "/usr/bin/python3" &&
      RunProcessCapture({"/usr/bin/xcode-select", {"-p"}, {}}).exit_code != 0)
    executable.clear();
  auto check = detail::CheckTool("python", "Python", executable, "3.8");
  if (CurrentHostId() == "macos" && check.status != EnvironmentDiagnosticStatus::Ready) {
    const std::filesystem::path framework = "/Library/Frameworks/Python.framework/Versions/3.12/bin/python3";
    if (std::filesystem::is_regular_file(framework))
      return detail::CheckTool("python", "Python", framework, "3.8");
  }
  if (check.status == EnvironmentDiagnosticStatus::Ready || CurrentHostId() != "windows")
    return check;
  const auto managed = ManagedPython();
  if (!managed.empty() && std::filesystem::is_regular_file(managed)) {
    return detail::CheckTool("python", "Python", managed, "3.8");
  }
  return check;
}

SetupAction SetupPython() {
  const auto host = CurrentHostId();
  if (host == "windows" && !ManagedPython().empty()) {
    return detail::InstallTool("install_python", "Install Python 3.12 from python.org",
                               {"https://www.python.org/ftp/python/3.12.10/python-3.12.10-embed-amd64.zip",
                                "4acbed6dd1c744b0376e3b1cf57ce906f9dc9e95e68824584c8099a63025a3c3", "",
                                ManagedPython().parent_path()},
                               {}, {"python"});
  }
  if (host == "macos" && !detail::ManagedToolDirectory().empty()) {
    auto action = detail::InstallTool("install_python", "Install the official macOS Python 3.12 package",
                                      {"https://www.python.org/ftp/python/3.12.10/python-3.12.10-macos11.pkg",
                                       "8373e58da4ea146b3eb1c1f9834f19a319440b6b679b06050b1f9ee3237aa8e4", "",
                                       detail::ManagedToolDirectory() / "python/3.12.10"},
                                      {}, {"python"});
    action.interactive = true;
    return action;
  }
  if (detail::IsUbuntuNoble()) {
    return {"Install Python with apt",
            std::nullopt,
            "install_python",
            {},
            {"python"},
            [] { return detail::InstallUbuntuPackages(std::array{std::string("python3")}); },
            true};
  }
  return {"Install Python 3 from https://www.python.org/downloads/ and make python3 available",
          std::nullopt,
          "install_python",
          {},
          {"python"}};
}

ProcessEnvironment PythonEnvironment() {
  ProcessEnvironment environment;
  const auto python = WebPython();
  if (python.status == EnvironmentDiagnosticStatus::Ready)
    detail::PrependPath(environment, python.path.parent_path());
  return environment;
}

std::filesystem::path EmsdkCommand(const std::filesystem::path& root) {
  return root / (CurrentHostId() == "windows" ? "emsdk.bat" : "emsdk");
}

EnvironmentDiagnostic EmscriptenCheck(std::string_view version, const ProcessEnvironment& environment) {
  const auto path = FindExecutable("emcc", environment).value_or(std::filesystem::path{});
  auto check = detail::CheckTool("emcc", "Emscripten", path, version, environment);
  if (check.version != version)
    check.status = EnvironmentDiagnosticStatus::Missing;
  return check;
}

ProcessEnvironment LoadEmsdk(const std::filesystem::path& root) {
  if (root.empty() || !std::filesystem::is_regular_file(root / ".emscripten"))
    return {};

  ProcessEnvironment environment = PythonEnvironment();
  environment.emplace_back("EMSDK_QUIET", "1");
  ProcessResult result;
  if (CurrentHostId() == "windows") {
    // Calling by name from the owning directory keeps cmd.exe quoting independent of the install path.
    result =
        RunProcessCapture({"cmd.exe", {"/d", "/v:off", "/c", "call emsdk_env.bat >nul && set"}, root, environment});
  } else {
    result = RunProcessCapture({"/bin/bash", {"-c", "source ./emsdk_env.sh >/dev/null && env"}, root, environment});
  }
  if (result.exit_code != 0)
    return {};
  // Carry the activation changes into build children without replacing their entire inherited environment.
  for (auto& entry : detail::ParseProcessEnvironment(result.output)) {
    const auto& key = entry.first;
    if (key == "PATH" || key == "EMSDK" || key == "EM_CONFIG" || key == "EM_CACHE" || key == "SSL_CERT_FILE" ||
        key.starts_with("EMSDK_")) {
      environment.push_back(std::move(entry));
    }
  }
  return environment;
}

ProcessEnvironment WebEnvironment(std::string_view version) {
  if (CurrentHostId() == "android" || EmscriptenCheck(version, {}).status == EnvironmentDiagnosticStatus::Ready)
    return {};
  const auto active = ReadEnvironmentVariable("EMSDK").value_or("");
  const auto emsdk = FindExecutable("emsdk").value_or(std::filesystem::path{});
  for (const auto& root : {std::filesystem::path(active), emsdk.parent_path(), ManagedEmsdk(version)}) {
    auto environment = LoadEmsdk(root);
    if (!environment.empty() && EmscriptenCheck(version, environment).status == EnvironmentDiagnosticStatus::Ready) {
      return environment;
    }
  }
  return {};
}

class WebDriver final : public PlatformDriver {
public:
  std::string_view Id() const noexcept override {
    return "web";
  }

  std::span<const std::string_view> TargetAbis() const noexcept override {
    static constexpr std::array abis{std::string_view{"wasm32"}};
    return abis;
  }

  bool SupportsCurrentHost() const noexcept override {
    return CurrentHostId() == "windows" || CurrentHostId() == "macos" || CurrentHostId() == "linux" ||
           CurrentHostId() == "android";
  }

  ProcessEnvironment ResolveEnvironment(const EnvironmentRequest& request) const override {
    const auto version = WebVersion(request);
    auto environment = WebEnvironment(version);
    const auto compiler = EmscriptenCheck(version, environment);
    if (compiler.status != EnvironmentDiagnosticStatus::Ready) {
      throw std::runtime_error("the selected HuxerUI SDK requires Emscripten " + version +
                               "; run 'huxerui doctor web'");
    }
    return environment;
  }

  std::vector<EnvironmentDiagnostic> DiagnoseEnvironment(const EnvironmentRequest& request) const override {
    if (!SupportsCurrentHost() || request.operation == EnvironmentOperation::Devices) {
      return PlatformDriver::DiagnoseEnvironment(request);
    }
    const auto version = WebVersion(request);
    const auto environment = WebEnvironment(version);
    std::vector<EnvironmentDiagnostic> checks;
    checks.push_back(EmscriptenCheck(version, environment));
    checks.push_back(detail::CheckTool("emcmake", "emcmake",
                                       FindExecutable("emcmake", environment).value_or(std::filesystem::path{})));
    if (request.operation == EnvironmentOperation::Run && CurrentHostId() != "android") {
      checks.push_back(
          detail::CheckTool("emrun", "emrun", FindExecutable("emrun", environment).value_or(std::filesystem::path{})));
    }
    if (request.operation == EnvironmentOperation::Run && CurrentHostId() == "android") {
      checks.push_back(detail::CheckTool("termux-open", "Termux system integration",
                                         FindExecutable("termux-open").value_or(std::filesystem::path{})));
    }
    if (CurrentHostId() != "android") {
      auto python = WebPython();
      python.required = false;
      checks.push_back(std::move(python));
      const auto root = ManagedEmsdk(version);
      auto emsdk = detail::CheckTool("emsdk", "Managed Emscripten SDK",
                                     !root.empty() && std::filesystem::is_regular_file(EmsdkCommand(root))
                                         ? EmsdkCommand(root)
                                         : std::filesystem::path{});
      emsdk.required = false;
      checks.push_back(std::move(emsdk));
    }
    return checks;
  }

  std::vector<SetupAction> PlanSetup(std::span<const EnvironmentDiagnostic> checks,
                                     const EnvironmentRequest& request) const override {
    if (!detail::NeedsSetup(checks, "emcc") && !detail::NeedsSetup(checks, "emcmake") &&
        !detail::NeedsSetup(checks, "emrun") && !detail::NeedsSetup(checks, "termux-open"))
      return {};
    const auto version = WebVersion(request);
    std::vector<SetupAction> actions;
    if (CurrentHostId() == "android") {
      if (detail::NeedsSetup(checks, "emcc") || detail::NeedsSetup(checks, "emcmake")) {
        actions.push_back({"Select a Termux Emscripten " + version + " toolchain", std::nullopt, "select_emscripten"});
      }
      if (detail::NeedsSetup(checks, "termux-open")) {
        actions.push_back({"Install Termux system integration tools",
                           ProcessCommand{"pkg", {"install", "termux-tools"}, {}},
                           "install_termux_tools",
                           {},
                           {"termux-open"},
                           {},
                           true});
      }
      return actions;
    }
    const auto root = ManagedEmsdk(version);
    if (root.empty())
      return {{"Set a user data directory for the managed emsdk", std::nullopt, "select_emsdk_root"}};
    if (detail::NeedsSetup(checks, "python"))
      actions.push_back(SetupPython());
    if (detail::NeedsSetup(checks, "emsdk")) {
      actions.push_back(detail::InstallTool(
          "install_emsdk", "Install official emsdk",
          {"https://codeload.github.com/emscripten-core/emsdk/tar.gz/refs/tags/" + std::string(emsdk_version),
           "75ad24a079bcbbe3c1bdb614101eab490d94346668ab3a8e250e7e53b4df565b", "emsdk-" + std::string(emsdk_version),
           root},
          {}, {"emsdk"}));
    }
    actions.push_back(
        {"Install and activate Emscripten " + version,
         std::nullopt,
         "install_emscripten",
         {"python", "emsdk"},
         {"emcc", "emcmake"},
         [root, version] {
           const auto executable = EmsdkCommand(root).string();
           const auto environment = PythonEnvironment();
           const int result = RunProcess({executable, {"install", version}, root, environment});
           return result == 0 ? RunProcess({executable, {"activate", version}, root, environment}) : result;
         }});
    return actions;
  }

  std::vector<GeneratedFile> CreateShell(const ProjectTemplateContext& context) const override {
    return RenderTemplateTree("platform/web/app", context);
  }

  std::vector<Diagnostic> Diagnose(const std::filesystem::path& shell_root) const override {
    static constexpr std::array required{
        std::string_view{"huxerui.cmake"},
        std::string_view{"index.html.in"},
        std::string_view{"favicon.svg"},
        std::string_view{"apple-touch-icon.png"},
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
        "-DHUXERUI_HOME=" + context.huxerui_home.string(),
    };
    const std::string generator = ResolveCMakeGenerator(context);
    if (context.optimization) {
      configure_arguments.push_back("-DHUXERUI_OPTIMIZATION=" + *context.optimization);
    }
    if (context.lto) {
      configure_arguments.push_back(std::string("-DHUXERUI_ENABLE_LTO=") + (*context.lto ? "ON" : "OFF"));
    }
    if (context.profiling) {
      configure_arguments.push_back("-DHUXERUI_ENABLE_PROFILING=ON");
    }
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
