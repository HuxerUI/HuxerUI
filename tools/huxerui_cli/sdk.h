#pragma once

#include <filesystem>
#include <string_view>

namespace huxerui::cli {

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

/// Locates the HuxerUI SDK used by the CLI.
///
/// A defined `HUXERUI_HOME` is authoritative. Otherwise the function inspects the installed layout surrounding the CLI
/// executable.
///
/// @param executable_path Resolved CLI executable path.
/// @return The selected SDK location, or a location with source `Missing` when no SDK is available.
/// @throws std::runtime_error if `HUXERUI_HOME` is defined but does not name a valid HuxerUI SDK or source checkout.
[[nodiscard]] SdkLocation LocateHuxerUIHome(const std::filesystem::path& executable_path);

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
