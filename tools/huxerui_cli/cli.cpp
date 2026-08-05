#include "cli.h"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <iterator>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "platform.h"
#include "process_runner.h"
#include "project.h"

namespace huxerui::cli {
namespace {

class UsageError final : public std::invalid_argument {
public:
  using std::invalid_argument::invalid_argument;
};

constexpr std::string_view version = HUXERUI_CLI_VERSION;

void PrintHelp(std::ostream& output) {
  output << "HuxerUI project and platform tool\n\n"
         << "Usage:\n"
         << "  huxerui create <name> [-p|--platform <platform-list>]\n"
         << "  huxerui platform add <platform-list>\n"
         << "  huxerui doctor [platform-list]\n"
         << "  huxerui devices [platform]\n"
         << "  huxerui build [platform-list] [--profile debug|release] [--generator <name>]\n"
         << "  huxerui run <platform> [--device <id>] [--profile debug|release] [--generator <name>]\n"
         << "  huxerui --version\n\n"
         << "A platform list is a comma-separated list or all.\n";
}

std::vector<std::string_view> SplitPlatformList(std::string_view value) {
  if (value.empty()) {
    throw UsageError("platform list cannot be empty");
  }

  std::vector<std::string_view> ids;
  std::size_t start = 0;
  while (start <= value.size()) {
    const std::size_t separator = value.find(',', start);
    const std::size_t end = separator == std::string_view::npos ? value.size() : separator;
    const std::string_view id = value.substr(start, end - start);
    if (id.empty()) {
      throw UsageError("platform list contains an empty platform");
    }
    ids.push_back(id);
    if (separator == std::string_view::npos) {
      break;
    }
    start = separator + 1;
  }
  return ids;
}

std::vector<const PlatformDriver*> ResolvePlatforms(std::string_view value) {
  const std::vector<std::string_view> requested = SplitPlatformList(value);
  if (std::find(requested.begin(), requested.end(), "all") != requested.end()) {
    if (requested.size() != 1) {
      throw UsageError("all cannot be combined with another platform");
    }
    std::vector<const PlatformDriver*> platforms;
    for (const std::string_view id : PlatformIds()) {
      platforms.push_back(FindPlatformDriver(id));
    }
    return platforms;
  }

  std::vector<const PlatformDriver*> platforms;
  for (const std::string_view id : requested) {
    const PlatformDriver* platform = FindPlatformDriver(id);
    if (!platform) {
      throw UsageError("unknown platform: " + std::string(id));
    }
    if (std::find(platforms.begin(), platforms.end(), platform) == platforms.end()) {
      platforms.push_back(platform);
    }
  }
  return platforms;
}

std::optional<Project> TryDiscoverProject(const std::filesystem::path& start) {
  try {
    return DiscoverProject(start);
  } catch (const std::runtime_error&) {
    return std::nullopt;
  }
}

bool IsExecutableFile(const std::filesystem::path& path) {
  return std::filesystem::is_regular_file(path);
}

bool FindExecutable(std::string_view name) {
  const std::optional<std::string> path_value = ReadEnvironmentVariable("PATH");
  if (!path_value) {
    return false;
  }

#if defined(_WIN32)
  constexpr char separator = ';';
  static constexpr std::string_view suffixes[]{"", ".exe", ".cmd", ".bat"};
#else
  constexpr char separator = ':';
  static constexpr std::string_view suffixes[]{""};
#endif

  const std::string_view paths(*path_value);
  std::size_t start = 0;
  while (start <= paths.size()) {
    const std::size_t delimiter = paths.find(separator, start);
    const std::size_t end = delimiter == std::string_view::npos ? paths.size() : delimiter;
    const std::filesystem::path directory = std::string(paths.substr(start, end - start));
    for (const std::string_view suffix : suffixes) {
      if (IsExecutableFile(directory / (std::string(name) + std::string(suffix)))) {
        return true;
      }
    }
    if (delimiter == std::string_view::npos) {
      break;
    }
    start = delimiter + 1;
  }
  return false;
}

int RunCreate(
    std::span<const std::string_view> arguments, const std::filesystem::path& working_directory, std::ostream& output
) {
  if (arguments.size() < 2) {
    throw UsageError("create requires a project name");
  }
  if (!IsValidProjectName(arguments[1])) {
    throw UsageError("project name must start with a letter and contain only letters, digits, underscores, or hyphens");
  }
  const ProjectTemplateContext context = MakeProjectTemplateContext(arguments[1]);
  std::string_view platform_list = "all";
  for (std::size_t index = 2; index < arguments.size(); ++index) {
    if (arguments[index] != "-p" && arguments[index] != "--platform") {
      throw UsageError("unexpected create argument: " + std::string(arguments[index]));
    }
    if (++index >= arguments.size()) {
      throw UsageError("--platform requires a value");
    }
    platform_list = arguments[index];
  }

  const std::vector<const PlatformDriver*> platforms = ResolvePlatforms(platform_list);
  const std::filesystem::path destination = working_directory / context.project_name;
  CreateProject(destination, context, platforms);

  output << "Created " << destination.string() << "\nPlatforms:";
  for (const PlatformDriver* platform : platforms) {
    output << ' ' << platform->Id();
  }
  output << '\n';
  return 0;
}

int RunPlatform(
    std::span<const std::string_view> arguments, const std::filesystem::path& working_directory, std::ostream& output
) {
  if (arguments.size() != 3 || arguments[1] != "add") {
    throw UsageError("platform usage: huxerui platform add <platform-list>");
  }

  const Project project = DiscoverProject(working_directory);
  const ProjectTemplateContext context = MakeProjectTemplateContext(project.root.filename().string());
  std::vector<const PlatformDriver*> platforms = ResolvePlatforms(arguments[2]);
  if (arguments[2] == "all") {
    platforms.erase(
        std::remove_if(
            platforms.begin(),
            platforms.end(),
            [&project](const PlatformDriver* platform) {
              return std::find(project.platforms.begin(), project.platforms.end(), platform->Id()) !=
                     project.platforms.end();
            }
        ),
        platforms.end()
    );
    if (platforms.empty()) {
      throw std::runtime_error("all available platforms are already enabled");
    }
  }
  AddProjectPlatforms(project, context, platforms);

  output << "Added platforms:";
  for (const PlatformDriver* platform : platforms) {
    output << ' ' << platform->Id();
  }
  output << '\n';
  return 0;
}

int RunDoctor(
    std::span<const std::string_view> arguments,
    const std::filesystem::path& working_directory,
    const std::filesystem::path& sdk_root,
    std::ostream& output
) {
  if (arguments.size() > 2) {
    throw UsageError("doctor accepts at most one platform list");
  }

  bool failed = false;
  output << "HuxerUI CLI " << version << '\n';
  output << "Host: " << CurrentHostId() << '\n';
  if (sdk_root.empty()) {
    output << "[missing] HuxerUI SDK; set HUXERUI_SDK_ROOT\n";
    failed = true;
  } else {
    output << "[ok] HuxerUI SDK: " << sdk_root.string() << '\n';
  }
  if (FindExecutable("cmake")) {
    output << "[ok] cmake\n";
  } else {
    output << "[missing] cmake\n";
    failed = true;
  }

  const std::optional<Project> project = TryDiscoverProject(working_directory);
  std::vector<const PlatformDriver*> platforms;
  if (!project) {
    output << "Project: not found\n";
    output << "Available platforms:";
    for (const std::string_view id : PlatformIds()) {
      output << ' ' << id;
    }
    output << '\n';
    if (arguments.size() == 2) {
      platforms = ResolvePlatforms(arguments[1]);
    }
  } else {
    output << "Project: " << project->root.string() << '\n';
    if (!std::filesystem::is_regular_file(project->root / "CMakeLists.txt")) {
      output << "[error] missing CMakeLists.txt\n";
      failed = true;
    }
    if (!std::filesystem::is_directory(project->root / "src")) {
      output << "[error] missing src directory\n";
      failed = true;
    }
    for (const std::string& id : project->unknown_platforms) {
      output << "[error] unknown platform directory: " << id << '\n';
      failed = true;
    }

    if (arguments.size() == 2 && arguments[1] != "all") {
      platforms = ResolvePlatforms(arguments[1]);
    } else {
      for (const std::string& id : project->platforms) {
        platforms.push_back(FindPlatformDriver(id));
      }
    }
    if (arguments.size() == 2) {
      for (const PlatformDriver* platform : platforms) {
        if (std::find(project->platforms.begin(), project->platforms.end(), platform->Id()) ==
            project->platforms.end()) {
          output << "[error] platform is not enabled by this project: " << platform->Id() << '\n';
          failed = true;
        }
      }
    }
  }

  for (const PlatformDriver* platform : platforms) {
    output << "Platform " << platform->Id() << ":\n";
    if (!platform->SupportsCurrentHost()) {
      output << "  [unavailable] unsupported from host " << CurrentHostId() << '\n';
      failed = true;
    }
    if (project) {
      for (const Diagnostic& diagnostic : platform->Diagnose(project->root / "platform" / platform->Id())) {
        output << (diagnostic.error ? "  [error] " : "  [ok] ") << diagnostic.message << '\n';
        failed = failed || diagnostic.error;
      }
    }
    for (const std::string_view tool : platform->RequiredTools()) {
      const bool found = FindExecutable(tool);
      output << (found ? "  [ok] " : "  [missing] ") << tool << '\n';
      failed = failed || !found;
    }
  }
  return failed ? 1 : 0;
}

void PrintDevices(const PlatformDriver& platform, std::span<const PlatformDevice> devices, std::ostream& output) {
  output << "Platform " << platform.Id() << ":\n";
  if (devices.empty()) {
    output << "  No devices found.\n";
    return;
  }
  for (const PlatformDevice& device : devices) {
    output << "  [" << DeviceStateName(device.state) << "] " << device.id;
    if (!device.name.empty()) {
      output << " (" << device.name << ')';
    }
    output << '\n';
  }
}

int RunDevices(std::span<const std::string_view> arguments, std::ostream& output) {
  if (arguments.size() > 2) {
    throw UsageError("devices accepts at most one platform");
  }

  std::vector<const PlatformDriver*> platforms;
  if (arguments.size() == 2) {
    platforms = ResolvePlatforms(arguments[1]);
    if (platforms.size() != 1) {
      throw UsageError("devices accepts exactly one platform or no platform");
    }
  } else {
    for (const std::string_view id : PlatformIds()) {
      const PlatformDriver* platform = FindPlatformDriver(id);
      if (platform->SupportsDeviceDiscovery() && platform->SupportsCurrentHost()) {
        platforms.push_back(platform);
      }
    }
  }

  if (platforms.empty()) {
    throw std::runtime_error("no device-capable platform is available from this host");
  }
  for (const PlatformDriver* platform : platforms) {
    if (!platform->SupportsCurrentHost()) {
      throw std::runtime_error(
          "platform " + std::string(platform->Id()) + " is unavailable from host " + std::string(CurrentHostId())
      );
    }
    if (!platform->SupportsDeviceDiscovery()) {
      throw std::runtime_error("platform does not support device discovery: " + std::string(platform->Id()));
    }
    PrintDevices(*platform, platform->DiscoverDevices(), output);
  }
  return 0;
}

struct BuildOptions {
  std::optional<std::string_view> platforms;
  std::string profile = "debug";
  std::string device;
  std::string cmake_generator;
};

BuildOptions ParseBuildOptions(std::span<const std::string_view> arguments, bool require_platform, bool allow_device) {
  BuildOptions options;
  for (std::size_t index = 1; index < arguments.size(); ++index) {
    if (arguments[index] == "--profile") {
      if (++index >= arguments.size()) {
        throw UsageError("--profile requires a value");
      }
      options.profile = arguments[index];
      if (options.profile != "debug" && options.profile != "release") {
        throw UsageError("profile must be debug or release");
      }
    } else if (arguments[index] == "--device") {
      if (!allow_device) {
        throw UsageError("--device is only valid for run");
      }
      if (++index >= arguments.size()) {
        throw UsageError("--device requires a value");
      }
      options.device = arguments[index];
    } else if (arguments[index] == "--generator") {
      if (++index >= arguments.size()) {
        throw UsageError("--generator requires a value");
      }
      options.cmake_generator = arguments[index];
    } else if (!options.platforms) {
      options.platforms = arguments[index];
    } else {
      throw UsageError("unexpected argument: " + std::string(arguments[index]));
    }
  }
  if (require_platform && !options.platforms) {
    throw UsageError("run requires one platform");
  }
  return options;
}

std::vector<const PlatformDriver*>
ResolveBuildPlatforms(const Project& project, const std::optional<std::string_view>& requested, bool single) {
  std::vector<const PlatformDriver*> platforms;
  if (!requested) {
    const PlatformDriver* current = FindPlatformDriver(CurrentHostId());
    if (!current ||
        std::find(project.platforms.begin(), project.platforms.end(), current->Id()) == project.platforms.end()) {
      throw UsageError("build requires a platform when the current host platform is not enabled");
    }
    platforms.push_back(current);
  } else if (*requested == "all") {
    for (const std::string& id : project.platforms) {
      platforms.push_back(FindPlatformDriver(id));
    }
  } else {
    platforms = ResolvePlatforms(*requested);
  }

  if (single && platforms.size() != 1) {
    throw UsageError("run accepts exactly one platform");
  }
  for (const PlatformDriver* platform : platforms) {
    if (std::find(project.platforms.begin(), project.platforms.end(), platform->Id()) == project.platforms.end()) {
      throw std::runtime_error("platform is not enabled by this project: " + std::string(platform->Id()));
    }
    if (!platform->SupportsCurrentHost()) {
      throw std::runtime_error(
          "platform " + std::string(platform->Id()) + " cannot be built from host " + std::string(CurrentHostId())
      );
    }
    for (const Diagnostic& diagnostic : platform->Diagnose(project.root / "platform" / platform->Id())) {
      if (diagnostic.error) {
        throw std::runtime_error("platform " + std::string(platform->Id()) + ": " + diagnostic.message);
      }
    }
  }
  return platforms;
}

PlatformCommandContext MakeCommandContext(
    const Project& project,
    const PlatformDriver& platform,
    const std::filesystem::path& sdk_root,
    const BuildOptions& options
) {
  const std::filesystem::path build_directory = project.root / ".huxerui/build" / platform.Id() / options.profile;
  std::string cmake_generator = options.cmake_generator;
  if (cmake_generator.empty() && !std::filesystem::is_regular_file(build_directory / "CMakeCache.txt") &&
      !ReadEnvironmentVariable("CMAKE_GENERATOR") && FindExecutable("ninja")) {
    cmake_generator = "Ninja";
  }
  return {
      project.root,
      sdk_root,
      build_directory,
      std::move(cmake_generator),
      options.profile,
      options.device,
  };
}

void ExecuteCommands(std::span<const ProcessCommand> commands, std::ostream& output) {
  for (const ProcessCommand& command : commands) {
    output << "> " << DescribeProcess(command) << '\n';
    output.flush();
    const int result = RunProcess(command);
    if (result != 0) {
      throw std::runtime_error(
          "command failed with exit code " + std::to_string(result) + ": " + DescribeProcess(command)
      );
    }
  }
}

void BuildPlatform(
    const Project& project,
    const PlatformDriver& platform,
    const std::filesystem::path& sdk_root,
    const BuildOptions& options,
    std::ostream& output
) {
  output << "Building " << platform.Id() << " (" << options.profile << ")\n";
  const PlatformCommandContext context = MakeCommandContext(project, platform, sdk_root, options);
  const std::vector<ProcessCommand> commands = platform.BuildCommands(context);
  ExecuteCommands(commands, output);
}

std::optional<PlatformDevice> SelectRunDevice(const PlatformDriver& platform, std::string_view requested) {
  if (!platform.SupportsDeviceDiscovery()) {
    if (!requested.empty()) {
      throw UsageError("--device is not supported for platform " + std::string(platform.Id()));
    }
    return std::nullopt;
  }

  const std::vector<PlatformDevice> devices = platform.DiscoverDevices();
  if (!requested.empty()) {
    const auto selected = std::find_if(devices.begin(), devices.end(), [requested](const PlatformDevice& device) {
      return device.id == requested;
    });
    if (selected == devices.end()) {
      throw std::runtime_error(
          "device " + std::string(requested) + " was not found for platform " + std::string(platform.Id())
      );
    }
    if (selected->state != DeviceState::Ready) {
      throw std::runtime_error("device " + selected->id + " is " + std::string(DeviceStateName(selected->state)));
    }
    return *selected;
  }

  std::vector<PlatformDevice> ready;
  std::copy_if(devices.begin(), devices.end(), std::back_inserter(ready), [](const PlatformDevice& device) {
    return device.state == DeviceState::Ready;
  });
  if (ready.empty()) {
    throw std::runtime_error(
        "no ready devices found for platform " + std::string(platform.Id()) + "; run 'huxerui devices " +
        std::string(platform.Id()) + "' for details"
    );
  }
  if (ready.size() > 1) {
    throw std::runtime_error(
        "multiple ready devices found for platform " + std::string(platform.Id()) + "; use --device <id>"
    );
  }
  return ready.front();
}

int RunBuild(
    std::span<const std::string_view> arguments,
    const std::filesystem::path& working_directory,
    const std::filesystem::path& sdk_root,
    std::ostream& output
) {
  if (sdk_root.empty()) {
    throw std::runtime_error("cannot locate the HuxerUI SDK; set HUXERUI_SDK_ROOT");
  }
  const BuildOptions options = ParseBuildOptions(arguments, false, false);
  const Project project = DiscoverProject(working_directory);
  if (!project.unknown_platforms.empty()) {
    throw std::runtime_error("unknown platform directory: " + project.unknown_platforms.front());
  }
  const std::vector<const PlatformDriver*> platforms = ResolveBuildPlatforms(project, options.platforms, false);
  for (const PlatformDriver* platform : platforms) {
    BuildPlatform(project, *platform, sdk_root, options, output);
  }
  return 0;
}

int RunApplication(
    std::span<const std::string_view> arguments,
    const std::filesystem::path& working_directory,
    const std::filesystem::path& sdk_root,
    std::ostream& output
) {
  if (sdk_root.empty()) {
    throw std::runtime_error("cannot locate the HuxerUI SDK; set HUXERUI_SDK_ROOT");
  }
  BuildOptions options = ParseBuildOptions(arguments, true, true);
  const Project project = DiscoverProject(working_directory);
  if (!project.unknown_platforms.empty()) {
    throw std::runtime_error("unknown platform directory: " + project.unknown_platforms.front());
  }
  const std::vector<const PlatformDriver*> platforms = ResolveBuildPlatforms(project, options.platforms, true);
  const PlatformDriver& platform = *platforms.front();
  const std::optional<PlatformDevice> device = SelectRunDevice(platform, options.device);
  if (device) {
    options.device = device->id;
    output << "Device: " << device->id;
    if (!device->name.empty()) {
      output << " (" << device->name << ')';
    }
    output << '\n';
  }
  BuildPlatform(project, platform, sdk_root, options, output);

  output << "Running " << platform.Id() << '\n';
  const PlatformCommandContext context = MakeCommandContext(project, platform, sdk_root, options);
  const std::vector<ProcessCommand> commands = platform.RunCommands(context);
  ExecuteCommands(commands, output);
  return 0;
}

} // namespace

int Run(
    std::span<const std::string_view> arguments,
    const std::filesystem::path& working_directory,
    const std::filesystem::path& sdk_root,
    std::ostream& output,
    std::ostream& error
) {
  try {
    if (arguments.empty() || arguments[0] == "--help" || arguments[0] == "-h") {
      PrintHelp(output);
      return 0;
    }
    if (arguments[0] == "--version") {
      output << "huxerui " << version << '\n';
      return 0;
    }
    if (arguments[0] == "create") {
      return RunCreate(arguments, working_directory, output);
    }
    if (arguments[0] == "platform") {
      return RunPlatform(arguments, working_directory, output);
    }
    if (arguments[0] == "doctor") {
      return RunDoctor(arguments, working_directory, sdk_root, output);
    }
    if (arguments[0] == "devices") {
      return RunDevices(arguments, output);
    }
    if (arguments[0] == "build") {
      return RunBuild(arguments, working_directory, sdk_root, output);
    }
    if (arguments[0] == "run") {
      return RunApplication(arguments, working_directory, sdk_root, output);
    }
    throw UsageError("unknown command: " + std::string(arguments[0]));
  } catch (const UsageError& exception) {
    error << "huxerui: " << exception.what() << '\n';
    error << "Run 'huxerui --help' for usage.\n";
    return 2;
  } catch (const std::exception& exception) {
    error << "huxerui: " << exception.what() << '\n';
    return 1;
  }
}

} // namespace huxerui::cli
