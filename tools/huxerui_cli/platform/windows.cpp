#include "platform.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iterator>
#include <sstream>
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

std::filesystem::path VisualStudioInstallation() {
  const std::filesystem::path locator = VisualStudioLocator();
  if (locator.empty()) {
    return {};
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
  const std::filesystem::path path =
      result.exit_code == 0 ? std::filesystem::path(installation) : std::filesystem::path{};
  return std::filesystem::is_regular_file(path / "Common7/Tools/VsDevCmd.bat") ? path : std::filesystem::path{};
}

void ImportMsvcEnvironment(const std::filesystem::path& installation) {
  const std::filesystem::path developer_command = installation / "Common7/Tools/VsDevCmd.bat";
  if (!std::filesystem::is_regular_file(developer_command)) {
    throw std::runtime_error("Visual Studio developer environment is missing: " + developer_command.string());
  }

  const std::filesystem::path script =
      std::filesystem::temp_directory_path() /
      ("huxerui-msvc-environment-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
       ".cmd");
  {
    std::ofstream stream(script, std::ios::binary | std::ios::trunc);
    if (!stream) {
      throw std::runtime_error("cannot create temporary MSVC environment script");
    }
    stream << "@echo off\r\n"
           << "chcp 65001 >nul\r\n"
           << "call \"" << developer_command.string() << "\" -arch=x64 -host_arch=x64 >nul\r\n"
           << "if errorlevel 1 exit /b %errorlevel%\r\n"
           << "set\r\n";
  }

  ProcessResult result;
  try {
    result = RunProcessCapture({script.string(), {}, script.parent_path()});
  } catch (...) {
    std::error_code error;
    std::filesystem::remove(script, error);
    throw;
  }
  std::error_code error;
  std::filesystem::remove(script, error);
  if (result.exit_code != 0) {
    throw std::runtime_error("cannot initialize the Visual Studio developer environment");
  }

  std::istringstream output(result.output);
  std::string line;
  while (std::getline(output, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const std::size_t separator = line.find('=');
    if (separator == std::string::npos || separator == 0) {
      continue;
    }
    SetProcessEnvironmentVariable(line.substr(0, separator), line.substr(separator + 1));
  }
  if (!FindExecutable("cl")) {
    throw std::runtime_error("Visual Studio developer environment did not provide the MSVC compiler");
  }
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
    const std::filesystem::path installation = VisualStudioInstallation();
    return {{
        installation.empty() ? EnvironmentDiagnosticStatus::Missing : EnvironmentDiagnosticStatus::Ready,
        "msvc",
        "MSVC C++ build tools",
        installation.string(),
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

  void PrepareBuildEnvironment() const override {
    const std::filesystem::path installation = VisualStudioInstallation();
    if (installation.empty()) {
      throw std::runtime_error("HuxerUI cannot find Visual Studio with the MSVC C++ x64 tools");
    }
    ImportMsvcEnvironment(installation);
  }

  std::vector<ProcessCommand> BuildCommands(const PlatformCommandContext& context) const override {
    std::vector<ProcessCommand> commands = detail::DesktopBuildCommands(context);
    commands.front().arguments.push_back("-DCMAKE_CXX_COMPILER=cl");
    return commands;
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
