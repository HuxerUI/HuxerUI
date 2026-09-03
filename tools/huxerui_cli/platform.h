#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "process_runner.h"
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
};

/// One command-backed or manual action proposed by `setup`.
struct SetupAction {
  /// Human-readable action description.
  std::string description;
  /// Command to execute, or `std::nullopt` when the user must complete the action manually.
  std::optional<ProcessCommand> command;
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
};

/// One file or directory published by the `package` command.
struct PackageArtifact {
  /// Existing package artifact.
  std::filesystem::path source;
  /// Safe path relative to the package output directory.
  std::filesystem::path destination;

  bool operator==(const PackageArtifact&) const = default;
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
  [[nodiscard]] virtual std::string_view Id() const noexcept = 0;

  /// Returns whether this driver can build from the current host.
  [[nodiscard]] virtual bool SupportsCurrentHost() const noexcept = 0;

  /// Diagnoses the host tools and SDK components required by this platform.
  /// @return One result for each platform requirement.
  [[nodiscard]] virtual std::vector<EnvironmentDiagnostic> DiagnoseEnvironment() const;

  /// Converts missing environment requirements into executable or manual setup actions.
  /// @param diagnostics Results previously returned by `DiagnoseEnvironment()`.
  /// @return Ordered setup actions; an empty result means no action is required or available.
  [[nodiscard]] virtual std::vector<SetupAction> PlanSetup(std::span<const EnvironmentDiagnostic> diagnostics) const;

  /// Generates the platform shell for an application or library preview.
  /// @param context Project identity used to render templates.
  /// @return Files relative to the platform shell root.
  [[nodiscard]] virtual std::vector<GeneratedFile> CreateShell(const ProjectTemplateContext& context) const = 0;

  /// Generates the platform-owned package for a reusable library.
  /// @param context Library identity used to render templates.
  /// @return Files relative to the library platform root, or an empty vector when no package is needed.
  [[nodiscard]] virtual std::vector<GeneratedFile> CreateLibraryPackage(const LibraryTemplateContext& context) const;

  /// Validates a generated platform shell.
  /// @param shell_root Platform shell directory.
  /// @return Missing-file and structural diagnostics.
  [[nodiscard]] virtual std::vector<Diagnostic> Diagnose(const std::filesystem::path& shell_root) const = 0;

  /// Returns whether this platform exposes selectable external devices from the current host.
  [[nodiscard]] virtual bool SupportsDeviceDiscovery() const noexcept;

  /// Discovers platform destinations with their current readiness state.
  /// @return Devices reported by the platform's canonical development tool.
  /// @throws std::logic_error if the platform does not support discovery.
  /// @throws std::runtime_error if the platform tool fails.
  [[nodiscard]] virtual std::vector<PlatformDevice> DiscoverDevices() const;

  /// Plans the optional CMake configure commands used to discover app-side library dependencies.
  /// @param context Active build context.
  /// @return Commands that must run before project integration and the platform build.
  [[nodiscard]] virtual std::vector<ProcessCommand> LibraryGraphCommands(const PlatformCommandContext& context) const;

  /// Updates generated platform integration from the current library graph.
  /// @param context Active build context.
  virtual void UpdateProjectIntegration(const PlatformCommandContext& context) const;

  /// Applies process-local environment preparation immediately before command planning.
  virtual void PrepareBuildEnvironment() const;

  /// Plans commands that build the selected platform and profile.
  /// @param context Active build context.
  /// @return Commands executed in order by the CLI.
  [[nodiscard]] virtual std::vector<ProcessCommand> BuildCommands(const PlatformCommandContext& context) const = 0;

  /// Plans commands that launch an already-built application.
  /// @param context Active build context, including the selected device when required.
  /// @return Commands executed in order by the CLI.
  [[nodiscard]] virtual std::vector<ProcessCommand> RunCommands(const PlatformCommandContext& context) const = 0;

  /// Plans platform-native packaging commands after a successful build.
  /// @param context Active package context.
  /// @return Commands that produce the files returned by `PackageArtifacts()`.
  [[nodiscard]] virtual std::vector<ProcessCommand> PackageCommands(const PlatformCommandContext& context) const;

  /// Resolves files emitted by a successful platform build.
  /// @param context Active build context.
  /// @return Source files and their package-relative destinations.
  [[nodiscard]] virtual std::vector<PackageArtifact> PackageArtifacts(const PlatformCommandContext& context) const = 0;

  /// Plans commands that open the platform's native development project.
  /// @param context Active build context.
  /// @return Commands executed in order by the CLI.
  /// @throws std::logic_error if the platform does not support `open`.
  [[nodiscard]] virtual std::vector<ProcessCommand> OpenCommands(const PlatformCommandContext& context) const;

protected:
  /// Returns executable names checked by the default environment diagnosis.
  [[nodiscard]] virtual std::span<const std::string_view> RequiredTools() const noexcept = 0;
};

/// Returns a stable lowercase name for a device state.
/// @param state State to name.
/// @return `"ready"`, `"offline"`, `"unauthorized"`, or `"unavailable"`.
[[nodiscard]] std::string_view DeviceStateName(DeviceState state) noexcept;

/// Parses `adb devices -l` output without invoking ADB.
/// @param output Complete ADB output.
/// @return Recognized devices in source order.
[[nodiscard]] std::vector<PlatformDevice> ParseAdbDevices(std::string_view output);

/// Parses `xcrun devicectl list devices` output without invoking `devicectl`.
/// @param output Complete tool output.
/// @return Recognized physical iOS devices in source order.
[[nodiscard]] std::vector<PlatformDevice> ParseIosPhysicalDevices(std::string_view output);

/// Parses `xcrun simctl list devices` output without invoking `simctl`.
/// @param output Complete tool output.
/// @return Simulators from available runtimes in source order, with only booted devices marked ready.
[[nodiscard]] std::vector<PlatformDevice> ParseIosSimulatorDevices(std::string_view output);

/// Returns the stable platform identifier for the host that compiled the CLI.
[[nodiscard]] std::string_view CurrentHostId() noexcept;

/// Finds a platform driver by its stable identifier.
/// @param id Lowercase platform identifier.
/// @return Driver singleton, or `nullptr` when `id` is unknown.
[[nodiscard]] const PlatformDriver* FindPlatformDriver(std::string_view id) noexcept;

/// Returns every registered platform identifier in CLI display order.
[[nodiscard]] std::vector<std::string_view> PlatformIds();

} // namespace huxerui::cli

namespace huxerui::cli::detail {

/// Reports missing regular files below a generated-tree root.
[[nodiscard]] std::vector<Diagnostic>
ValidateRequiredFiles(const std::filesystem::path& root, std::span<const std::string_view> paths);

/// Reads a complete file as binary bytes.
[[nodiscard]] std::string ReadFile(const std::filesystem::path& path);

/// Extracts one string property from CLI-owned JSON metadata.
[[nodiscard]] std::string JsonString(std::string_view json, std::string_view key);

/// Resolves the unique generated application integration plan for a build.
[[nodiscard]] std::filesystem::path AppIntegrationPlan(const PlatformCommandContext& context);

/// Converts an optional external device to the common command-line selector arguments.
[[nodiscard]] std::vector<std::string> DeviceArguments(const std::optional<PlatformDevice>& device);

/// Converts a lowercase CLI profile to its CMake configuration name.
[[nodiscard]] std::string ProfileConfiguration(std::string_view profile);

/// Plans the shared desktop CMake configure and build commands.
[[nodiscard]] std::vector<ProcessCommand> DesktopBuildCommands(const PlatformCommandContext& context);

/// Plans the shared CMake install staging commands used by desktop packages.
[[nodiscard]] std::vector<ProcessCommand> DesktopPackageStageCommands(
    const PlatformCommandContext& context, const std::filesystem::path& staging, std::string_view install_component);

/// Plans the CMake configure command that emits the app-side library graph.
[[nodiscard]] std::vector<ProcessCommand> LibraryGraphConfigureCommands(const PlatformCommandContext& context);

/// Plans the Android library-graph configure command for an explicit host.
[[nodiscard]] std::vector<ProcessCommand>
AndroidLibraryGraphCommands(const PlatformCommandContext& context, std::string_view host_id);

/// Plans Android Gradle commands for an explicit host and optional Termux `aapt2` path.
[[nodiscard]] std::vector<ProcessCommand>
AndroidBuildCommands(const PlatformCommandContext& context, std::string_view host_id,
                     const std::filesystem::path& aapt2);

/// Plans Android launch commands for an explicit host.
[[nodiscard]] std::vector<ProcessCommand>
AndroidRunCommands(const PlatformCommandContext& context, std::string_view host_id);

/// Plans the Web development-server command for an explicit host.
[[nodiscard]] std::vector<ProcessCommand>
WebRunCommands(const PlatformCommandContext& context, std::string_view host_id);

/// Returns the Android driver singleton.
[[nodiscard]] const PlatformDriver& AndroidPlatformDriver() noexcept;
/// Returns the Windows driver singleton.
[[nodiscard]] const PlatformDriver& WindowsPlatformDriver() noexcept;
/// Returns the Linux driver singleton.
[[nodiscard]] const PlatformDriver& LinuxPlatformDriver() noexcept;
/// Returns the macOS driver singleton.
[[nodiscard]] const PlatformDriver& MacOSPlatformDriver() noexcept;
/// Returns the iOS driver singleton.
[[nodiscard]] const PlatformDriver& IosPlatformDriver() noexcept;
/// Returns the Web driver singleton.
[[nodiscard]] const PlatformDriver& WebPlatformDriver() noexcept;

} // namespace huxerui::cli::detail
