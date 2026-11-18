#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "process_runner.h"
#include "sdk.h"
#include "template.h"

namespace huxerui::cli {

/// Result of validating one generated platform-shell requirement.
struct Diagnostic {
  /// Whether the result prevents the requested operation.
  bool error = false;
  /// Human-readable English diagnostic.
  std::string message;
};

/// Availability state for one host tool or SDK requirement.
enum class EnvironmentDiagnosticStatus {
  /// The requirement is satisfied.
  Ready,
  /// The requirement applies but is not currently available.
  Missing,
  /// A prerequisite must be satisfied before the requirement can be checked.
  Blocked,
  /// The requirement or platform cannot be used from the current host.
  Unavailable,
};

/// One environment requirement reported by `doctor` and consumed by `setup`.
struct EnvironmentDiagnostic {
  /// Current availability state.
  EnvironmentDiagnosticStatus status = EnvironmentDiagnosticStatus::Missing;
  /// Stable driver-owned identifier used when planning setup actions.
  std::string id;
  /// Human-readable requirement name.
  std::string label;
  /// Resolved path, version output, or another useful diagnostic detail.
  std::string detail;
  std::string version = {};
  std::string required_version = {};
  std::filesystem::path path = {};
  /// False for checks needed only to perform a proposed installation.
  bool required = true;
};

enum class EnvironmentOperation { Build, Run, Package, Devices };

/// Project and invocation inputs shared by diagnostics, setup planning, and tool discovery.
struct EnvironmentRequest {
  EnvironmentOperation operation = EnvironmentOperation::Build;
  std::filesystem::path project_root;
  std::filesystem::path sdk_home;
  std::filesystem::path java_home;
  std::string generator;
  std::string package_format;
  std::string device;
  /// Shared CMake discovery result when a plan has already resolved it.
  std::optional<std::filesystem::path> cmake_path = std::nullopt;
};

/// One automatic or manual action proposed by `setup`.
struct SetupAction {
  /// Human-readable action description.
  std::string description;
  /// Child process to run. An action without a command or an install callback requires manual work.
  std::optional<ProcessCommand> command;
  /// Stable identifier for this action within its driver.
  std::string id = {};
  /// Checks that must be ready before the action can run.
  std::vector<std::string> prerequisites = {};
  /// Checks that must pass when the environment is probed again after the action.
  std::vector<std::string> provides = {};
  /// Deferred work for installations that need more than one child process.
  std::function<int()> install = {};
  /// The vendor or system installer may prompt even when the CLI confirmation is skipped.
  bool interactive = false;
};

/// Readiness state reported by a platform device tool.
enum class DeviceState {
  /// The device is available for build and launch operations.
  Ready,
  /// The device is known but not currently running or connected.
  Offline,
  /// The device is connected but has not authorized the development host.
  Unauthorized,
  /// The platform tool reported another unusable state.
  Unavailable,
};

/// Platform-specific destination category.
enum class DeviceKind {
  /// The platform does not distinguish destination kinds.
  Unspecified,
  /// A physical device.
  Physical,
  /// A simulator or emulator destination.
  Simulator,
};

/// Device discovered by a platform's canonical development tool.
struct PlatformDevice {
  /// Stable identifier passed back to the platform tool.
  std::string id;
  /// Optional human-readable model or device name.
  std::string name;
  /// Current readiness state.
  DeviceState state = DeviceState::Unavailable;
  /// Physical or simulated destination classification when the platform exposes it.
  DeviceKind kind = DeviceKind::Unspecified;
  /// Optional build-system destination identifier distinct from `id`.
  std::string destination_id;

  /// Compares every discovered-device field.
  bool operator==(const PlatformDevice&) const = default;
};

/// Inputs shared by platform build, run, package, and open command planning.
struct PlatformCommandContext {
  /// Application project root.
  std::filesystem::path project_root;
  /// Active HuxerUI SDK prefix or source checkout.
  std::filesystem::path huxerui_home;
  /// Platform- and profile-specific incremental build directory.
  std::filesystem::path build_directory;
  /// Explicit CMake generator, or an empty string when the driver should preserve or select its default.
  std::string cmake_generator;
  /// Lowercase build profile, currently `debug` or `release`.
  std::string profile;
  /// Selected external device, or no value for host-local and implicit-device platforms.
  std::optional<PlatformDevice> device;
  /// Whether command planning is producing a distributable package instead of an ordinary incremental build.
  bool package = false;
  /// Explicit release optimization override; an absent value preserves project settings.
  std::optional<std::string> optimization;
  /// Explicit release LTO override; an absent value preserves project settings.
  std::optional<bool> lto;
  /// Canonical target ABI names, or an empty list to preserve the native build defaults.
  std::vector<std::string> abis = {};
  /// Explicit Linux package format, or no override to use AppImage.
  std::optional<std::string> package_format = std::nullopt;
  /// Whether to compile profiling support into the selected HuxerUI source build.
  bool profiling = false;
  /// Whether this build belongs to a development watch session.
  bool watch = false;
  /// Resolved platform toolchain and SDK overrides for build children.
  ProcessEnvironment environment = {};
};

/// One file or directory published by the `package` command.
struct PackageArtifact {
  /// Existing package artifact.
  std::filesystem::path source;
  /// Safe path relative to the package output directory.
  std::filesystem::path destination;

  bool operator==(const PackageArtifact&) const = default;
};

/// Rendered files and the directory where the CLI publishes them.
struct GeneratedTree {
  std::filesystem::path directory;
  std::vector<GeneratedFile> files;
};

/// Owns CLI behavior for one target platform.
///
/// Drivers describe environment requirements, generate the platform shell, and plan direct process invocations.
/// They do not execute commands themselves, which keeps CLI output, failure handling, and tests consistent across
/// platforms.
class PlatformDriver {
public:
  /// Destroys a platform driver.
  virtual ~PlatformDriver() = default;

  /// Returns the stable lowercase platform identifier used by CLI arguments and project directories.
  virtual std::string_view Id() const noexcept = 0;

  /// Returns the canonical ABI names accepted by this driver before host and destination checks.
  virtual std::span<const std::string_view> TargetAbis() const noexcept = 0;

  /// Returns whether this driver can build from the current host.
  virtual bool SupportsCurrentHost() const noexcept = 0;

  /// Diagnoses the requirements for the requested operation without installing tools or changing the project.
  /// @param request Operation and any project-specific tool selections.
  /// @return One result for each platform requirement.
  virtual std::vector<EnvironmentDiagnostic> DiagnoseEnvironment(const EnvironmentRequest& request = {}) const;

  /// Converts missing environment requirements into executable or manual setup actions.
  /// @param diagnostics Results previously returned by `DiagnoseEnvironment()`.
  /// @param request Same operation and project inputs used for diagnostics.
  /// @return Ordered setup actions; an empty result means no action is required or available.
  virtual std::vector<SetupAction> PlanSetup(std::span<const EnvironmentDiagnostic> diagnostics,
                                             const EnvironmentRequest& request = {}) const;

  /// Generates the platform shell for an application or library preview.
  /// @param context Project identity used to render templates.
  /// @return Files relative to the platform shell root.
  virtual std::vector<GeneratedFile> CreateShell(const ProjectTemplateContext& context) const = 0;

  /// Generates the platform-owned package for a reusable library.
  /// @param context Library identity used to render templates.
  /// @param library_root Project directory that will contain the package.
  /// @return Package directory and files relative to it, or an empty tree when no package is needed.
  virtual GeneratedTree CreateLibraryPackage(const LibraryTemplateContext& context,
      const std::filesystem::path& library_root) const;

  /// Validates a generated platform shell.
  /// @param shell_root Platform shell directory.
  /// @return Missing-file and structural diagnostics.
  virtual std::vector<Diagnostic> Diagnose(const std::filesystem::path& shell_root) const = 0;

  /// Returns whether this platform exposes selectable external devices from the current host.
  virtual bool SupportsDeviceDiscovery() const noexcept;

  /// Discovers platform destinations with their current readiness state.
  /// @param request Project and toolchain inputs used to resolve the discovery executable.
  /// @return Devices reported by the platform's canonical development tool.
  /// @throws std::logic_error if the platform does not support discovery.
  /// @throws std::runtime_error if the platform tool fails.
  virtual std::vector<PlatformDevice> DiscoverDevices(const EnvironmentRequest& request = {}) const;

  /// Plans the optional CMake configure commands used to discover app-side library dependencies.
  /// @param context Active build context.
  /// @return Commands that must run before project integration and the platform build.
  virtual std::vector<ProcessCommand> LibraryGraphCommands(const PlatformCommandContext& context) const;

  /// Updates generated platform integration from the current library graph.
  /// @param context Active build context.
  virtual void UpdateProjectIntegration(const PlatformCommandContext& context) const;

  /// Resolves platform-owned paths and environment values for child processes.
  virtual ProcessEnvironment ResolveEnvironment(const EnvironmentRequest& request) const;

  /// Rejects an explicit ABI selection incompatible with the current build inputs.
  virtual void ValidateBuildAbis(const PlatformCommandContext& context) const;

  /// Checks the output ABI where the target is selected by an external toolchain.
  virtual void VerifyBuildAbis(const PlatformCommandContext& context) const;

  /// Plans commands that build the selected platform and profile.
  /// @param context Active build context.
  /// @return Commands executed in order by the CLI.
  virtual std::vector<ProcessCommand> BuildCommands(const PlatformCommandContext& context) const = 0;

  /// Plans commands that launch an already-built application.
  /// @param context Active build context, including the selected device when required.
  /// @return Commands executed in order by the CLI.
  virtual std::vector<ProcessCommand> RunCommands(const PlatformCommandContext& context) const = 0;

  /// Plans platform-native packaging commands after a successful build.
  /// @param context Active package context.
  /// @return Commands that produce the files returned by `PackageArtifacts()`.
  virtual std::vector<ProcessCommand> PackageCommands(const PlatformCommandContext& context) const;

  /// Resolves files emitted by a successful platform build.
  /// @param context Active build context.
  /// @return Source files and their package-relative destinations.
  virtual std::vector<PackageArtifact> PackageArtifacts(const PlatformCommandContext& context) const = 0;

  /// Plans commands that open the platform's native development project.
  /// @param context Active build context.
  /// @return Commands executed in order by the CLI.
  /// @throws std::logic_error if the platform does not support `open`.
  virtual std::vector<ProcessCommand> OpenCommands(const PlatformCommandContext& context) const;

protected:
  /// Returns executable names checked by the default environment diagnosis.
  virtual std::span<const std::string_view> RequiredTools() const noexcept {
    return {};
  }
};

/// Returns a stable lowercase name for a device state.
/// @param state State to name.
/// @return `"ready"`, `"offline"`, `"unauthorized"`, or `"unavailable"`.
std::string_view DeviceStateName(DeviceState state) noexcept;

/// Parses `adb devices -l` output without invoking ADB.
/// @param output Complete ADB output.
/// @return Recognized devices in source order.
std::vector<PlatformDevice> ParseAdbDevices(std::string_view output);

/// Parses `xcrun devicectl list devices` output without invoking `devicectl`.
/// @param output Complete tool output.
/// @return Recognized physical iOS devices in source order.
std::vector<PlatformDevice> ParseIosPhysicalDevices(std::string_view output);

/// Parses `xcrun simctl list devices` output without invoking `simctl`.
/// @param output Complete tool output.
/// @return Simulators from available runtimes in source order, with only booted devices marked ready.
std::vector<PlatformDevice> ParseIosSimulatorDevices(std::string_view output);

/// Returns the stable platform identifier for the host that compiled the CLI.
std::string_view CurrentHostId() noexcept;

/// Finds a platform driver by its stable identifier.
/// @param id Lowercase platform identifier.
/// @return Driver singleton, or `nullptr` when `id` is unknown.
const PlatformDriver* FindPlatformDriver(std::string_view id) noexcept;

/// Returns every registered platform identifier in CLI display order.
std::vector<std::string_view> PlatformIds();

} // namespace huxerui::cli

namespace huxerui::cli::detail {

/// Resolves one CLI spelling to a canonical ABI supported by the selected platform.
std::string_view NormalizeTargetAbi(const PlatformDriver& platform, std::string_view abi);

/// Joins canonical ABI names for a native command or an incremental build directory.
std::string JoinTargetAbis(std::span<const std::string> abis, char separator);

/// Reports missing regular files below a generated-tree root.
std::vector<Diagnostic> ValidateRequiredFiles(const std::filesystem::path& root,
                                              std::span<const std::string_view> paths);

/// Reads a complete file as binary bytes.
std::string ReadFile(const std::filesystem::path& path);

/// Extracts one string property from CLI-owned JSON metadata.
std::string JsonString(std::string_view json, std::string_view key);

/// Resolves the unique generated application integration plan for a build.
std::filesystem::path AppIntegrationPlan(const PlatformCommandContext& context);

/// Converts an optional external device to the common command-line selector arguments.
std::vector<std::string> DeviceArguments(const std::optional<PlatformDevice>& device);

/// Converts a lowercase CLI profile to its CMake configuration name.
std::string ProfileConfiguration(std::string_view profile);

/// Plans the shared desktop CMake configure and build commands.
std::vector<ProcessCommand> DesktopBuildCommands(const PlatformCommandContext& context);

/// Plans the shared CMake install staging commands used by desktop packages.
std::vector<ProcessCommand> DesktopPackageStageCommands(
    const PlatformCommandContext& context, const std::filesystem::path& staging, std::string_view install_component);

/// Plans the CMake configure command that emits the app-side library graph.
std::vector<ProcessCommand> LibraryGraphConfigureCommands(const PlatformCommandContext& context);

/// Plans the Android library-graph configure command for an explicit host.
std::vector<ProcessCommand> AndroidLibraryGraphCommands(const PlatformCommandContext& context,
                                                        std::string_view host_id);

/// Plans Android Gradle commands for an explicit host and optional Termux `aapt2` path.
std::vector<ProcessCommand> AndroidBuildCommands(const PlatformCommandContext& context, std::string_view host_id,
                                                 const std::filesystem::path& aapt2);

/// Plans Android launch commands for an explicit host.
std::vector<ProcessCommand> AndroidRunCommands(const PlatformCommandContext& context, std::string_view host_id);

/// Plans the Web development-server command for an explicit host.
std::vector<ProcessCommand> WebRunCommands(const PlatformCommandContext& context, std::string_view host_id);

/// Returns the Android driver singleton.
const PlatformDriver& AndroidPlatformDriver() noexcept;
/// Returns the Windows driver singleton.
const PlatformDriver& WindowsPlatformDriver() noexcept;
/// Returns the Linux driver singleton.
const PlatformDriver& LinuxPlatformDriver() noexcept;
/// Returns the macOS driver singleton.
const PlatformDriver& MacOSPlatformDriver() noexcept;
/// Returns the iOS driver singleton.
const PlatformDriver& IosPlatformDriver() noexcept;
/// Returns the Web driver singleton.
const PlatformDriver& WebPlatformDriver() noexcept;

} // namespace huxerui::cli::detail
