#include "mcpp.h"

#include <cstddef>
#include <filesystem>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace huxerui::cli::mcpp {
namespace {

class UsageError final : public std::invalid_argument {
public:
  using std::invalid_argument::invalid_argument;
};

void PrintHelp(std::ostream& output) {
  output << "HuxerUI standalone mcpp build frontend\n\n"
         << "Usage:\n"
         << "  huxerui mcpp build [--source <path>] [--release] [--locked] [--offline] [--verbose]\n\n"
         << "The source directory must contain mcpp.toml.\n";
}

void RequireOnce(bool& selected, std::string_view option) {
  if (selected) {
    throw UsageError(std::string(option) + " may be specified only once");
  }
  selected = true;
}

std::filesystem::path ResolveProjectRoot(const std::filesystem::path& working_directory,
                                         const std::filesystem::path& source) {
  const std::filesystem::path candidate = source.is_absolute() ? source : working_directory / source;
  const std::filesystem::path project_root = std::filesystem::absolute(candidate).lexically_normal();
  if (!std::filesystem::is_directory(project_root)) {
    throw std::runtime_error("mcpp source directory does not exist: " + project_root.string());
  }
  if (!std::filesystem::is_regular_file(project_root / "mcpp.toml")) {
    throw std::runtime_error("mcpp project is missing mcpp.toml: " + project_root.string());
  }
  return project_root;
}

BuildOptions ParseBuildOptions(std::span<const std::string_view> arguments,
                               const std::filesystem::path& working_directory) {
  BuildOptions options;
  bool source_selected = false;
  std::filesystem::path source = working_directory;
  for (std::size_t index = 1; index < arguments.size(); ++index) {
    const std::string_view argument = arguments[index];
    if (argument == "--source") {
      RequireOnce(source_selected, argument);
      if (++index == arguments.size() || arguments[index].empty() || arguments[index].starts_with('-')) {
        throw UsageError("mcpp build --source requires a directory");
      }
      source = std::filesystem::path(arguments[index]);
    } else if (argument == "--release") {
      RequireOnce(options.release, argument);
    } else if (argument == "--locked") {
      RequireOnce(options.locked, argument);
    } else if (argument == "--offline") {
      RequireOnce(options.offline, argument);
    } else if (argument == "--verbose") {
      RequireOnce(options.verbose, argument);
    } else if (argument == "--help" || argument == "-h") {
      throw UsageError("help must be requested as `huxerui mcpp --help`");
    } else {
      throw UsageError("unknown mcpp build option: " + std::string(argument));
    }
  }
  options.project_root = ResolveProjectRoot(working_directory, source);
  return options;
}

void ExecuteCommands(std::span<const ProcessCommand> commands, std::ostream& output) {
  for (const ProcessCommand& command : commands) {
    output << "> " << DescribeProcess(command) << '\n';
    output.flush();
    const int result = RunProcess(command);
    if (result != 0) {
      throw std::runtime_error(
          "mcpp command failed with exit code " + std::to_string(result) + ": " + DescribeProcess(command)
      );
    }
  }
}

} // namespace

std::vector<ProcessCommand> BuildCommands(const BuildOptions& options) {
  if (options.project_root.empty()) {
    throw std::invalid_argument("mcpp project root cannot be empty");
  }

  std::vector<std::string> arguments{"build"};
  if (options.release) {
    arguments.emplace_back("--release");
  }
  if (options.locked) {
    arguments.emplace_back("--locked");
  }
  if (options.offline) {
    arguments.emplace_back("--offline");
  }
  if (options.verbose) {
    arguments.emplace_back("--verbose");
  }
  return {ProcessCommand{"mcpp", std::move(arguments), options.project_root}};
}

int Run(std::span<const std::string_view> arguments, const std::filesystem::path& working_directory,
        std::ostream& output, std::ostream& error) {
  try {
    if (arguments.empty() || arguments[0] == "--help" || arguments[0] == "-h") {
      PrintHelp(output);
      return 0;
    }
    if (arguments[0] != "build") {
      throw UsageError("unknown mcpp command: " + std::string(arguments[0]));
    }
    if (arguments.size() == 2 && (arguments[1] == "--help" || arguments[1] == "-h")) {
      PrintHelp(output);
      return 0;
    }

    const BuildOptions options = ParseBuildOptions(arguments, working_directory);
    if (!FindExecutable("mcpp")) {
      throw std::runtime_error("mcpp executable was not found on PATH");
    }
    output << "Building mcpp project " << options.project_root.string() << " ("
           << (options.release ? "release" : "development") << ")\n";
    const std::vector<ProcessCommand> commands = BuildCommands(options);
    ExecuteCommands(commands, output);
    return 0;
  } catch (const UsageError& exception) {
    error << "huxerui mcpp: " << exception.what() << '\n';
    error << "Run 'huxerui mcpp --help' for usage.\n";
    return 2;
  } catch (const std::exception& exception) {
    error << "huxerui mcpp: " << exception.what() << '\n';
    return 1;
  }
}

} // namespace huxerui::cli::mcpp
