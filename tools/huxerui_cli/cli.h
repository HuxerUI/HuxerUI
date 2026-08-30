#pragma once

#include <filesystem>
#include <iosfwd>
#include <span>
#include <string_view>

#include "sdk.h"

namespace huxerui::cli {

/// Runs one HuxerUI CLI invocation.
///
/// The argument span excludes the executable name. Diagnostics and usage errors are written to `error`; normal command
/// output is written to `output`. The supplied streams make confirmation prompts and command output testable without
/// replacing process-wide standard streams.
///
/// @param arguments Command and options, such as `{"build", "windows", "--profile", "release"}`.
/// @param working_directory Directory used for project discovery and relative command behavior.
/// @param sdk Resolved HuxerUI SDK location for this invocation.
/// @param input Input stream used by interactive commands such as `setup`.
/// @param output Destination for normal command output.
/// @param error Destination for usage and execution diagnostics.
/// @return `0` on success, `2` for invalid usage, or `1` for another command failure.
int Run(std::span<const std::string_view> arguments, const std::filesystem::path& working_directory,
        const SdkLocation& sdk, std::istream& input, std::ostream& output, std::ostream& error);

} // namespace huxerui::cli
