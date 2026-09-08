#pragma once

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>

namespace huxerui::cli {

/// Identifies incremental output by the normalized framework location.
[[nodiscard]] std::string BuildHomeKey(const std::filesystem::path& home);

/// Validates an explicit source override before validating the configured SDK candidate.
[[nodiscard]] std::filesystem::path ResolveBuildHome(const std::filesystem::path& sdk_home,
    const std::optional<std::filesystem::path>& source, const std::filesystem::path& working_directory);

/// Identifies how the CLI located the active HuxerUI SDK.
enum class SdkLocationSource {
  /// No usable SDK was found.
  Missing,
  /// `HUXERUI_HOME` selected the SDK.
  Environment,
  /// The CLI executable's installed layout selected the SDK.
  Executable,
};

/// Resolved SDK location and its source of truth.
struct SdkLocation {
  /// SDK prefix containing public headers, CMake metadata, tools, and platform artifacts.
  std::filesystem::path home;
  /// Mechanism that selected `home`.
  SdkLocationSource source = SdkLocationSource::Missing;
};

/// Resolves the running CLI executable path.
/// @param argument_zero Value received as the process `argv[0]`.
/// @return An absolute, normalized executable path when the host can resolve it.
[[nodiscard]] std::filesystem::path ExecutablePath(std::string_view argument_zero);

/// Locates the configured HuxerUI SDK candidate without validating a build selection.
///
/// A defined `HUXERUI_HOME` is authoritative. Otherwise the function inspects the installed layout surrounding the CLI
/// executable.
///
/// @param executable_path Resolved CLI executable path.
/// @return The configured location, or source `Missing` when no SDK is available.
/// Environment selections are validated by the consuming command so --source can override an invalid SDK path.
[[nodiscard]] SdkLocation LocateHuxerUIHome(const std::filesystem::path& executable_path);

/// Rejects source checkouts, missing installations, and a CLI belonging to another SDK.
/// @param sdk Selected SDK location.
/// @param executable_path Running CLI executable, resolved independently of HUXERUI_HOME.
/// @throws std::runtime_error if replacing the selected SDK would be ambiguous or unsafe.
void ValidateSdkUpdate(const SdkLocation& sdk, const std::filesystem::path& executable_path);

/// Runs the embedded SDK installer without changing persistent environment selection.
/// @param sdk Installed SDK selected by the running CLI.
/// @param target_version Explicit release version, or empty for the latest stable release.
/// @param check_only Query versions without replacing the SDK.
/// @param assume_yes Skip the installer's interactive confirmation.
/// @param output Destination for process handoff diagnostics; the installer inherits standard streams.
/// @return Installer exit code, or zero after a successful Windows updater handoff.
int UpdateSdk(const SdkLocation& sdk, std::string_view target_version, bool check_only, bool assume_yes,
              std::ostream& output);

/// Resolves and validates an explicit HuxerUI source checkout.
/// @param path Repository root, either absolute or relative to the current working directory.
/// @return The absolute, normalized repository root.
/// @throws std::runtime_error if `path` is not a HuxerUI source checkout.
[[nodiscard]] std::filesystem::path ResolveHuxerUISource(const std::filesystem::path& path);

/// Locates the canonical application-development Skill in an SDK or source checkout.
/// @param huxerui_home SDK prefix or repository root.
/// @return Path to the `huxerui-app-development` Skill directory.
/// @throws std::runtime_error if the SDK layout is invalid or the Skill is absent.
[[nodiscard]] std::filesystem::path ResolveApplicationDevelopmentSkill(const std::filesystem::path& huxerui_home);

/// Returns the stable diagnostic name for an SDK location source.
/// @param source Source value to name.
/// @return `"missing"`, `"environment"`, or `"executable"`.
[[nodiscard]] std::string_view SdkLocationSourceName(SdkLocationSource source) noexcept;

} // namespace huxerui::cli
