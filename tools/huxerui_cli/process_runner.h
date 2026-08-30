#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace huxerui::cli {

/// Describes a child process without invoking a shell.
struct ProcessCommand {
  /// Executable name or path.
  std::string executable;
  /// Arguments passed directly to the executable, excluding `argv[0]`.
  std::vector<std::string> arguments;
  /// Working directory for the child, or an empty path to inherit the current directory.
  std::filesystem::path working_directory;
};

/// Captured child-process completion state.
struct ProcessResult {
  /// Native process exit code.
  int exit_code = 0;
  /// Captured standard output decoded as UTF-8.
  std::string output;
};

/// Reads a process environment variable.
/// @param name Environment variable name.
/// @return The variable value, or `std::nullopt` when it is not defined.
[[nodiscard]] std::optional<std::string> ReadEnvironmentVariable(std::string_view name);

/// Sets an environment variable for the current process and subsequently launched children.
/// @param name Environment variable name.
/// @param value New value.
/// @throws std::runtime_error if the operating system rejects the update.
void SetProcessEnvironmentVariable(std::string_view name, std::string_view value);

/// Resolves an executable using the current process search path.
/// @param name Executable name without shell syntax.
/// @return The resolved path, or `std::nullopt` when no executable is found.
[[nodiscard]] std::optional<std::filesystem::path> FindExecutable(std::string_view name);

/// Formats a command for human-readable CLI output.
/// @param command Command to describe.
/// @return A quoted display string. The result is diagnostic text and is not intended for shell execution.
[[nodiscard]] std::string DescribeProcess(const ProcessCommand& command);

/// Runs a command with inherited standard streams.
/// @param command Command to execute.
/// @return The native process exit code.
/// @throws std::runtime_error if the process cannot be created or observed.
int RunProcess(const ProcessCommand& command);

/// Runs a command and captures its standard output.
/// @param command Command to execute.
/// @return Exit code and UTF-8 output.
/// @throws std::runtime_error if the process cannot be created or observed.
[[nodiscard]] ProcessResult RunProcessCapture(const ProcessCommand& command);

} // namespace huxerui::cli
