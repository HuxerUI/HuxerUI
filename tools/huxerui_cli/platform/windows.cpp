#include "platform.h"

#include "toolchain.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iterator>
#include <system_error>
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

std::filesystem::path VisualStudioInstallation(bool require_cpp = true) {
  const std::filesystem::path locator = VisualStudioLocator();
  if (locator.empty()) {
    return {};
  }
  std::vector<std::string> arguments{"-latest", "-products", "*"};
  if (require_cpp) {
    arguments.insert(arguments.end(), {"-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64"});
  }
  arguments.insert(arguments.end(), {"-property", "installationPath"});
  const ProcessResult result = RunProcessCapture({locator.string(), std::move(arguments), {}});
  std::string installation = result.output;
  while (!installation.empty() && std::isspace(static_cast<unsigned char>(installation.back()))) {
    installation.pop_back();
  }
  const std::filesystem::path path =
      result.exit_code == 0 ? std::filesystem::path(installation) : std::filesystem::path{};
  return std::filesystem::is_regular_file(path / "Common7/Tools/VsDevCmd.bat") ? path : std::filesystem::path{};
}

std::filesystem::path WixPayloadScript(const PlatformCommandContext& context) {
  const std::array candidates{
      context.huxerui_home / "cmake/HuxerUIGenerateWixPayloads.cmake",
      context.huxerui_home / "lib/cmake/HuxerUI/HuxerUIGenerateWixPayloads.cmake",
  };
  const auto script = std::find_if(candidates.begin(), candidates.end(), [](const std::filesystem::path& candidate) {
    return std::filesystem::is_regular_file(candidate);
  });
  if (script == candidates.end()) {
    throw std::runtime_error("HuxerUI SDK does not contain the Windows installer payload generator");
  }
  return *script;
}

std::filesystem::path WindowsPackagePlan(const PlatformCommandContext& context) {
  return context.build_directory / "huxerui-package/windows" / detail::ProfileConfiguration(context.profile) /
         "package.json";
}

std::filesystem::path WindowsPackageRoot(const PlatformCommandContext& context) {
  return context.project_root / ".huxerui/package/windows" / context.profile;
}

std::filesystem::path WindowsPackageArtifact(const PlatformCommandContext& context, std::string_view plan) {
  const std::string target = detail::JsonString(plan, "target");
  const std::string version = detail::JsonString(plan, "version");
  return WindowsPackageRoot(context) / (target + "-Setup-" + version + ".exe");
}

ProcessEnvironment ImportMsvcEnvironment(const std::filesystem::path& installation) {
  const std::filesystem::path developer_command = installation / "Common7/Tools/VsDevCmd.bat";
  if (!std::filesystem::is_regular_file(developer_command)) {
    throw std::runtime_error("Visual Studio developer environment is missing: " + developer_command.string());
  }

  // Capture VsDevCmd's environment in a child shell without changing the CLI process.
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

  return detail::ParseProcessEnvironment(result.output);
}

bool HasMsvcTools(const ProcessEnvironment& environment) {
  const auto sdk = detail::EnvironmentValue(environment, "WINDOWSSDKDIR");
  const auto include_version = detail::EnvironmentValue(environment, "WINDOWSSDKVERSION");
  return FindExecutable("cl", environment) && FindExecutable("link", environment) && !sdk.empty() &&
         detail::VersionAtLeast(detail::ParseToolVersion(include_version), "10.0.22000") &&
         std::filesystem::is_regular_file(std::filesystem::path(sdk) / "Include" / include_version / "um/Windows.h") &&
         std::filesystem::is_regular_file(std::filesystem::path(sdk) / "Lib" / include_version / "um/x64/Kernel32.Lib");
}

int SetupMsvc(const std::filesystem::path& existing) {
  const auto root = detail::ManagedToolDirectory();
  if (root.empty())
    throw std::runtime_error("LOCALAPPDATA is required to prepare Visual Studio Build Tools");
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto work = root / ("vs-build-tools-" + std::to_string(nonce));
  std::filesystem::create_directories(root);
  if (!std::filesystem::create_directory(work))
    throw std::runtime_error("Visual Studio setup is already running");
  const auto cleanup = [&] {
    std::error_code error;
    std::filesystem::remove_all(work, error);
  };
  try {
    const auto script = work / "install.ps1";
    {
      std::ofstream output(script, std::ios::binary);
      output << R"ps1(param([string]$Instance)
$ErrorActionPreference = 'Stop'
$workload = 'Microsoft.VisualStudio.Workload.VCTools'
$components = @('--add', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '--add',
                'Microsoft.VisualStudio.Component.Windows11SDK.26100')
if ($Instance) {
  $installer = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/setup.exe'
  $arguments = @('modify', '--installPath', ('"' + $Instance + '"')) + $components
} else {
  $installer = Join-Path $PSScriptRoot 'vs_buildtools.exe'
  Invoke-WebRequest -UseBasicParsing 'https://aka.ms/vs/17/release/vs_buildtools.exe' -OutFile $installer
  $arguments = @('--wait', '--add', $workload) + $components
}
$signature = Get-AuthenticodeSignature -FilePath $installer
if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch '(^|, )CN=Microsoft Corporation(,|$)') {
  throw 'Microsoft Visual Studio installer signature could not be verified.'
}
try {
  $process = Start-Process -FilePath $installer -ArgumentList ($arguments + @('--passive','--norestart')) -Verb RunAs -Wait -PassThru
} catch {
  if ($_.Exception.NativeErrorCode -eq 1223) { exit 1223 }
  throw
}
exit $process.ExitCode
)ps1";
      if (!output)
        throw std::runtime_error("cannot write the Visual Studio installation script");
    }
    std::vector<std::string> arguments{"-NoProfile", "-ExecutionPolicy", "Bypass", "-File", script.string()};
    if (!existing.empty())
      arguments.insert(arguments.end(), {"-Instance", existing.string()});
    const auto powershell = detail::RequireSetupTool("powershell");
    const int result = RunProcess({powershell.string(), std::move(arguments), work});
    if (result == 0) {
      const auto installation = VisualStudioInstallation();
      const auto environment = installation.empty() ? ProcessEnvironment{} : ImportMsvcEnvironment(installation);
      if (installation.empty() || !HasMsvcTools(environment)) {
        throw std::runtime_error("Visual Studio setup did not provide the x64 compiler, linker, and Windows SDK");
      }
      // Verify that the selected compiler can compile against the SDK and link a Windows import library.
      const auto source = work / "probe.cpp";
      {
        std::ofstream output(source);
        output << "#include <windows.h>\nint main() { return GetCurrentProcessId() == 0; }\n";
      }
      const auto probe = RunProcessCapture(detail::WithEnvironment(
          {"cl", {"/nologo", "/EHsc", source.string(), "/link", "kernel32.lib"}, work}, environment));
      if (probe.exit_code != 0 || !std::filesystem::is_regular_file(work / "probe.exe")) {
        throw std::runtime_error("MSVC compile and link verification failed:\n" + probe.output);
      }
    }
    cleanup();
    return result;
  } catch (...) {
    cleanup();
    throw;
  }
}

class WindowsDriver final : public PlatformDriver {
public:
  std::string_view Id() const noexcept override {
    return "windows";
  }

  std::span<const std::string_view> TargetAbis() const noexcept override {
    static constexpr std::array abis{std::string_view{"x86_64"}};
    return abis;
  }

  bool SupportsCurrentHost() const noexcept override {
    return CurrentHostId() == "windows";
  }

  std::vector<EnvironmentDiagnostic> DiagnoseEnvironment(const EnvironmentRequest& request) const override {
    if (!SupportsCurrentHost() || request.operation == EnvironmentOperation::Devices) {
      return PlatformDriver::DiagnoseEnvironment(request);
    }
    const auto installation = VisualStudioInstallation();
    EnvironmentDiagnostic check{EnvironmentDiagnosticStatus::Missing,
                                "msvc",
                                "MSVC C++ build tools and Windows SDK",
                                installation.string(),
                                {},
                                {},
                                installation};
    if (!installation.empty()) {
      try {
        if (HasMsvcTools(ImportMsvcEnvironment(installation)))
          check.status = EnvironmentDiagnosticStatus::Ready;
      } catch (const std::runtime_error& error) {
        check.detail = error.what();
      }
    }
    return {std::move(check)};
  }

  std::vector<SetupAction> PlanSetup(std::span<const EnvironmentDiagnostic> diagnostics,
                                     const EnvironmentRequest&) const override {
    if (!detail::NeedsSetup(diagnostics, "msvc"))
      return {};
    auto existing = VisualStudioInstallation();
    if (existing.empty())
      existing = VisualStudioInstallation(false);
    return {{existing.empty() ? "Install Microsoft Visual Studio Build Tools and Windows SDK"
                              : "Add the x64 C++ tools and Windows SDK to " + existing.string(),
             std::nullopt,
             "install_msvc",
             {},
             {"msvc"},
             [existing] { return SetupMsvc(existing); },
             true}};
  }

  std::vector<GeneratedFile> CreateShell(const ProjectTemplateContext& context) const override {
    std::vector<GeneratedFile> files = RenderTemplateTree("platform/desktop/app", context);
    std::vector<GeneratedFile> platform_files = RenderTemplateTree("platform/windows/app", context);
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
        std::string_view{"app.manifest"},
        std::string_view{"app.ico"},
        std::string_view{"app.rc.in"},
    };
    return detail::ValidateRequiredFiles(shell_root, required);
  }

  ProcessEnvironment ResolveEnvironment(const EnvironmentRequest&) const override {
    const auto installation = VisualStudioInstallation();
    if (installation.empty()) {
      throw std::runtime_error("cannot find Visual Studio with the MSVC C++ x64 tools; run 'huxerui doctor windows'");
    }
    const auto environment = ImportMsvcEnvironment(installation);
    if (!HasMsvcTools(environment))
      throw std::runtime_error("the selected Visual Studio instance lacks the Windows SDK");
    return environment;
  }

  std::vector<ProcessCommand> BuildCommands(const PlatformCommandContext& context) const override {
    std::vector<ProcessCommand> commands = detail::DesktopBuildCommands(context);
    commands.front().arguments.push_back("-DCMAKE_CXX_COMPILER=cl");
    if (context.package) {
      const std::filesystem::path wix_root = context.project_root / ".huxerui/package/windows/dependencies/wix";
      commands.front().arguments.push_back("-DHUXERUI_WIX_ROOT=" + wix_root.string());
    }
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
    const std::string plan = detail::ReadFile(WindowsPackagePlan(context));
    const std::filesystem::path artifact = WindowsPackageArtifact(context, plan);
    return {{artifact, artifact.filename()}};
  }

  std::vector<ProcessCommand> PackageCommands(const PlatformCommandContext& context) const override {
    const std::string plan = detail::ReadFile(WindowsPackagePlan(context));
    const std::string target = detail::JsonString(plan, "target");
    const std::string install_component = detail::JsonString(plan, "installComponent");
    const std::filesystem::path package_source = detail::JsonString(plan, "packageSource");
    const std::filesystem::path bundle_source = detail::JsonString(plan, "bundleSource");
    const std::string installer_plan = detail::ReadFile(detail::JsonString(plan, "installerPlan"));
    const std::filesystem::path wix = detail::JsonString(installer_plan, "wix");
    const std::filesystem::path installer = detail::JsonString(installer_plan, "installer");
    const std::string installer_component = detail::JsonString(installer_plan, "installComponent");
    const std::filesystem::path root = WindowsPackageRoot(context);
    const std::filesystem::path staging = root / "staging";
    const std::filesystem::path installer_staging = root / "installer";
    const std::filesystem::path project_shell = context.project_root / "platform/windows";
    const std::filesystem::path msi = root / (target + ".msi");
    const std::filesystem::path bundle = WindowsPackageArtifact(context, plan);
    const std::filesystem::path installer_payloads = root / "installer-payloads.wxs";
    std::vector<ProcessCommand> commands =
        detail::DesktopPackageStageCommands(context, staging, install_component);
    std::vector<ProcessCommand> installer_commands =
        detail::DesktopPackageStageCommands(context, installer_staging, installer_component);
    commands.insert(commands.end(), std::make_move_iterator(installer_commands.begin()),
                    std::make_move_iterator(installer_commands.end()));
    commands.push_back(
        {wix.string(),
         {"build", package_source.string(), "-arch", "x64", "-bindpath", "Application=" + staging.string(),
          "-bindpath", "Project=" + project_shell.string(), "-out", msi.string()},
         root});
    commands.push_back(
        {"cmake",
         {"-DHUXERUI_WIX_PAYLOAD_DIRECTORY=" + installer_staging.string(),
          "-DHUXERUI_WIX_PAYLOAD_EXECUTABLE=" + installer.string(),
          "-DHUXERUI_WIX_PAYLOAD_OUTPUT=" + installer_payloads.string(), "-P", WixPayloadScript(context).string()},
         root});
    commands.push_back(
        {wix.string(),
         {"build", bundle_source.string(), installer_payloads.string(), "-arch", "x64", "-bindpath",
          "Installer=" + installer_staging.string(), "-bindpath", "Package=" + root.string(), "-bindpath",
          "Project=" + project_shell.string(), "-out", bundle.string()},
         root});
    return commands;
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
