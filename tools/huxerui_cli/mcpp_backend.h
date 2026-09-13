#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "process_runner.h"

namespace huxerui::cli {

class PlatformDriver;

/// What one CLI platform means to mcpp: the target row it builds for and the
/// `mcpp pack --format` that produces the thing a user installs there.
struct McppTarget {
  /// The canonical target triple, e.g. `x86_64-linux-android`.
  std::string triple;
  /// The distribution format `mcpp pack --format` takes, e.g. `apk`.
  std::string format;
  /// True when the artifact `mcpp run` hands the runner is the packaged form
  /// (`--format`), which is the case for an APK and an installed `.app`.
  bool runs_packaged = false;
};

/// The architecture segment of the host's own triple.
[[nodiscard]] std::string_view McppHostArchitecture() noexcept;

/// Maps a CLI platform identifier to its mcpp target.
/// @param platform_id One of the CLI's platform identifiers.
/// @param physical_device Whether the selected device is physical hardware; selects the device row over the
///        emulator or simulator row for Android and iOS.
/// @param host_architecture The host's architecture segment, which the desktop rows take.
/// @return The target, or a target with an empty triple for an unknown platform.
[[nodiscard]] McppTarget ResolveMcppTarget(std::string_view platform_id, bool physical_device,
                                           std::string_view host_architecture);

/// Reads the platforms an mcpp package declares in `[package] platforms`, as CLI platform identifiers
/// (`emscripten` is `web`). A manifest without the key gets the three desktop platforms.
[[nodiscard]] std::vector<std::string> McppProjectPlatforms(const std::filesystem::path& manifest);

/// Renders `[package] platforms` for a set of CLI platforms, in mcpp's own vocabulary.
[[nodiscard]] std::string McppPlatformsLine(std::span<const std::string> platform_ids);

/// `mcpp build --target <triple>`.
[[nodiscard]] ProcessCommand McppBuildCommand(const std::filesystem::path& root, const McppTarget& target, bool release);
/// `mcpp run --target <triple> [--format <format>]`.
[[nodiscard]] ProcessCommand McppRunCommand(const std::filesystem::path& root, const McppTarget& target, bool release);
/// `mcpp pack --target <triple> --format <format>`.
[[nodiscard]] ProcessCommand McppPackCommand(const std::filesystem::path& root, const McppTarget& target, bool release);
/// The directory `mcpp pack --format web` writes.
[[nodiscard]] std::filesystem::path McppWebOutputDirectory(const std::filesystem::path& root);

} // namespace huxerui::cli
