#include "cli.h"

#include <algorithm>
#include <array>
#include <exception>
#include <filesystem>
#include <istream>
#include <iterator>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
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

struct AgentSkillMapping {
  std::string_view id;
  AgentSkillDirectory directory;
};

constexpr std::array agent_skill_mappings{
    AgentSkillMapping{"codex", AgentSkillDirectory::Shared},
    AgentSkillMapping{"claude", AgentSkillDirectory::Claude},
    AgentSkillMapping{"antigravity", AgentSkillDirectory::Shared},
    AgentSkillMapping{"opencode", AgentSkillDirectory::Shared},
    AgentSkillMapping{"command-code", AgentSkillDirectory::Shared},
    AgentSkillMapping{"omp", AgentSkillDirectory::Shared},
    AgentSkillMapping{"dsh", AgentSkillDirectory::Shared},
    AgentSkillMapping{"zcode", AgentSkillDirectory::ZCode},
};

void PrintHelp(std::ostream& output) {
  output << "HuxerUI project and platform tool\n\n"
         << "Usage:\n"
         << "  huxerui create app <name> [--id <project-id>] [-p|--platform <platform-list>] "
            "[--agent <agent-list>]\n"
         << "  huxerui create library <name> [--namespace <cpp-namespace>] [--target <public-cmake-target>] "
            "[--id <project-id>] [-p|--platform <platform-list>] [--agent <agent-list>]\n"
         << "  huxerui platform add <platform-list>\n"
         << "  huxerui doctor [platform-list]\n"
         << "  huxerui setup <platform-list> [--yes]\n"
         << "  huxerui devices [platform]\n"
         << "  huxerui build [platform-list] [--device <id>] [--profile debug|release] [--generator <name>] "
            "[--source <path>] [--java-home <path>]\n"
         << "  huxerui run <platform> [--device <id>] [--profile debug|release] [--generator <name>] "
            "[--source <path>] [--java-home <path>]\n"
         << "  huxerui package <platform-list> [--device <id>] [--profile debug|release] [--generator <name>] "
            "[--source <path>] [--java-home <path>]\n"
         << "  huxerui open ios [--source <path>]\n"
         << "  huxerui --version\n\n"
         << "A platform list is a comma-separated list or all.\n"
         << "Without --platform, an app and a common library's Preview enable all platforms.\n"
         << "An agent list is comma-separated and supports ";
  for (std::size_t index = 0; index < agent_skill_mappings.size(); ++index) {
    if (index > 0) {
      output << ", ";
    }
    output << agent_skill_mappings[index].id;
  }
  output << ", all, or none.\n";
}

std::vector<std::string_view> SplitList(std::string_view value, std::string_view kind) {
  if (value.empty()) {
    throw UsageError(std::string(kind) + " list cannot be empty");
  }

  std::vector<std::string_view> ids;
  std::size_t start = 0;
  while (start <= value.size()) {
    const std::size_t separator = value.find(',', start);
    const std::size_t end = separator == std::string_view::npos ? value.size() : separator;
    const std::string_view id = value.substr(start, end - start);
    if (id.empty()) {
      throw UsageError(std::string(kind) + " list contains an empty " + std::string(kind));
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
  const std::vector<std::string_view> requested = SplitList(value, "platform");
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

std::vector<AgentSkillDirectory> ResolveAgentSkillDirectories(std::string_view value) {
  const std::vector<std::string_view> requested = SplitList(value, "agent");
  const bool all = std::find(requested.begin(), requested.end(), "all") != requested.end();
  const bool none = std::find(requested.begin(), requested.end(), "none") != requested.end();
  if (all || none) {
    if (requested.size() != 1) {
      throw UsageError(std::string(all ? "all" : "none") + " cannot be combined with another agent");
    }
    if (none) {
      return {};
    }
    return {AgentSkillDirectory::Shared, AgentSkillDirectory::Claude, AgentSkillDirectory::ZCode};
  }

  std::vector<AgentSkillDirectory> directories;
  for (const std::string_view id : requested) {
    const auto mapping = std::find_if(
        agent_skill_mappings.begin(), agent_skill_mappings.end(), [id](const AgentSkillMapping& candidate) {
          return candidate.id == id;
        }
    );
    if (mapping == agent_skill_mappings.end()) {
      throw UsageError("unknown agent: " + std::string(id));
    }
    if (std::find(directories.begin(), directories.end(), mapping->directory) == directories.end()) {
      directories.push_back(mapping->directory);
    }
  }
  return directories;
}

std::optional<Project> TryDiscoverProject(const std::filesystem::path& start) {
  try {
    return DiscoverProject(start);
  } catch (const std::runtime_error&) {
    return std::nullopt;
  }
}

std::vector<EnvironmentDiagnostic> DiagnoseCommonEnvironment(const SdkLocation& sdk) {
  std::vector<EnvironmentDiagnostic> diagnostics;
  if (sdk.home.empty()) {
    diagnostics.push_back({
        EnvironmentDiagnosticStatus::Missing,
        "huxerui_home",
        "HUXERUI_HOME; install HuxerUI or set HUXERUI_HOME",
        {},
    });
  } else {
    diagnostics.push_back({
        EnvironmentDiagnosticStatus::Ready,
        "huxerui_home",
        "HUXERUI_HOME (" + std::string(SdkLocationSourceName(sdk.source)) + ")",
        sdk.home.string(),
    });
  }

  const std::optional<std::filesystem::path> cmake = FindExecutable("cmake");
  diagnostics.push_back({
      cmake ? EnvironmentDiagnosticStatus::Ready : EnvironmentDiagnosticStatus::Missing,
      "cmake",
      "cmake",
      cmake ? cmake->string() : std::string{},
  });
  return diagnostics;
}

bool PrintEnvironmentDiagnostics(std::span<const EnvironmentDiagnostic> diagnostics, std::string_view indentation,
                                 std::ostream& output) {
  bool ready = true;
  for (const EnvironmentDiagnostic& diagnostic : diagnostics) {
    std::string_view status;
    switch (diagnostic.status) {
    case EnvironmentDiagnosticStatus::Ready:
      status = "ok";
      break;
    case EnvironmentDiagnosticStatus::Missing:
      status = "missing";
      ready = false;
      break;
    case EnvironmentDiagnosticStatus::Unavailable:
      status = "unavailable";
      ready = false;
      break;
    }
    output << indentation << '[' << status << "] " << diagnostic.label;
    if (!diagnostic.detail.empty()) {
      output << ": " << diagnostic.detail;
    }
    output << '\n';
  }
  return ready;
}

void ExecuteCommands(std::span<const ProcessCommand> commands, std::ostream& output);

std::vector<SetupAction> PlanCommonSetup(std::span<const EnvironmentDiagnostic> diagnostics) {
  std::vector<SetupAction> actions;
  for (const EnvironmentDiagnostic& diagnostic : diagnostics) {
    if (diagnostic.status != EnvironmentDiagnosticStatus::Missing) {
      continue;
    }
    if (diagnostic.id == "huxerui_home") {
      actions.push_back({"Install the HuxerUI SDK or set HUXERUI_HOME", std::nullopt});
    } else if (diagnostic.id == "cmake") {
      actions.push_back({"Install CMake from https://cmake.org/download/ and add it to PATH", std::nullopt});
    }
  }
  return actions;
}

void AppendUniqueActions(std::vector<SetupAction>& destination, std::vector<SetupAction> source) {
  for (SetupAction& action : source) {
    const auto duplicate = std::find_if(destination.begin(), destination.end(), [&action](const SetupAction& existing) {
      return existing.description == action.description;
    });
    if (duplicate == destination.end()) {
      destination.push_back(std::move(action));
    }
  }
}

bool DiagnosticsReady(std::span<const EnvironmentDiagnostic> diagnostics) {
  return std::all_of(diagnostics.begin(), diagnostics.end(), [](const EnvironmentDiagnostic& diagnostic) {
    return diagnostic.status == EnvironmentDiagnosticStatus::Ready;
  });
}

int RunSetup(std::span<const std::string_view> arguments, const SdkLocation& sdk, std::istream& input,
             std::ostream& output) {
  if (arguments.size() < 2) {
    throw UsageError("setup requires an explicit platform list");
  }

  bool assume_yes = false;
  std::optional<std::string_view> platform_list;
  for (std::size_t index = 1; index < arguments.size(); ++index) {
    if (arguments[index] == "--yes") {
      if (assume_yes) {
        throw UsageError("--yes may be specified only once");
      }
      assume_yes = true;
    } else if (!platform_list) {
      platform_list = arguments[index];
    } else {
      throw UsageError("unexpected setup argument: " + std::string(arguments[index]));
    }
  }
  if (!platform_list) {
    throw UsageError("setup requires an explicit platform list");
  }

  const std::vector<const PlatformDriver*> platforms = ResolvePlatforms(*platform_list);
  const std::vector<EnvironmentDiagnostic> common_diagnostics = DiagnoseCommonEnvironment(sdk);
  output << "Common environment:\n";
  PrintEnvironmentDiagnostics(common_diagnostics, "  ", output);

  bool environment_ready = DiagnosticsReady(common_diagnostics);
  std::vector<SetupAction> actions = PlanCommonSetup(common_diagnostics);
  for (const PlatformDriver* platform : platforms) {
    const std::vector<EnvironmentDiagnostic> diagnostics = platform->DiagnoseEnvironment();
    output << "Platform " << platform->Id() << ":\n";
    PrintEnvironmentDiagnostics(diagnostics, "  ", output);
    environment_ready = DiagnosticsReady(diagnostics) && environment_ready;
    AppendUniqueActions(actions, platform->PlanSetup(diagnostics));
  }

  if (environment_ready) {
    output << "Environment is ready; no setup changes are required.\n";
    return 0;
  }
  if (actions.empty()) {
    output << "No setup action is available for the requested environment.\n";
    return 1;
  }

  output << "Setup plan:\n";
  for (const SetupAction& action : actions) {
    output << "  [" << (action.command ? "command" : "manual") << "] " << action.description << '\n';
    if (action.command) {
      output << "    > " << DescribeProcess(*action.command) << '\n';
    }
  }

  if (!assume_yes) {
    output << "Continue? [y/N] ";
    output.flush();
    std::string response;
    std::getline(input, response);
    if (response != "y" && response != "Y" && response != "yes" && response != "YES") {
      output << "Setup cancelled.\n";
      return 1;
    }
  }

  for (const SetupAction& action : actions) {
    if (!action.command) {
      output << "Manual action required: " << action.description << '\n';
      continue;
    }
    ExecuteCommands(std::span<const ProcessCommand>(&*action.command, 1), output);
  }

  output << "Post-setup diagnosis:\n";
  output << "Common environment:\n";
  const std::vector<EnvironmentDiagnostic> post_common_diagnostics = DiagnoseCommonEnvironment(sdk);
  PrintEnvironmentDiagnostics(post_common_diagnostics, "  ", output);
  environment_ready = DiagnosticsReady(post_common_diagnostics);
  for (const PlatformDriver* platform : platforms) {
    const std::vector<EnvironmentDiagnostic> diagnostics = platform->DiagnoseEnvironment();
    output << "Platform " << platform->Id() << ":\n";
    PrintEnvironmentDiagnostics(diagnostics, "  ", output);
    environment_ready = DiagnosticsReady(diagnostics) && environment_ready;
  }
  if (!environment_ready) {
    output << "Environment setup is incomplete.\n";
    return 1;
  }
  output << "Environment setup is complete.\n";
  return 0;
}

int RunCreate(std::span<const std::string_view> arguments, const std::filesystem::path& working_directory,
              const std::filesystem::path& huxerui_home, std::ostream& output) {
  if (arguments.size() < 3) {
    throw UsageError("create requires app or library and a project name");
  }
  ProjectKind kind;
  if (arguments[1] == "app") {
    kind = ProjectKind::App;
  } else if (arguments[1] == "library") {
    kind = ProjectKind::Library;
  } else {
    throw UsageError("create kind must be app or library");
  }
  if (!IsValidProjectName(arguments[2])) {
    throw UsageError("project name must start with a letter and contain only letters, digits, underscores, or hyphens");
  }
  if (kind == ProjectKind::Library && !IsValidLibraryProjectName(arguments[2])) {
    throw UsageError(
        "library name must start with a letter and contain non-empty letter or digit segments separated by '-' or '_'"
    );
  }

  std::optional<std::string_view> project_id;
  std::optional<std::string_view> cpp_namespace;
  std::optional<std::string_view> public_target;
  std::optional<std::string_view> platform_list;
  std::string_view agent_list = "codex";
  bool platform_specified = false;
  bool agent_specified = false;
  if (kind == ProjectKind::App) {
    platform_list = "all";
  }
  for (std::size_t index = 3; index < arguments.size(); ++index) {
    const std::string_view argument = arguments[index];
    if (argument != "-p" && argument != "--platform" && argument != "--id" && argument != "--agent" &&
        argument != "--namespace" && argument != "--target") {
      throw UsageError("unexpected create argument: " + std::string(arguments[index]));
    }
    if (++index >= arguments.size()) {
      throw UsageError(std::string(argument) + " requires a value");
    }
    if (argument == "--id") {
      if (project_id) {
        throw UsageError("--id may be specified only once");
      }
      project_id = arguments[index];
    } else if (argument == "--namespace") {
      if (kind != ProjectKind::Library) {
        throw UsageError("--namespace is supported only for library creation");
      }
      if (cpp_namespace) {
        throw UsageError("--namespace may be specified only once");
      }
      cpp_namespace = arguments[index];
    } else if (argument == "--target") {
      if (kind != ProjectKind::Library) {
        throw UsageError("--target is supported only for library creation");
      }
      if (public_target) {
        throw UsageError("--target may be specified only once");
      }
      public_target = arguments[index];
    } else if (argument == "-p" || argument == "--platform") {
      if (platform_specified) {
        throw UsageError("--platform may be specified only once");
      }
      platform_list = arguments[index];
      platform_specified = true;
    } else {
      if (agent_specified) {
        throw UsageError("--agent may be specified only once");
      }
      agent_list = arguments[index];
      agent_specified = true;
    }
  }

  if (project_id && !IsValidProjectId(*project_id)) {
    throw UsageError("project ID must be a lowercase reverse-domain identifier with letter-prefixed segments");
  }
  if (cpp_namespace && cpp_namespace->empty()) {
    throw UsageError("--namespace requires a non-empty value");
  }
  if (public_target && public_target->empty()) {
    throw UsageError("--target requires a non-empty value");
  }
  ProjectTemplate project_template;
  try {
    project_template = kind == ProjectKind::Library
                           ? ProjectTemplate{MakeLibraryTemplateContext(arguments[2], cpp_namespace.value_or(""),
                                 public_target.value_or(""), project_id.value_or(""))}
                           : ProjectTemplate{MakeProjectTemplateContext(arguments[2], project_id.value_or(""))};
  } catch (const std::invalid_argument& error) {
    throw UsageError(error.what());
  }
  const std::vector<const PlatformDriver*> selected_platforms =
      platform_list ? ResolvePlatforms(*platform_list) : std::vector<const PlatformDriver*>{};
  std::vector<const PlatformDriver*> application_platforms = selected_platforms;
  std::vector<const PlatformDriver*> library_platforms;
  if (kind == ProjectKind::Library) {
    library_platforms = selected_platforms;
    if (!platform_specified) {
      application_platforms = ResolvePlatforms("all");
    }
  }
  const std::vector<AgentSkillDirectory> agent_skill_directories = ResolveAgentSkillDirectories(agent_list);
  const std::filesystem::path skill_source = agent_skill_directories.empty()
                                                 ? std::filesystem::path{}
                                                 : ResolveApplicationDevelopmentSkill(huxerui_home);
  const std::filesystem::path destination = working_directory / arguments[2];
  CreateProject(destination, project_template, application_platforms, library_platforms, skill_source,
                agent_skill_directories);

  output << "Created " << (kind == ProjectKind::App ? "app " : "library ") << destination.string() << '\n';
  if (kind == ProjectKind::App) {
    output << "Platforms:";
    for (const PlatformDriver* platform : application_platforms) {
      output << ' ' << platform->Id();
    }
  } else {
    output << "Library platforms:";
    for (const PlatformDriver* platform : library_platforms) {
      output << ' ' << platform->Id();
    }
    if (library_platforms.empty()) {
      output << " none";
    }
    output << "\nPreview platforms:";
    for (const PlatformDriver* platform : application_platforms) {
      output << ' ' << platform->Id();
    }
  }
  output << '\n';
  return 0;
}

int RunPlatform(std::span<const std::string_view> arguments, const std::filesystem::path& working_directory,
                std::ostream& output) {
  if (arguments.size() != 3 || arguments[1] != "add") {
    throw UsageError("platform usage: huxerui platform add <platform-list>");
  }

  const Project project = DiscoverProject(working_directory);
  const ProjectTemplate project_template = LoadProjectTemplate(project);
  const std::vector<const PlatformDriver*> platforms = ResolvePlatforms(arguments[2]);
  AddProjectPlatforms(project, project_template, platforms);

  output << "Updated platforms:";
  for (const PlatformDriver* platform : platforms) {
    output << ' ' << platform->Id();
  }
  output << '\n';
  return 0;
}

int RunDoctor(std::span<const std::string_view> arguments, const std::filesystem::path& working_directory,
              const SdkLocation& sdk, std::ostream& output) {
  if (arguments.size() > 2) {
    throw UsageError("doctor accepts at most one platform list");
  }

  bool failed = false;
  output << "HuxerUI CLI " << version << '\n';
  output << "Host: " << CurrentHostId() << '\n';
  failed = !PrintEnvironmentDiagnostics(DiagnoseCommonEnvironment(sdk), {}, output);

  std::optional<Project> project = TryDiscoverProject(working_directory);
  if (project) {
    project = ResolveApplicationProject(*project);
  }
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
    failed = !PrintEnvironmentDiagnostics(platform->DiagnoseEnvironment(), "  ", output) || failed;
    if (project) {
      for (const Diagnostic& diagnostic : platform->Diagnose(project->root / "platform" / platform->Id())) {
        output << (diagnostic.error ? "  [error] " : "  [ok] ") << diagnostic.message << '\n';
        failed = failed || diagnostic.error;
      }
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
    if (!platform->SupportsDeviceDiscovery()) {
      throw std::runtime_error("platform does not support device discovery: " + std::string(platform->Id()));
    }
    if (!platform->SupportsCurrentHost()) {
      throw std::runtime_error(
          "platform " + std::string(platform->Id()) + " is unavailable from host " + std::string(CurrentHostId())
      );
    }
    PrintDevices(*platform, platform->DiscoverDevices(), output);
  }
  return 0;
}

struct BuildOptions {
  std::optional<std::string_view> platforms;
  std::optional<std::filesystem::path> source;
  std::optional<std::filesystem::path> java_home;
  std::string profile = "debug";
  std::string device;
  std::string cmake_generator;
  std::optional<PlatformDevice> selected_device;
  bool profile_explicit = false;
};

BuildOptions ParseBuildOptions(std::span<const std::string_view> arguments, std::string_view command) {
  BuildOptions options;
  for (std::size_t index = 1; index < arguments.size(); ++index) {
    if (arguments[index] == "--profile") {
      if (++index >= arguments.size()) {
        throw UsageError("--profile requires a value");
      }
      options.profile = arguments[index];
      options.profile_explicit = true;
      if (options.profile != "debug" && options.profile != "release") {
        throw UsageError("profile must be debug or release");
      }
    } else if (arguments[index] == "--device") {
      if (++index >= arguments.size()) {
        throw UsageError("--device requires a value");
      }
      options.device = arguments[index];
    } else if (arguments[index] == "--generator") {
      if (++index >= arguments.size()) {
        throw UsageError("--generator requires a value");
      }
      options.cmake_generator = arguments[index];
    } else if (arguments[index] == "--source") {
      if (options.source) {
        throw UsageError("--source may be specified only once");
      }
      if (++index >= arguments.size()) {
        throw UsageError("--source requires a value");
      }
      options.source = std::filesystem::path(arguments[index]);
    } else if (arguments[index] == "--java-home") {
      if (options.java_home) {
        throw UsageError("--java-home may be specified only once");
      }
      if (++index >= arguments.size()) {
        throw UsageError("--java-home requires a value");
      }
      if (arguments[index].empty()) {
        throw UsageError("--java-home requires a non-empty path");
      }
      options.java_home = std::filesystem::path(arguments[index]);
    } else if (!options.platforms) {
      options.platforms = arguments[index];
    } else {
      throw UsageError("unexpected argument: " + std::string(arguments[index]));
    }
  }
  if (!command.empty() && !options.platforms) {
    throw UsageError(std::string(command) + " requires one platform");
  }
  return options;
}

std::filesystem::path ResolveAndExportBuildHome(const std::filesystem::path& sdk_home,
                                                const std::optional<std::filesystem::path>& source,
                                                const std::filesystem::path& working_directory) {
  std::filesystem::path huxerui_home;
  if (source) {
    huxerui_home = ResolveHuxerUISource(source->is_absolute() ? *source : working_directory / *source);
  } else if (sdk_home.empty()) {
    throw std::runtime_error("cannot locate HUXERUI_HOME; install HuxerUI or use --source <path>");
  } else {
    huxerui_home = sdk_home;
  }
  SetProcessEnvironmentVariable("HUXERUI_HOME", huxerui_home.string());
  return huxerui_home;
}

std::vector<const PlatformDriver*>
ResolveBuildPlatforms(const Project& project, const BuildOptions& options, std::string_view command) {
  std::vector<const PlatformDriver*> platforms;
  if (!options.platforms) {
    const PlatformDriver* current = FindPlatformDriver(CurrentHostId());
    if (!current ||
        std::find(project.platforms.begin(), project.platforms.end(), current->Id()) == project.platforms.end()) {
      throw UsageError("build requires a platform when the current host platform is not enabled");
    }
    platforms.push_back(current);
  } else if (*options.platforms == "all") {
    for (const std::string& id : project.platforms) {
      platforms.push_back(FindPlatformDriver(id));
    }
  } else {
    platforms = ResolvePlatforms(*options.platforms);
  }

  if (!command.empty() && platforms.size() != 1) {
    throw UsageError(std::string(command) + " accepts exactly one platform");
  }
  if (!options.device.empty()) {
    if (platforms.size() != 1) {
      throw UsageError("--device requires exactly one build platform");
    }
    if (!platforms.front()->SupportsDeviceDiscovery()) {
      throw UsageError("--device is not supported for platform " + std::string(platforms.front()->Id()));
    }
  }
  const bool builds_android = std::ranges::any_of(platforms, [](const PlatformDriver* platform) {
    return platform->Id() == "android";
  });
  if (options.java_home && !builds_android) {
    throw UsageError("--java-home is supported only for Android builds");
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

void ResolveJavaHome(BuildOptions& options, const std::filesystem::path& working_directory) {
  if (!options.java_home) {
    return;
  }
  const std::filesystem::path java_home =
      (options.java_home->is_absolute() ? *options.java_home : working_directory / *options.java_home)
          .lexically_normal();
  if (!std::filesystem::is_directory(java_home)) {
    throw std::runtime_error("Java home is not a directory: " + java_home.string());
  }
#if defined(_WIN32)
  constexpr std::string_view java_executable = "java.exe";
#else
  constexpr std::string_view java_executable = "java";
#endif
  if (!std::filesystem::is_regular_file(java_home / "bin" / java_executable)) {
    throw std::runtime_error("Java home does not contain bin/" + std::string(java_executable) + ": " +
                             java_home.string());
  }
  options.java_home = java_home;
}

PlatformCommandContext MakeCommandContext(const Project& project, const PlatformDriver& platform,
                                          const std::filesystem::path& huxerui_home, const BuildOptions& options) {
  std::string build_variant(platform.Id());
  if (platform.Id() == "ios") {
    build_variant +=
        options.selected_device && options.selected_device->kind == DeviceKind::Physical ? "-device" : "-simulator";
  }
  const std::filesystem::path build_directory = project.root / ".huxerui/build" / build_variant / options.profile;
  std::string cmake_generator = options.cmake_generator;
  if (platform.Id() == "windows") {
    if (!cmake_generator.empty()) {
      throw UsageError("Windows builds select an MSVC-compatible CMake generator automatically");
    }
    if (!std::filesystem::is_regular_file(build_directory / "CMakeCache.txt")) {
      cmake_generator = FindExecutable("ninja") ? "Ninja" : "NMake Makefiles";
    }
  } else if (
      platform.Id() != "android" && platform.Id() != "ios" && cmake_generator.empty() &&
      !std::filesystem::is_regular_file(build_directory / "CMakeCache.txt") &&
      !ReadEnvironmentVariable("CMAKE_GENERATOR") && FindExecutable("ninja")
  ) {
    cmake_generator = "Ninja";
  }
  return {
      project.root,
      huxerui_home,
      build_directory,
      std::move(cmake_generator),
      options.profile,
      options.selected_device,
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

void BuildPlatform(const Project& project, const PlatformDriver& platform,
                   const std::filesystem::path& huxerui_home, const BuildOptions& options, std::ostream& output) {
  output << "Building " << platform.Id() << " (" << options.profile << ")\n";
  const PlatformCommandContext context = MakeCommandContext(project, platform, huxerui_home, options);
  platform.PrepareBuildEnvironment();
  ExecuteCommands(platform.LibraryGraphCommands(context), output);
  platform.UpdateProjectIntegration(context);
  if (platform.Id() == "android" && options.java_home) {
    output << "Java home: " << options.java_home->string() << '\n';
    SetProcessEnvironmentVariable("JAVA_HOME", options.java_home->string());
  }
  ExecuteCommands(platform.BuildCommands(context), output);
}

std::optional<PlatformDevice> SelectDevice(const PlatformDriver& platform, std::string_view requested) {
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

int RunBuild(std::span<const std::string_view> arguments, const std::filesystem::path& working_directory,
             const std::filesystem::path& sdk_home, std::ostream& output) {
  BuildOptions options = ParseBuildOptions(arguments, {});
  const std::filesystem::path huxerui_home = ResolveAndExportBuildHome(sdk_home, options.source, working_directory);
  const Project project = ResolveApplicationProject(DiscoverProject(working_directory));
  if (!project.unknown_platforms.empty()) {
    throw std::runtime_error("unknown platform directory: " + project.unknown_platforms.front());
  }
  const std::vector<const PlatformDriver*> platforms = ResolveBuildPlatforms(project, options, {});
  ResolveJavaHome(options, working_directory);
  if (!options.device.empty()) {
    options.selected_device = SelectDevice(*platforms.front(), options.device);
  }
  for (const PlatformDriver* platform : platforms) {
    BuildPlatform(project, *platform, huxerui_home, options, output);
  }
  return 0;
}

int RunApplication(std::span<const std::string_view> arguments, const std::filesystem::path& working_directory,
                   const std::filesystem::path& sdk_home, std::ostream& output) {
  BuildOptions options = ParseBuildOptions(arguments, "run");
  const std::filesystem::path huxerui_home = ResolveAndExportBuildHome(sdk_home, options.source, working_directory);
  const Project project = ResolveApplicationProject(DiscoverProject(working_directory));
  if (!project.unknown_platforms.empty()) {
    throw std::runtime_error("unknown platform directory: " + project.unknown_platforms.front());
  }
  const std::vector<const PlatformDriver*> platforms = ResolveBuildPlatforms(project, options, "run");
  ResolveJavaHome(options, working_directory);
  const PlatformDriver& platform = *platforms.front();
  const std::optional<PlatformDevice> device = SelectDevice(platform, options.device);
  if (device) {
    options.selected_device = device;
    output << "Device: " << device->id;
    if (!device->name.empty()) {
      output << " (" << device->name << ')';
    }
    output << '\n';
  }
  BuildPlatform(project, platform, huxerui_home, options, output);

  output << "Running " << platform.Id() << '\n';
  const PlatformCommandContext context = MakeCommandContext(project, platform, huxerui_home, options);
  const std::vector<ProcessCommand> commands = platform.RunCommands(context);
  ExecuteCommands(commands, output);
  return 0;
}

void CopyPackageArtifacts(std::span<const PackageArtifact> artifacts,
                          const std::filesystem::path& destination_root, std::ostream& output) {
  if (artifacts.empty()) {
    throw std::runtime_error("platform did not produce any package artifacts");
  }
  std::filesystem::create_directories(destination_root);
  for (const PackageArtifact& artifact : artifacts) {
    if (artifact.destination.empty() || artifact.destination.is_absolute() ||
        std::find(artifact.destination.begin(), artifact.destination.end(), std::filesystem::path{".."}) !=
            artifact.destination.end()) {
      throw std::logic_error("platform produced an invalid package destination");
    }
    if (!std::filesystem::exists(artifact.source)) {
      throw std::runtime_error("package artifact is missing: " + artifact.source.string());
    }
    const std::filesystem::path destination = destination_root / artifact.destination;
    std::filesystem::create_directories(destination.parent_path());
    if (std::filesystem::is_directory(artifact.source)) {
      std::filesystem::copy(artifact.source, destination,
                            std::filesystem::copy_options::recursive |
                                std::filesystem::copy_options::overwrite_existing);
    } else {
      std::filesystem::copy_file(artifact.source, destination, std::filesystem::copy_options::overwrite_existing);
    }
    output << "Packaged " << destination.string() << '\n';
  }
}

int RunPackage(std::span<const std::string_view> arguments, const std::filesystem::path& working_directory,
               const std::filesystem::path& sdk_home, std::ostream& output) {
  BuildOptions options = ParseBuildOptions(arguments, "package");
  const std::filesystem::path huxerui_home = ResolveAndExportBuildHome(sdk_home, options.source, working_directory);
  if (!options.profile_explicit) {
    options.profile = "release";
  }
  const Project project = ResolveApplicationProject(DiscoverProject(working_directory));
  if (!project.unknown_platforms.empty()) {
    throw std::runtime_error("unknown platform directory: " + project.unknown_platforms.front());
  }
  const std::vector<const PlatformDriver*> platforms = ResolveBuildPlatforms(project, options, {});
  ResolveJavaHome(options, working_directory);
  if (!options.device.empty()) {
    options.selected_device = SelectDevice(*platforms.front(), options.device);
  }

  for (const PlatformDriver* platform : platforms) {
    BuildPlatform(project, *platform, huxerui_home, options, output);
    const PlatformCommandContext context = MakeCommandContext(project, *platform, huxerui_home, options);
    const std::filesystem::path destination = project.root / "dist" / platform->Id();
    std::error_code error;
    std::filesystem::remove_all(destination, error);
    if (error) {
      throw std::runtime_error("cannot replace package directory " + destination.string() + ": " + error.message());
    }
    CopyPackageArtifacts(platform->PackageArtifacts(context), destination, output);
  }
  return 0;
}

int RunOpen(std::span<const std::string_view> arguments, const std::filesystem::path& working_directory,
            const std::filesystem::path& sdk_home, std::ostream& output) {
  BuildOptions options = ParseBuildOptions(arguments, "open");
  if (!options.platforms || *options.platforms != "ios") {
    throw UsageError("open usage: huxerui open ios [--source <path>]");
  }
  if (options.profile_explicit || !options.device.empty() || !options.cmake_generator.empty() || options.java_home) {
    throw UsageError("open accepts only --source <path>");
  }
  const std::filesystem::path huxerui_home = ResolveAndExportBuildHome(sdk_home, options.source, working_directory);
  const Project project = ResolveApplicationProject(DiscoverProject(working_directory));
  if (!project.unknown_platforms.empty()) {
    throw std::runtime_error("unknown platform directory: " + project.unknown_platforms.front());
  }
  const std::vector<const PlatformDriver*> platforms = ResolveBuildPlatforms(project, options, "open");
  const PlatformDriver& platform = *platforms.front();
  if (platform.Id() != "ios") {
    throw UsageError("open currently supports ios only");
  }
  output << "Opening ios Xcode project\n";
  const PlatformCommandContext context = MakeCommandContext(project, platform, huxerui_home, options);
  ExecuteCommands(platform.LibraryGraphCommands(context), output);
  platform.UpdateProjectIntegration(context);
  ExecuteCommands(platform.OpenCommands(context), output);
  return 0;
}

} // namespace

int Run(std::span<const std::string_view> arguments, const std::filesystem::path& working_directory,
        const SdkLocation& sdk, std::istream& input, std::ostream& output, std::ostream& error) {
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
      return RunCreate(arguments, working_directory, sdk.home, output);
    }
    if (arguments[0] == "platform") {
      return RunPlatform(arguments, working_directory, output);
    }
    if (arguments[0] == "doctor") {
      return RunDoctor(arguments, working_directory, sdk, output);
    }
    if (arguments[0] == "setup") {
      return RunSetup(arguments, sdk, input, output);
    }
    if (arguments[0] == "devices") {
      return RunDevices(arguments, output);
    }
    if (arguments[0] == "build") {
      return RunBuild(arguments, working_directory, sdk.home, output);
    }
    if (arguments[0] == "run") {
      return RunApplication(arguments, working_directory, sdk.home, output);
    }
    if (arguments[0] == "package") {
      return RunPackage(arguments, working_directory, sdk.home, output);
    }
    if (arguments[0] == "open") {
      return RunOpen(arguments, working_directory, sdk.home, output);
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
