#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "process_runner.h"

namespace huxerui::cli::mcpp {

/// What the CLI asks the mcpp integration to build or run.
struct CommandOptions {
  /// Root directory containing `mcpp.toml`.
  std::filesystem::path project_root;
  /// HuxerUI platform identifier, such as `linux` or `web`.
  std::string platform;
  /// Logical profile, `debug` or `release`.
  std::string profile = "debug";
  /// The mcpp profile `ProfileName` resolved, or empty for the logical profile's own.
  std::string profile_name = {};
  /// Features the command opens, such as `huxerui/profiling`.
  std::vector<std::string> features = {};
  /// Canonical ABIs to build, one target each; empty builds the platform's default.
  std::vector<std::string> abis = {};
};

/// The mcpp target triple of a platform's canonical ABI, such as `x86_64-linux-android` for Android `x86_64`.
/// @throws std::invalid_argument for an ABI the mcpp integration does not build, naming why.
[[nodiscard]] std::string AbiTriple(std::string_view platform, std::string_view abi);

/// The mcpp profile a logical profile selects with the CLI's release overrides.
///
/// A debug build is mcpp's `dev` profile, and the overrides apply to release builds only, as in the CMake
/// integration. A release build with `--optimization size` or `minsize`, or `--lto on`, selects one of the profiles
/// `huxerui create` declares: `release-size`, `release-minsize`, `release-lto`, `release-size-lto`, and
/// `release-minsize-lto`.
/// @return `dev`, `release`, or one of the declared release profiles.
[[nodiscard]] std::string ProfileName(std::string_view profile, const std::optional<std::string>& optimization,
                                      const std::optional<bool>& lto);

/// Whether a manifest declares a profile, as `[profile.<name>]` or as a key of the `[profile]` table.
[[nodiscard]] bool DeclaresProfile(std::string_view manifest, std::string_view name);

/// The feature that compiles the framework's profiling support, named through the key the manifest gives the
/// framework dependency: `huxerui/profiling` or `huxerui.huxerui/profiling`.
/// @throws std::runtime_error when the manifest declares no HuxerUI dependency.
[[nodiscard]] std::string ProfilingFeature(std::string_view manifest);

/// Resolves the options of a command from the CLI's build intent and the project's manifest: `optimization` and
/// `lto` select a declared release profile, and `profiling` opens the framework's profiling feature.
/// @throws std::invalid_argument when the selected profile is not declared.
[[nodiscard]] CommandOptions ResolveOptions(const std::filesystem::path& project_root, std::string_view platform,
                                            std::string_view profile, const std::optional<std::string>& optimization,
                                            const std::optional<bool>& lto, bool profiling);

/// The arguments that select a profile and features, for a command that takes them like `mcpp build`.
[[nodiscard]] std::vector<std::string> ProfileArguments(const CommandOptions& options);

/// The mcpp target triple a platform is built for, or an empty string for the host's own desktop platform.
/// @throws std::invalid_argument for a platform the mcpp integration does not build.
[[nodiscard]] std::string_view TargetTriple(std::string_view platform);

/// The `mcpp build` command for a platform and profile.
[[nodiscard]] ProcessCommand BuildCommand(const CommandOptions& options);

/// The `mcpp run` command, which builds and starts the application: the host's program, or for a platform with a
/// target triple the package its format writes, through that format's runner.
[[nodiscard]] ProcessCommand RunCommand(const CommandOptions& options);

/// The `mcpp run --print-launch` command that builds the program and reports, as JSON, how the named runner the
/// project's packages supply would start it, without starting it.
[[nodiscard]] ProcessCommand LaunchCommand(const CommandOptions& options, std::string_view runner);

/// The program the `mcpp.launch` envelope of `mcpp run --print-launch --message-format json` starts.
/// @throws std::runtime_error when the output carries no launch envelope.
[[nodiscard]] std::filesystem::path ParseLaunchProgram(std::string_view output);

/// The mcpp package format a platform's distributable is written in.
/// @param platform HuxerUI platform identifier.
/// @param requested The `--format` the caller selected, or empty for the platform's default.
/// @return The format: `appimage`, `deb`, or `rpm` on Linux, `setup` on Windows, `dmg` on macOS, `web` on the Web,
///         `apk` on Android, and `app` on iOS.
/// @throws std::runtime_error for a platform mcpp has no package format for.
[[nodiscard]] std::string PackageFormat(std::string_view platform, std::string_view requested);

/// The `mcpp pack` command that writes a platform's distributable and reports it as JSON on standard output.
[[nodiscard]] ProcessCommand PackCommand(const CommandOptions& options, std::string_view format);

/// One distributable `mcpp pack` reported.
struct PackedArtifact {
  /// Absolute path of the file or directory.
  std::filesystem::path path;
  /// `file` or `dir`.
  std::string type;
};

/// The program `mcpp build --message-format json` reported building: the application's `bin`, `app`, or `shared`
/// artifact, from the `artifact` events it writes before its envelope.
/// @throws std::runtime_error when the output names no program.
[[nodiscard]] PackedArtifact ParseBuiltProgram(std::string_view output, std::string& name);

/// Reads the artifacts from the `mcpp.pack` envelope `mcpp pack --message-format json` writes.
/// @throws std::runtime_error when the envelope reports no artifact.
[[nodiscard]] std::vector<PackedArtifact> ParsePackArtifacts(std::string_view envelope);

} // namespace huxerui::cli::mcpp
