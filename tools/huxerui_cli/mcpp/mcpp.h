#pragma once

#include <filesystem>
#include <iosfwd>
#include <span>
#include <string_view>
#include <vector>

#include "process_runner.h"

namespace huxerui::cli::mcpp {

/// Options for the standalone mcpp build frontend.
struct BuildOptions {
  /// Root directory containing `mcpp.toml`.
  std::filesystem::path project_root;
  /// Selects mcpp's release profile instead of its development profile.
  bool release = false;
  /// Requires the mcpp lock file to be up to date.
  bool locked = false;
  /// Prevents mcpp from accessing the network.
  bool offline = false;
  /// Enables verbose mcpp output.
  bool verbose = false;
};

/// Creates the direct child process command for an mcpp project.
[[nodiscard]] std::vector<ProcessCommand> BuildCommands(const BuildOptions& options);

/// Runs the standalone `huxerui mcpp` command.
///
/// The argument span starts with `build`, and the command does not use the HuxerUI SDK or project discovery.
[[nodiscard]] int Run(std::span<const std::string_view> arguments, const std::filesystem::path& working_directory,
                      std::ostream& output, std::ostream& error);

} // namespace huxerui::cli::mcpp
