#include "cli.h"

#include <algorithm>
#include <array>
#include <exception>
#include <filesystem>
#include <fstream>
#include <istream>
#include <iterator>
#include <optional>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "platform.h"
#include "process_runner.h"
#include "project.h"
#include "prompt.h"
#include "mcpp/mcpp.h"
#include "toolchain.h"
#include "watch.h"

namespace huxerui::cli {
namespace {

class UsageError final : public std::invalid_argument {
public:
  using std::invalid_argument::invalid_argument;
};

constexpr std::string_view version = HUXERUI_CLI_VERSION;

// How the current command writes its own lines; `Run` styles it when a terminal shows them.
prompt::Console console;
constexpr std::array<std::pair<std::string_view, std::string_view>, 3> command_aliases{{
    {"b", "build"},
    {"r", "run"},
    {"pkg", "package"},
}};

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

constexpr std::string_view build_options =
    "[--build-system cmake|mcpp] [-d|--device <id>] [--config debug|release] [--generator <name>] [--sdk <path>] "
    "[--java-home <path>] [--no-shell]";
constexpr std::string_view package_format_option = " [-f|--format appimage|deb|rpm]";
constexpr std::string_view watch_option_help = "--watch rebuilds and relaunches the selected application after edits.\n";
constexpr std::string_view sdk_option_help =
    "--sdk <path> selects an installed SDK or a HuxerUI source checkout for this invocation.\n";
constexpr std::string_view sdk_commands =
    "  huxerui sdk list\n"
    "  huxerui sdk use <version> [--yes]\n"
    "  huxerui sdk update [--check] [--yes]\n";

void PrintBuildOptions(std::ostream& output) {
  output << "A project that declares both build systems builds with cmake; --build-system mcpp selects mcpp.\n"
         << "Android and iOS build through their standard shells, Gradle and Xcode; --no-shell builds and packages\n"
         << "them with the mcpp integration alone.\n"
         << sdk_option_help
         << "--config defaults to debug for build and run, and release for package.\n"
         << "--profile enables profiling in a source build selected with --sdk <path>.\n"
         << "Build, run, and package accept --abi <abi-list> for a single platform.\n"
         << "They also accept --optimization default|size|minsize and --lto on|off.\n";
}

void PrintPackageOptions(std::ostream& output) {
  output << "With Linux as the sole packaging platform, -f or --format selects appimage (default), deb, or rpm.\n";
}

void PrintBuildHelp(std::ostream& output, std::string_view command) {
  output << "Usage:\n  huxerui " << command;
  if (command == "open") {
    output << " ios [--build-system cmake|mcpp] [--sdk <path>]\n\n" << sdk_option_help;
    return;
  }
  const auto platforms = command == "build" ? "[platform-list]" : command == "run" ? "<platform>" : "<platform-list>";
  output << ' ' << platforms << ' ' << build_options;
  if (command == "package") {
    output << package_format_option;
  } else if (command == "run") {
    output << " [--watch]";
  }
  output << "\n\n";
  PrintBuildOptions(output);
  if (command == "package") {
    PrintPackageOptions(output);
  } else if (command == "run") {
    output << watch_option_help;
  }
}

/// The help as written, colored on a terminal only: the title and the headings in HuxerUI's color, each usage line's
/// command in bold, and its arguments muted. The text itself is the same everywhere, so what a script or an agent reads does not change.
void WriteHelp(std::string_view text, std::ostream& output) {
  if (!console.Styled()) {
    output << text;
    return;
  }
  using prompt::Role;
  bool first = true;
  bool aliases = false;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = text.find('\n', start);
    const std::string line(text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
    start = end == std::string_view::npos ? text.size() : end + 1;
    prompt::Line painted{{line, Role::Plain}};
    if (line.empty()) {
      aliases = false;
    } else if (first && !line.starts_with(' ') && !line.ends_with(':')) {
      painted = {{line, Role::Brand}};
    } else if (!line.starts_with(' ') && line.ends_with(':')) {
      painted = {{line, Role::Brand}};
      aliases = line == "Command aliases:";
    } else if (line.starts_with("  huxerui ")) {
      // The command is the words before the first argument, option, or placeholder.
      std::size_t command_end = 2;
      while (command_end < line.size()) {
        const std::size_t word = line.find_first_not_of(' ', command_end);
        if (word == std::string::npos || line[word] == '[' || line[word] == '<' || line[word] == '-') {
          break;
        }
        command_end = line.find(' ', word);
        if (command_end == std::string::npos) {
          command_end = line.size();
        }
      }
      painted = {{"  ", Role::Plain}, {line.substr(2, command_end - 2), Role::Strong},
                 {line.substr(command_end), Role::Muted}};
    } else if (aliases && line.starts_with("  ")) {
      const std::size_t alias_end = line.find(' ', 2);
      painted = {{"  ", Role::Plain}, {line.substr(2, alias_end - 2), Role::Strong}, {line.substr(alias_end), Role::Plain}};
    }
    console.Write(output, painted);
    first = false;
  }
}

void PrintHelp(std::ostream& output) {
  output << "HuxerUI project and platform tool\n\n"
         << "Usage:\n"
         << "  huxerui create [app <name>] [--build-system <build-system-list>] "
            "[--template <name>] [--id <project-id>] [-p|--platform <platform-list>] [--agent <agent-list>] [--yes]\n"
         << "  huxerui create [library <name>] [--build-system <build-system-list>] "
            "[--namespace <cpp-namespace>] [--target <public-cmake-target>] "
            "[--id <project-id>] [-p|--platform <platform-list>] [--agent <agent-list>] [--yes]\n"
         << "  huxerui platform add <platform-list>\n"
         << "  huxerui doctor [platform-list] [--operation build|run|package|devices] [--json]\n"
         << "  huxerui setup <platform-list> [--operation build|run|package|devices] [--yes]\n"
         << sdk_commands << "  huxerui devices [platform]\n"
         << "  huxerui build [platform-list] " << build_options << '\n'
         << "  huxerui run <platform> " << build_options << " [--watch]\n"
         << "  huxerui package <platform-list> " << build_options << package_format_option << '\n'
         << "  huxerui open ios [--build-system cmake|mcpp] [--sdk <path>]\n"
         << "  huxerui --version\n\n";
  output << "Command aliases:\n";
  for (const auto& [alias, command] : command_aliases) {
    output << "  " << alias << "  " << command << '\n';
  }
  output << "\nA platform list is a comma-separated list or all.\n";
  PrintBuildOptions(output);
  output << watch_option_help;
  PrintPackageOptions(output);
  output << "create asks for a missing kind, name, build system, template, platform list, or agent list when\n"
         << "run in a terminal, and confirms before writing; --yes, or no terminal, applies the defaults instead.\n"
         << "A build-system list is cmake, mcpp, or cmake,mcpp. mcpp alone creates a C++ modules project\n"
         << "(import huxerui;) and a list naming cmake a headers project (#include <huxerui/huxerui.h>); a library\n"
         << "naming both is also a module, whose interface the mcpp build generates from its headers.\n"
         << "An app defaults to cmake and a library to both.\n"
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

/// What a build system takes from the host to run at all. CMake builds against a HuxerUI SDK with the host's CMake;
/// mcpp takes the framework from its manifest and needs only itself, installed through xlings.
std::vector<EnvironmentDiagnostic> EngineChecks(BuildSystem build_system, const SdkLocation& sdk, bool explicit_sdk) {
  if (build_system == BuildSystem::Mcpp) {
    return {detail::ResolveMcpp(), detail::ResolveXlings()};
  }
  std::vector<EnvironmentDiagnostic> diagnostics;
  if (sdk.home.empty()) {
    diagnostics.push_back({
        EnvironmentDiagnosticStatus::Missing,
        "huxerui_home",
        "HUXERUI_HOME; install HuxerUI or set HUXERUI_HOME",
        {},
    });
  } else {
    auto status = EnvironmentDiagnosticStatus::Ready;
    std::string detail = sdk.home.string();
    try {
      static_cast<void>(ResolveBuildHome(sdk.home, explicit_sdk ? std::optional(sdk.home) : std::nullopt, {}));
    } catch (const std::runtime_error& error) {
      status = EnvironmentDiagnosticStatus::Missing;
      detail += "; " + std::string(error.what());
    }
    diagnostics.push_back({
        status,
        "huxerui_home",
        "HUXERUI_HOME (" + std::string(SdkLocationSourceName(sdk.source)) + ")",
        std::move(detail),
    });
  }
  diagnostics.push_back(detail::ResolveCMake());
  return diagnostics;
}

/// The engine checks of the build system an operation reaches without --build-system, which are required, followed
/// by those of the other build systems the root declares, which are reported as optional with how to select them.
/// Outside a project the default is CMake and every other build system is the alternative.
std::vector<EnvironmentDiagnostic> DiagnoseCommonEnvironment(const SdkLocation& sdk, bool explicit_sdk,
                                                             BuildSystem build_system,
                                                             std::span<const BuildSystem> alternatives) {
  std::vector<EnvironmentDiagnostic> diagnostics = EngineChecks(build_system, sdk, explicit_sdk);
  for (const BuildSystem alternative : alternatives) {
    for (EnvironmentDiagnostic& check : EngineChecks(alternative, sdk, explicit_sdk)) {
      if (check.required) {
        check.detail += std::string(check.detail.empty() ? "" : "  ") + "(for --build-system " +
                        std::string(BuildSystemName(alternative)) + ")";
        check.required = false;
      }
      diagnostics.push_back(std::move(check));
    }
  }
  return diagnostics;
}

void PrintEnvironmentDiagnostics(std::span<const EnvironmentDiagnostic> diagnostics, std::string_view indentation,
                                 std::ostream& output) {
  for (const EnvironmentDiagnostic& diagnostic : diagnostics) {
    std::string_view status;
    switch (diagnostic.status) {
    case EnvironmentDiagnosticStatus::Ready:
      status = "ok";
      break;
    case EnvironmentDiagnosticStatus::Missing:
      status = "missing";
      break;
    case EnvironmentDiagnosticStatus::Blocked:
      status = "blocked";
      break;
    case EnvironmentDiagnosticStatus::Unavailable:
      status = "unavailable";
      break;
    }
    output << indentation << '[' << status << "] " << diagnostic.label;
    if (!diagnostic.required)
      output << " (optional)";
    if (!diagnostic.detail.empty()) {
      output << ": " << diagnostic.detail;
    }
    output << '\n';
  }
}

std::string JoinPlatformIds(std::span<const PlatformDriver* const> platforms) {
  std::string joined;
  for (const PlatformDriver* platform : platforms) {
    joined += (joined.empty() ? "" : ",") + std::string(platform->Id());
  }
  return joined;
}

std::string JoinBuildSystems(std::span<const BuildSystem> build_systems, std::string_view separator) {
  std::string joined;
  for (const BuildSystem build_system : build_systems) {
    joined += (joined.empty() ? "" : std::string(separator)) + std::string(BuildSystemName(build_system));
  }
  return joined;
}

template <typename T>
T RequireAnswer(std::optional<T> answer) {
  if (!answer) {
    throw std::runtime_error("project creation was cancelled");
  }
  return std::move(*answer);
}

int RunCreate(std::span<const std::string_view> arguments, const std::filesystem::path& working_directory,
              const std::filesystem::path& huxerui_home, prompt::Prompter* prompter, std::ostream& output) {
  std::optional<ProjectKind> kind;
  std::optional<std::string> name;
  std::optional<std::string_view> project_id;
  std::optional<std::string_view> cpp_namespace;
  std::optional<std::string_view> public_target;
  std::optional<std::string_view> platform_list;
  std::optional<std::string_view> agent_list;
  std::vector<BuildSystem> build_systems;
  std::optional<std::string_view> template_name;
  std::string prompted_platforms;
  bool assume_defaults = false;
  for (std::size_t index = 1; index < arguments.size(); ++index) {
    const std::string_view argument = arguments[index];
    if (argument == "--yes") {
      if (assume_defaults) {
        throw UsageError("--yes may be specified only once");
      }
      assume_defaults = true;
      continue;
    }
    if (!argument.starts_with('-')) {
      if (!kind) {
        if (argument == "app") {
          kind = ProjectKind::App;
        } else if (argument == "library") {
          kind = ProjectKind::Library;
        } else {
          throw UsageError("create kind must be app or library");
        }
      } else if (!name) {
        name = std::string(argument);
      } else {
        throw UsageError("unexpected create argument: " + std::string(argument));
      }
      continue;
    }
    if (argument != "-p" && argument != "--platform" && argument != "--id" && argument != "--agent" &&
        argument != "--namespace" && argument != "--target" && argument != "--build-system" &&
        argument != "--template") {
      throw UsageError("unexpected create argument: " + std::string(argument));
    }
    if (++index >= arguments.size()) {
      throw UsageError(std::string(argument) + " requires a value");
    }
    const std::string_view value = arguments[index];
    const auto once = [&](auto& destination, std::string_view option) {
      if (destination) {
        throw UsageError(std::string(option) + " may be specified only once");
      }
    };
    if (argument == "--id") {
      once(project_id, argument);
      project_id = value;
    } else if (argument == "--namespace") {
      once(cpp_namespace, argument);
      cpp_namespace = value;
    } else if (argument == "--target") {
      once(public_target, argument);
      public_target = value;
    } else if (argument == "-p" || argument == "--platform") {
      once(platform_list, "--platform");
      platform_list = value;
    } else if (argument == "--agent") {
      once(agent_list, argument);
      agent_list = value;
    } else if (argument == "--template") {
      once(template_name, argument);
      template_name = value;
    } else {
      if (!build_systems.empty()) {
        throw UsageError("--build-system may be specified only once");
      }
      for (const std::string_view id : SplitList(value, "build system")) {
        const std::optional<BuildSystem> build_system = ParseBuildSystem(id);
        if (!build_system) {
          throw UsageError("unknown build system: " + std::string(id) + "; expected cmake or mcpp");
        }
        build_systems.push_back(*build_system);
      }
    }
  }
  if (assume_defaults) {
    prompter = nullptr;
  }
  // The questions open under one title, written before the first of them.
  bool introduced = false;
  const auto ask = [&]() -> prompt::Prompter& {
    if (!introduced) {
      const std::array intro{prompt::IntroLine("create a project", prompter->OutputStyle())};
      prompter->Show(intro);
      introduced = true;
    }
    return *prompter;
  };

  if (!kind) {
    if (prompter == nullptr) {
      throw UsageError("create requires app or library and a project name");
    }
    const std::array<prompt::Choice, 2> kinds{{
        {"app", "an application"},
        {"library", "a reusable library with a Preview application"},
    }};
    kind = RequireAnswer(ask().Select("Project kind", kinds, 0)) == 0 ? ProjectKind::App : ProjectKind::Library;
  }
  const auto name_problem = [&kind](std::string_view candidate) -> std::string {
    if (!IsValidProjectName(candidate)) {
      return "project name must start with a letter and contain only letters, digits, underscores, or hyphens";
    }
    if (kind == ProjectKind::Library && !IsValidLibraryProjectName(candidate)) {
      return "library name must start with a letter and contain non-empty letter or digit segments separated by '-' "
             "or '_'";
    }
    return {};
  };
  if (!name) {
    if (prompter == nullptr) {
      throw UsageError("create requires app or library and a project name");
    }
    name = RequireAnswer(ask().Text("Project name", kind == ProjectKind::App ? "hello-hui" : "hello-hui-lib",
                                    name_problem));
  }
  if (const std::string problem = name_problem(*name); !problem.empty()) {
    throw UsageError(problem);
  }
  if (kind != ProjectKind::Library && (cpp_namespace || public_target)) {
    throw UsageError(std::string(cpp_namespace ? "--namespace" : "--target") +
                     " is supported only for library creation");
  }

  if (prompter != nullptr && build_systems.empty()) {
    // One question selects both the build systems and, through them, the source style. A library declaring both
    // keeps its headers and gains a module interface over them, so every consumer can use it, which is why it is the
    // library's recommendation; an application has no importer, so it stays headers.
    using prompt::Role;
    const prompt::Span headers{"headers", Role::Headers};
    const prompt::Span modules{"C++ modules", Role::Modules};
    const prompt::Line both = kind == ProjectKind::Library
                                  ? prompt::Line{headers, {" & ", Role::Plain}, modules,
                                                 {"  (#include & import, recommended)", Role::Muted}}
                                  : prompt::Line{headers, {"  (#include <huxerui/huxerui.h>)", Role::Muted}};
    const std::array<prompt::Choice, 3> choices{{
        {"cmake", prompt::Line{headers, {"  (#include <huxerui/huxerui.h>)", Role::Muted}}},
        {"mcpp", prompt::Line{modules, {"  (import huxerui;)", Role::Muted}}},
        {"cmake + mcpp", both},
    }};
    const std::size_t answer =
        RequireAnswer(ask().Select("Build system", choices, kind == ProjectKind::App ? 0 : 2));
    if (answer != 1) {
      build_systems.push_back(BuildSystem::CMake);
    }
    if (answer != 0) {
      build_systems.push_back(BuildSystem::Mcpp);
    }
  }
  const CreateSelection selection = ResolveCreateSelection(*kind, build_systems);

  if (template_name && selection.style != SourceStyle::Modules) {
    throw UsageError("--template selects C++ modules content from templates/; it needs --build-system mcpp");
  }
  if (template_name) {
    const bool known = kind == ProjectKind::App ? *template_name == "app" || *template_name == "navigation"
                                                : *template_name == "library";
    if (!known) {
      throw UsageError("unknown template: " + std::string(*template_name) + "; expected " +
                       (kind == ProjectKind::App ? "app or navigation" : "library"));
    }
  }
  if (prompter != nullptr && !template_name && kind == ProjectKind::App && selection.style == SourceStyle::Modules) {
    const std::array<prompt::Choice, 2> templates{{
        {"app", "a page with a counter"},
        {"navigation", "pages on a NavigationStack"},
    }};
    template_name = RequireAnswer(ask().Select("Template", templates, 0)) == 0 ? "app" : "navigation";
  }
  if (prompter != nullptr && !platform_list && kind == ProjectKind::App) {
    // Every platform is on, as without --platform; the hints state only what building a platform needs.
    std::vector<prompt::Choice> choices;
    std::vector<std::size_t> all;
    for (const std::string_view id : PlatformIds()) {
      all.push_back(choices.size());
      const std::string_view hint = id == "macos" || id == "ios" ? "builds on a Mac"
                                    : id == "web"                ? "WebAssembly"
                                    : id == "android"            ? "an APK through Gradle"
                                                                 : "";
      choices.push_back({std::string(id), std::string(hint)});
    }
    for (const std::size_t index : RequireAnswer(ask().MultiSelect("Platforms", choices, all))) {
      prompted_platforms += (prompted_platforms.empty() ? "" : ",") + choices[index].label;
    }
    platform_list = prompted_platforms;
  }
  const bool platform_specified = platform_list.has_value();

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
                           ? ProjectTemplate{MakeLibraryTemplateContext(*name, cpp_namespace.value_or(""),
                                 public_target.value_or(""), project_id.value_or(""))}
                           : ProjectTemplate{MakeProjectTemplateContext(*name, project_id.value_or(""))};
  } catch (const std::invalid_argument& error) {
    throw UsageError(error.what());
  }
  std::vector<const PlatformDriver*> application_platforms;
  std::vector<const PlatformDriver*> library_platforms;
  if (kind == ProjectKind::App) {
    application_platforms = ResolvePlatforms(platform_list.value_or("all"));
  } else {
    application_platforms = ResolvePlatforms(platform_list.value_or("all"));
    if (platform_specified) {
      library_platforms = application_platforms;
    }
  }
  std::string prompted_agents;
  if (prompter != nullptr && !agent_list) {
    // The skill is looked for before the question, so an agent is not offered only to fail after the last answer.
    bool skill_found = true;
    try {
      static_cast<void>(ResolveApplicationDevelopmentSkill(huxerui_home));
    } catch (const std::runtime_error&) {
      skill_found = false;
    }
    std::vector<prompt::Choice> choices;
    for (const AgentSkillMapping& mapping : agent_skill_mappings) {
      choices.push_back({std::string(mapping.id)});
    }
    std::vector<std::size_t> checked;
    if (skill_found) {
      checked.push_back(0);
    } else {
      const std::array note{prompt::NoteLine(
          {{"No agent skill is found; set HUXERUI_HOME to a HuxerUI SDK or source checkout to add one.",
            prompt::Role::Muted}},
          prompter->OutputStyle())};
      ask().Show(note);
    }
    for (const std::size_t index : RequireAnswer(ask().MultiSelect("Agent skills", choices, checked, 0))) {
      prompted_agents += (prompted_agents.empty() ? "" : ",") + choices[index].label;
    }
    agent_list = prompted_agents.empty() ? std::string_view("none") : std::string_view(prompted_agents);
  }
  const std::vector<AgentSkillDirectory> agent_skill_directories =
      ResolveAgentSkillDirectories(agent_list.value_or("codex"));
  const std::filesystem::path skill_source = agent_skill_directories.empty()
                                                 ? std::filesystem::path{}
                                                 : ResolveApplicationDevelopmentSkill(huxerui_home);
  const std::filesystem::path destination = working_directory / *name;
  if (introduced && !RequireAnswer(prompter->Confirm("Create " + *name + "?", true))) {
    const std::array declined{prompt::NoteLine({{"Nothing was created.", prompt::Role::Muted}},
                                               prompter->OutputStyle())};
    prompter->Show(declined);
    return 0;
  }
  CreateProject(destination, project_template, application_platforms, library_platforms, skill_source,
                agent_skill_directories, selection, huxerui_home, template_name.value_or(std::string_view{}));

  // The selections as options, so the same project can be created again without prompts.
  std::string command = "huxerui create " + std::string(kind == ProjectKind::App ? "app " : "library ") + *name +
                        " --build-system " + JoinBuildSystems(selection.build_systems, ",");
  if (template_name) {
    command += " --template " + std::string(*template_name);
  }
  if (platform_specified || kind == ProjectKind::App) {
    command += " --platform " + JoinPlatformIds(application_platforms);
  }
  if (project_id) {
    command += " --id " + std::string(*project_id);
  }
  if (cpp_namespace) {
    command += " --namespace " + std::string(*cpp_namespace);
  }
  if (public_target) {
    command += " --target " + std::string(*public_target);
  }
  if (agent_list) {
    command += " --agent " + std::string(*agent_list);
  }
  if (!introduced) {
    output << "Created " << (kind == ProjectKind::App ? "app " : "library ") << destination.string() << '\n';
    output << "Style: " << CreateStyleName(selection) << '\n';
    output << "Build systems: " << JoinBuildSystems(selection.build_systems, ", ") << '\n';
    if (kind == ProjectKind::App) {
      output << "Platforms:";
      for (const PlatformDriver* platform : application_platforms) {
        output << ' ' << platform->Id();
      }
      output << '\n';
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
      output << '\n';
    }
    output << "Command: " << command << '\n';
  }
  if (introduced) {
    // After questions, the outcome closes them: where the project is, what to run next, and the same choices as
    // options.
    using prompt::Role;
    const std::string host(CurrentHostId());
    const auto runs_here = std::ranges::find_if(application_platforms, [&host](const PlatformDriver* platform) {
      return platform->Id() == host;
    });
    const std::string first = application_platforms.empty()
                                  ? host
                                  : std::string((runs_here != application_platforms.end() ? *runs_here
                                                                                          : application_platforms.front())
                                                    ->Id());
    const std::string directory = kind == ProjectKind::App ? *name : *name + "/examples/preview";
    std::vector<std::pair<std::string, prompt::Line>> details{
        {"next", {{"cd " + directory + " && huxerui run " + first, Role::Accent}}},
    };
    // The default for a project declaring both is CMake, which needs an SDK and system packages; mcpp does not, but
    // needs itself.
    const bool mcpp_ready = !selection.Has(BuildSystem::Mcpp) ||
                            detail::ResolveMcpp().status == EnvironmentDiagnosticStatus::Ready;
    if (selection.build_systems.size() > 1 && mcpp_ready) {
      details.push_back({"or", {{"huxerui run " + first + " --build-system mcpp", Role::Accent},
                                {"  (fetches its own toolchain)", Role::Muted}}});
    }
    details.push_back({"same as", {{command, Role::Muted}}});
    if (!mcpp_ready) {
      details.push_back({"note", {{"mcpp is unavailable; 'huxerui doctor' shows how to install it", Role::Muted}}});
    }
    if (const auto* library = std::get_if<LibraryTemplateContext>(&project_template);
        library != nullptr && selection.module_interface) {
      details.push_back({"note", {{"modules/" + ModuleInterfaceName(*library) +
                                       ".cppm is generated from the headers by the mcpp build",
                                   Role::Muted}}});
    }
    prompter->Show(prompt::OutroLines(
        {{"Created ", Role::Plain}, {std::string(kind == ProjectKind::App ? "app " : "library ") + "./" + *name,
                                     Role::Strong}},
        details, prompter->OutputStyle()));
  }
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

struct EnvironmentOptions {
  std::optional<std::string_view> platforms;
  std::optional<std::filesystem::path> sdk;
  EnvironmentRequest request = {};
  std::vector<std::string_view> abis;
  bool json = false;
  bool yes = false;
  /// The build system the project's operations dispatch to by default; empty outside a project.
  std::optional<BuildSystem> build_system;
  /// The build systems the project's root declares.
  std::vector<BuildSystem> declared;
};

constexpr std::array operation_names{"build", "run", "package", "devices"};

EnvironmentOptions ParseEnvironmentOptions(std::span<const std::string_view> arguments, bool setup,
                                           const std::filesystem::path& directory) {
  EnvironmentOptions options;
  bool operation_explicit = false;
  for (std::size_t index = 1; index < arguments.size(); ++index) {
    const auto option = arguments[index];
    const auto value = [&] {
      if (++index == arguments.size() || arguments[index].empty() || arguments[index].starts_with("--")) {
        throw UsageError(std::string(option) + " requires a value");
      }
      return arguments[index];
    };
    if (option == "--operation" && !operation_explicit) {
      operation_explicit = true;
      const auto operation = value();
      const auto position = std::ranges::find(operation_names, operation);
      if (position == operation_names.end())
        throw UsageError("unknown operation: " + std::string(operation));
      options.request.operation = static_cast<EnvironmentOperation>(position - operation_names.begin());
    } else if (option == "--json" && !setup && !options.json) {
      options.json = true;
    } else if (option == "--yes" && setup && !options.yes) {
      options.yes = true;
    } else if (option == "--sdk" && !options.sdk) {
      options.sdk = value();
    } else if (option == "--java-home" && options.request.java_home.empty()) {
      options.request.java_home = std::filesystem::absolute(directory / value());
    } else if (option == "--generator" && options.request.generator.empty()) {
      options.request.generator = value();
    } else if ((option == "--device" || option == "-d") && options.request.device.empty()) {
      options.request.device = value();
    } else if ((option == "--format" || option == "-f") && options.request.package_format.empty()) {
      options.request.package_format = value();
      if (options.request.package_format != "appimage" && options.request.package_format != "deb" &&
          options.request.package_format != "rpm")
        throw UsageError("unknown Linux package format");
    } else if (option == "--abi" && options.abis.empty()) {
      options.abis = SplitList(value(), "ABI");
    } else if (!option.starts_with('-') && !options.platforms) {
      options.platforms = option;
    } else {
      throw UsageError("unexpected environment option: " + std::string(option));
    }
  }
  if (setup && !options.platforms)
    throw UsageError("setup requires an explicit platform list");
  return options;
}

struct EnvironmentTarget {
  std::string platform;
  std::vector<EnvironmentDiagnostic> checks;
  /// The labels of the host-toolchain checks the build system provides instead, which the plan leaves out.
  std::vector<std::string> provided = {};
};

struct EnvironmentPlan {
  std::vector<EnvironmentDiagnostic> common;
  std::vector<EnvironmentTarget> targets;
  std::vector<SetupAction> actions;
  /// The build system that provisions its own toolchain, when the plan is for one.
  std::string provider = {};
};

bool DiagnosticsReady(std::span<const EnvironmentDiagnostic> diagnostics) {
  return std::ranges::all_of(diagnostics, [](const auto& check) {
    return !check.required || check.status == EnvironmentDiagnosticStatus::Ready;
  });
}

bool EnvironmentReady(const EnvironmentPlan& plan) {
  return DiagnosticsReady(plan.common) &&
         std::ranges::all_of(plan.targets, [](const auto& target) { return DiagnosticsReady(target.checks); });
}

std::vector<const PlatformDriver*> EnvironmentPlatforms(const EnvironmentOptions& options,
                                                        const std::optional<Project>& project) {
  if (options.platforms && *options.platforms != "all")
    return ResolvePlatforms(*options.platforms);
  std::vector<const PlatformDriver*> platforms;
  if (!options.platforms && !project && options.request.operation != EnvironmentOperation::Devices)
    return platforms;
  for (const auto id : PlatformIds()) {
    const auto* driver = FindPlatformDriver(id);
    if (!driver->SupportsCurrentHost() ||
        (options.request.operation == EnvironmentOperation::Devices && !driver->SupportsDeviceDiscovery())) {
      continue;
    }
    if (!project || std::ranges::find(project->platforms, id) != project->platforms.end())
      platforms.push_back(driver);
  }
  return platforms;
}

/// The plan for an operation dispatched to `build_system`, or outside a project when it is empty; `declared` are the
/// root's build systems, whose others are reported as alternatives.
EnvironmentPlan CreateEnvironmentPlan(const SdkLocation& sdk, EnvironmentRequest request,
                                      std::span<const PlatformDriver* const> platforms, bool explicit_sdk,
                                      std::optional<BuildSystem> build_system,
                                      std::span<const BuildSystem> declared = {}) {
  EnvironmentPlan plan;
  request.provisioned_toolchain = build_system && ProvisionsToolchain(*build_system);
  if (request.provisioned_toolchain) {
    plan.provider = BuildSystemName(*build_system);
  }
  const bool applicable = platforms.empty() || std::ranges::any_of(platforms, [](const auto* platform) {
                            return platform->SupportsCurrentHost();
                          });
  if (applicable && request.operation != EnvironmentOperation::Devices) {
    const BuildSystem engine = build_system.value_or(BuildSystem::CMake);
    std::vector<BuildSystem> alternatives;
    for (const BuildSystem candidate : {BuildSystem::CMake, BuildSystem::Mcpp}) {
      const bool declared_here = !build_system || std::ranges::find(declared, candidate) != declared.end();
      if (candidate != engine && declared_here) {
        alternatives.push_back(candidate);
      }
    }
    plan.common = DiagnoseCommonEnvironment(sdk, explicit_sdk, engine, alternatives);
    // Only a required check is set up; an alternative build system's engine is reported, never installed.
    const bool sdk_required = std::ranges::any_of(plan.common, [](const EnvironmentDiagnostic& check) {
      return check.id == "huxerui_home" && check.required;
    });
    if (sdk_required && detail::NeedsSetup(plan.common, "huxerui_home")) {
      plan.actions.push_back(
          {"Install the HuxerUI SDK or set HUXERUI_HOME", std::nullopt, "install_huxerui_sdk", {}, {"huxerui_home"}});
    }
    if (build_system == BuildSystem::Mcpp) {
      std::ranges::move(detail::PlanMcppSetup(plan.common), std::back_inserter(plan.actions));
    }
  }

  const auto cmake = std::ranges::find(plan.common, std::string_view{"cmake"}, &EnvironmentDiagnostic::id);
  if (cmake != plan.common.end())
    request.cmake_path = cmake->path;
  for (const auto* platform : platforms) {
    std::vector<EnvironmentDiagnostic> checks;
    std::vector<SetupAction> actions;
    try {
      checks = platform->DiagnoseEnvironment(request);
      actions = platform->PlanSetup(checks, request);
    } catch (const std::runtime_error& error) {
      checks.push_back({EnvironmentDiagnosticStatus::Blocked, "probe", "Platform environment probe", error.what()});
    }
    std::vector<std::string> provided;
    if (request.provisioned_toolchain) {
      // The build system brings its own compilers, libraries, and packaging tools: the host's are not checked, and an
      // action that would only install them is not proposed.
      std::vector<std::string> provisioned;
      std::erase_if(checks, [&provisioned, &provided](const EnvironmentDiagnostic& check) {
        if (check.host_toolchain) {
          provisioned.push_back(check.id);
          provided.push_back(check.label);
        }
        return check.host_toolchain;
      });
      std::erase_if(actions, [&provisioned](const SetupAction& action) {
        return !action.provides.empty() && std::ranges::all_of(action.provides, [&provisioned](const auto& id) {
          return std::ranges::find(provisioned, id) != provisioned.end();
        });
      });
    }
    // Driver-owned checks are scoped to the target. Actions may also depend on the common CMake check.
    const std::string prefix = std::string(platform->Id()) + ".";
    for (auto& check : checks)
      check.id.insert(0, prefix);
    for (auto& action : actions) {
      action.id.insert(0, prefix);
      for (auto& item : action.prerequisites) {
        if (item != "cmake")
          item.insert(0, prefix);
      }
      for (auto& item : action.provides)
        item.insert(0, prefix);
      if (std::ranges::none_of(plan.actions, [&](const auto& prior) { return prior.id == action.id; })) {
        plan.actions.push_back(std::move(action));
      }
    }
    plan.targets.push_back({std::string(platform->Id()), std::move(checks), std::move(provided)});
  }
  if (cmake != plan.common.end()) {
    // An Android target with cmake.dir supplies its own CMake check.
    cmake->required = plan.targets.empty() || std::ranges::any_of(plan.targets, [](const auto& target) {
                        return std::ranges::find(target.checks, target.platform + ".cmake",
                                                 &EnvironmentDiagnostic::id) == target.checks.end();
                      });
    if (cmake->required && cmake->status == EnvironmentDiagnosticStatus::Missing) {
      plan.actions.insert(plan.actions.begin(), detail::PlanCMakeSetup());
    }
  }
  return plan;
}

std::string JsonQuote(std::string_view text) {
  std::string result = "\"";
  constexpr std::string_view hex = "0123456789abcdef";
  for (const unsigned char character : text) {
    if (character == '"' || character == '\\')
      result += '\\';
    if (character < 32) {
      result += "\\u00";
      result += hex[character >> 4];
      result += hex[character & 15];
    } else {
      result += static_cast<char>(character);
    }
  }
  return result + '"';
}

template <typename Range, typename Write> void WriteJsonArray(std::ostream& output, const Range& values, Write write) {
  output << '[';
  bool first = true;
  for (const auto& element : values) {
    if (!std::exchange(first, false))
      output << ',';
    write(element);
  }
  output << ']';
}

std::string_view CheckStatusName(EnvironmentDiagnosticStatus status) {
  switch (status) {
  case EnvironmentDiagnosticStatus::Ready:
    return "ready";
  case EnvironmentDiagnosticStatus::Missing:
    return "missing";
  case EnvironmentDiagnosticStatus::Blocked:
    return "blocked";
  case EnvironmentDiagnosticStatus::Unavailable:
    return "unsupported";
  }
  return {};
}

std::string_view LibraryModulesStateName(LibraryModules::State state) {
  switch (state) {
  case LibraryModules::State::NotLibrary:
    break;
  case LibraryModules::State::Module:
    return "module";
  case LibraryModules::State::NotExported:
    return "not-exported";
  case LibraryModules::State::NoMcpp:
    return "no-mcpp";
  }
  return "none";
}

/// What doctor's header says of a library's C++ modules, or nothing for an application. It never fails the
/// diagnosis: a library that is not a module yet still builds and is still included, and a module named otherwise
/// than the documentation recommends still works.
std::optional<std::string> LibraryModulesNote(const LibraryModules& modules) {
  switch (modules.state) {
  case LibraryModules::State::NotLibrary:
    return std::nullopt;
  case LibraryModules::State::Module: {
    std::string note = "import " + modules.module;
    if (!modules.reexports.empty()) {
      std::string units;
      for (const std::string& unit : modules.reexports) {
        units += (units.empty() ? "" : ", ") + unit;
      }
      note += " (re-exports " + units + ")";
    }
    if (!modules.recommended.empty()) {
      note += "; recommended name: " + modules.recommended + ", the package";
    }
    return note;
  }
  case LibraryModules::State::NotExported:
    return "not exported; " + modules.reason;
  case LibraryModules::State::NoMcpp:
    return "not exported; an mcpp declaration builds the module from the headers";
  }
  return std::nullopt;
}

void PrintEnvironmentJson(const EnvironmentPlan& plan, EnvironmentOperation operation,
                          const std::vector<std::string>& project_errors, const LibraryModules& modules,
                          std::ostream& output) {
  const auto write_strings = [&](const auto& values) {
    WriteJsonArray(output, values, [&](const auto& value) { output << JsonQuote(value); });
  };
  const auto write_check = [&](const auto& check) {
    output << "{\"id\":" << JsonQuote(check.id) << ",\"status\":" << JsonQuote(CheckStatusName(check.status))
           << ",\"label\":" << JsonQuote(check.label);
    if (!check.detail.empty())
      output << ",\"detail\":" << JsonQuote(check.detail);
    if (!check.version.empty())
      output << ",\"version\":" << JsonQuote(check.version);
    if (!check.required_version.empty())
      output << ",\"requiredVersion\":" << JsonQuote(check.required_version);
    if (!check.path.empty())
      output << ",\"path\":" << JsonQuote(check.path.string());
    output << '}';
  };

  output << "{\"schema\":1,\"host\":{\"platform\":" << JsonQuote(CurrentHostId())
         << ",\"arch\":" << JsonQuote(detail::HostArchitecture())
         << "},\"operation\":" << JsonQuote(operation_names[static_cast<std::size_t>(operation)]) << ",\"targets\":";
  WriteJsonArray(output, plan.targets, [&](const auto& target) {
    const bool overrides_cmake =
        std::ranges::find(target.checks, target.platform + ".cmake", &EnvironmentDiagnostic::id) != target.checks.end();
    const bool common_ready = std::ranges::all_of(plan.common, [&](const auto& check) {
      return !check.required || (overrides_cmake && check.id == "cmake") ||
             check.status == EnvironmentDiagnosticStatus::Ready;
    });
    output << "{\"platform\":" << JsonQuote(target.platform)
           << ",\"ready\":" << (common_ready && DiagnosticsReady(target.checks) ? "true" : "false") << ",\"requires\":";
    std::vector<std::string> requirements;
    for (const auto& check : plan.common) {
      if (check.required && !(overrides_cmake && check.id == "cmake"))
        requirements.push_back(check.id);
    }
    for (const auto& check : target.checks)
      if (check.required)
        requirements.push_back(check.id);
    write_strings(requirements);
    output << '}';
  });
  output << ",\"checks\":[";
  bool first = true;
  const auto append_checks = [&](const auto& checks) {
    for (const auto& check : checks) {
      if (!std::exchange(first, false))
        output << ',';
      write_check(check);
    }
  };
  append_checks(plan.common);
  for (const auto& target : plan.targets)
    append_checks(target.checks);
  output << "],\"actions\":";
  WriteJsonArray(output, plan.actions, [&](const auto& action) {
    output << "{\"id\":" << JsonQuote(action.id)
           << ",\"kind\":" << JsonQuote(action.command || action.install ? "automatic" : "manual")
           << ",\"description\":" << JsonQuote(action.description) << ",\"requires\":";
    write_strings(action.prerequisites);
    output << ",\"provides\":";
    write_strings(action.provides);
    if (action.interactive)
      output << ",\"interactive\":true";
    output << '}';
  });
  output << ",\"projectErrors\":";
  write_strings(project_errors);
  if (modules.state != LibraryModules::State::NotLibrary) {
    output << ",\"library\":{\"modules\":" << JsonQuote(LibraryModulesStateName(modules.state));
    if (modules.state == LibraryModules::State::Module) {
      output << ",\"module\":" << JsonQuote(modules.module) << ",\"reexports\":";
      write_strings(modules.reexports);
      if (!modules.recommended.empty())
        output << ",\"recommended\":" << JsonQuote(modules.recommended);
    }
    output << '}';
  }
  output << "}\n";
}

std::string JoinLabels(std::span<const std::string> labels) {
  std::string joined;
  for (const std::string& label : labels) {
    joined += (joined.empty() ? "" : ", ") + label;
  }
  return joined;
}

/// The plan as a terminal shows it: one titled group per scope, each requirement marked by its state, and the
/// requirements a build system provides itself named as provided.
void PrintStyledEnvironmentPlan(const EnvironmentPlan& plan, std::ostream& output) {
  using Mark = prompt::Console::Mark;
  const auto group = [&output](std::string_view title, std::span<const EnvironmentDiagnostic> checks,
                               std::span<const std::string> provided, std::string_view provider) {
    console.Gap(output);
    console.Step(output, title);
    const std::string provider_label = "provided by " + std::string(provider);
    std::size_t width = provided.empty() ? 0 : provider_label.size();
    for (const EnvironmentDiagnostic& check : checks) {
      width = std::max(width, check.label.size());
    }
    for (const EnvironmentDiagnostic& check : checks) {
      const bool ready = check.status == EnvironmentDiagnosticStatus::Ready;
      const Mark mark = ready                                              ? Mark::Ready
                        : !check.required || check.status == EnvironmentDiagnosticStatus::Unavailable ? Mark::Optional
                        : check.status == EnvironmentDiagnosticStatus::Blocked ? Mark::Blocked
                                                                               : Mark::Missing;
      // Optional matters only for what is not ready; a ready optional tool reads as any ready tool.
      std::string detail = check.detail;
      if (!check.required && !ready) {
        detail += detail.empty() ? "(optional)" : "  (optional)";
      }
      console.Item(output, mark, check.label, detail, width);
    }
    if (!provided.empty()) {
      // A summary, so it may be cut to the terminal's width; the plain report names every check.
      console.Item(output, Mark::Ready, provider_label,
                   std::to_string(provided.size()) + (provided.size() == 1 ? " check: " : " checks: ") +
                       JoinLabels(provided),
                   width, true);
    }
  };
  if (!plan.common.empty()) {
    group("Common environment", plan.common, {}, {});
  }
  for (const auto& target : plan.targets) {
    group("Platform " + target.platform, target.checks, target.provided, plan.provider);
  }
  if (!plan.actions.empty()) {
    console.Gap(output);
    console.Step(output, "Setup plan");
    for (const auto& action : plan.actions) {
      console.Item(output, Mark::Action, action.command || action.install ? "automatic" : "manual",
                   action.description, std::string_view{"automatic"}.size());
    }
  }
}

void PrintEnvironmentPlan(const EnvironmentPlan& plan, std::ostream& output) {
  if (console.Styled()) {
    PrintStyledEnvironmentPlan(plan, output);
    return;
  }
  if (!plan.common.empty()) {
    output << "Common environment:\n";
    PrintEnvironmentDiagnostics(plan.common, "  ", output);
  }
  for (const auto& target : plan.targets) {
    output << "Platform " << target.platform << ":\n";
    PrintEnvironmentDiagnostics(target.checks, "  ", output);
    if (!target.provided.empty()) {
      output << "  [provided] by " << plan.provider << ": " << JoinLabels(target.provided) << '\n';
    }
  }
  if (!plan.actions.empty()) {
    output << "Setup plan:\n";
    for (const auto& action : plan.actions) {
      output << "  [" << (action.command || action.install ? "automatic" : "manual") << "] " << action.description
             << '\n';
    }
  }
}

bool PlanCheckReady(const EnvironmentPlan& plan, std::string_view id) {
  const auto ready = [id](const auto& checks) {
    return std::ranges::any_of(checks, [id](const auto& check) {
      return check.id == id && check.status == EnvironmentDiagnosticStatus::Ready;
    });
  };
  return ready(plan.common) ||
         std::ranges::any_of(plan.targets, [&](const auto& target) { return ready(target.checks); });
}

int ApplyEnvironmentPlan(EnvironmentPlan plan, const SdkLocation& sdk, const EnvironmentOptions& options,
                         std::span<const PlatformDriver* const> platforms, std::istream& input, std::ostream& output) {
  if (EnvironmentReady(plan)) {
    output << "Environment is ready; no setup changes are required.\n";
    return 0;
  }
  const auto automatic = [](const auto& action) { return action.command || action.install; };
  const bool can_install = std::ranges::any_of(plan.actions, automatic);
  if (!options.yes && !plan.actions.empty()) {
    output << "Continue? [y/N] " << std::flush;
    std::string response;
    std::getline(input, response);
    if (response != "y" && response != "Y" && response != "yes" && response != "YES") {
      output << "Setup cancelled.\n";
      return 3;
    }
  }

  std::vector<std::string> attempted;
  while (can_install && !EnvironmentReady(plan)) {
    const auto next = std::ranges::find_if(plan.actions, [&](const auto& action) {
      return automatic(action) && std::ranges::find(attempted, action.id) == attempted.end() &&
             std::ranges::all_of(action.prerequisites, [&](const auto& id) { return PlanCheckReady(plan, id); });
    });
    if (next == plan.actions.end())
      break;
    const SetupAction action = *next;
    attempted.push_back(action.id);
    output << action.description << '\n' << std::flush;
    const int result = action.install ? action.install() : RunProcess(detail::WithEnvironment(*action.command, {}));
    if (result == 1602 || result == 5004 || result == 130 || result == 1223) {
      output << "Setup cancelled.\n";
      return 3;
    }
    if (result == 3010 || result == 1641) {
      output << "Restart required. Run setup again after restarting.\n";
      return 4;
    }
    if (result != 0) {
      throw std::runtime_error(action.description + " failed with exit code " + std::to_string(result));
    }

    // Rebuild the remaining actions with the tool paths and environment discovered after this installation.
    plan = CreateEnvironmentPlan(sdk, options.request, platforms, options.sdk.has_value(), options.build_system,
                                  options.declared);
    const auto unsatisfied =
        std::ranges::find_if(action.provides, [&](const auto& id) { return !PlanCheckReady(plan, id); });
    if (unsatisfied != action.provides.end()) {
      PrintEnvironmentPlan(plan, output);
      throw std::runtime_error("setup completed but " + *unsatisfied + " is still unavailable");
    }
  }

  output << "Post-setup diagnosis:\n";
  PrintEnvironmentPlan(plan, output);
  if (!EnvironmentReady(plan)) {
    output << "Environment setup is incomplete.\n";
    return 1;
  }
  output << "Environment setup is complete.\n";
  return 0;
}

int RunEnvironment(std::span<const std::string_view> arguments, const std::filesystem::path& directory, SdkLocation sdk,
                   bool setup, std::istream& input, std::ostream& output) {
  auto options = ParseEnvironmentOptions(arguments, setup, directory);
  auto project = TryDiscoverProject(directory);
  // A library is diagnosed through the Preview it runs, but its C++ modules are the library root's own.
  const std::optional<Project> discovered = project;
  if (project) {
    project = ResolveApplicationProject(*project);
    options.request.project_root = project->root;
    options.build_system = project->DefaultBuildSystem();
    options.declared = project->build_systems;
  }
  if (options.sdk) {
    sdk = {std::filesystem::absolute(directory / *options.sdk), SdkLocationSource::Environment};
  }
  options.request.sdk_home = sdk.home;
  const auto platforms = EnvironmentPlatforms(options, project);
  if (!options.request.package_format.empty() && (options.request.operation != EnvironmentOperation::Package ||
                                                  platforms.size() != 1 || platforms.front()->Id() != "linux")) {
    throw UsageError("--format requires Linux as the sole packaging platform");
  }
  if (options.request.operation == EnvironmentOperation::Devices &&
      (!options.abis.empty() || !options.request.device.empty() || !options.request.generator.empty())) {
    throw UsageError("--abi, --device and --generator apply to build operations");
  }
  if (!options.abis.empty() || !options.request.device.empty()) {
    if (platforms.size() != 1)
      throw UsageError("--abi and --device require one platform");
    if (!options.request.device.empty() && !platforms.front()->SupportsDeviceDiscovery()) {
      throw UsageError("--device requires a device-capable platform");
    }
    for (const auto abi : options.abis) {
      if (detail::NormalizeTargetAbi(*platforms.front(), abi).empty())
        throw UsageError("unsupported ABI");
    }
  }
  if (!options.request.java_home.empty() &&
      std::ranges::none_of(platforms, [](const auto* platform) { return platform->Id() == "android"; }))
    throw UsageError("--java-home requires Android");
  if (!options.request.generator.empty() && std::ranges::any_of(platforms, [](const auto* platform) {
        return platform->Id() == "android" || platform->Id() == "ios" || platform->Id() == "windows";
      }))
    throw UsageError("--generator requires platform builds that accept an explicit CMake generator");
  if (platforms.empty() && (setup || (options.platforms && !project))) {
    throw UsageError("no selected platforms are available on this host");
  }

  auto plan = CreateEnvironmentPlan(sdk, options.request, platforms, options.sdk.has_value(), options.build_system,
                                    options.declared);
  if (setup) {
    PrintEnvironmentPlan(plan, output);
    return ApplyEnvironmentPlan(std::move(plan), sdk, options, platforms, input, output);
  }

  std::vector<std::string> project_errors;
  // A library's C++ modules state, whatever build system the operation reaches: what an importer can name.
  LibraryModules modules;
  if (project && options.request.operation != EnvironmentOperation::Devices) {
    modules = DescribeLibraryModules(*discovered);
    if (!std::filesystem::is_directory(project->root / "src"))
      project_errors.push_back("missing src directory");
    for (const auto& id : project->unknown_platforms)
      project_errors.push_back("unknown platform directory: " + id);
    auto shell_platforms = platforms;
    if (!options.platforms || *options.platforms == "all") {
      shell_platforms.clear();
      for (const auto& id : project->platforms)
        shell_platforms.push_back(FindPlatformDriver(id));
    }
    for (const auto* platform : shell_platforms) {
      if (std::ranges::find(project->platforms, platform->Id()) == project->platforms.end()) {
        project_errors.push_back("platform is not enabled by this project: " + std::string(platform->Id()));
        continue;
      }
      for (const auto& check : platform->Diagnose(project->root / "platform" / platform->Id())) {
        if (check.error)
          project_errors.push_back(check.message);
      }
    }
  }
  if (options.json) {
    PrintEnvironmentJson(plan, options.request.operation, project_errors, modules, output);
  } else {
    // The checks are those of the build system a command reaches without --build-system.
    std::string build_systems;
    if (project) {
      build_systems = JoinBuildSystems(project->build_systems, ", ");
      if (project->build_systems.size() > 1) {
        build_systems += " (checking " + std::string(BuildSystemName(*options.build_system)) + ", the default)";
      }
    }
    // A library's C++ modules are part of what the project is, so they are stated with its build systems.
    const std::optional<std::string> modules_note = LibraryModulesNote(modules);
    if (console.Styled()) {
      console.Step(output, "HuxerUI CLI " + std::string(version) + " on " + std::string(CurrentHostId()));
      console.Note(output, "Project        " + (project ? project->root.string() : std::string("not found")));
      if (project) {
        console.Note(output, "Build systems  " + build_systems);
      }
      if (modules_note) {
        console.Note(output, "C++ modules    " + *modules_note);
      }
    } else {
      output << "HuxerUI CLI " << version << "\nHost: " << CurrentHostId()
             << "\nProject: " << (project ? project->root.string() : "not found") << '\n';
      if (project) {
        output << "Build systems: " << build_systems << '\n';
      }
      if (modules_note) {
        output << "C++ modules: " << *modules_note << '\n';
      }
    }
    PrintEnvironmentPlan(plan, output);
    if (!project_errors.empty() && console.Styled()) {
      console.Gap(output);
      console.Step(output, "Project");
      for (const auto& error : project_errors)
        console.Item(output, prompt::Console::Mark::Missing, error, {});
    } else {
      for (const auto& error : project_errors)
        output << "[error] " << error << '\n';
    }
  }
  return EnvironmentReady(plan) && project_errors.empty() ? 0 : 1;
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

int RunDevices(std::span<const std::string_view> arguments, const std::filesystem::path& directory,
               std::ostream& output) {
  EnvironmentRequest request;
  request.operation = EnvironmentOperation::Devices;
  if (const auto project = TryDiscoverProject(directory))
    request.project_root = ResolveApplicationProject(*project).root;
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
    PrintDevices(*platform, platform->DiscoverDevices(request), output);
  }
  return 0;
}

struct BuildOptions {
  std::optional<std::string_view> platforms;
  std::optional<std::filesystem::path> sdk_override;
  std::optional<std::filesystem::path> java_home;
  std::string profile = "debug";
  std::string device;
  std::string cmake_generator;
  std::optional<PlatformDevice> selected_device;
  bool profile_explicit = false;
  bool package = false;
  std::optional<std::string> optimization;
  std::optional<bool> lto;
  bool profiling = false;
  bool watch = false;
  std::vector<std::string> abis;
  std::optional<std::string> package_format;
  std::optional<BuildSystem> build_system;
  bool no_shell = false;
};

BuildOptions ParseBuildOptions(std::span<const std::string_view> arguments, std::string_view command) {
  BuildOptions options;
  for (std::size_t index = 1; index < arguments.size(); ++index) {
    const std::string_view option = arguments[index];
    const auto read_value = [&]() {
      if (++index == arguments.size() || arguments[index].empty() || arguments[index].starts_with('-')) {
        throw UsageError(std::string(option) + " requires a value");
      }
      return arguments[index];
    };
    if (option == "--config") {
      options.profile = read_value();
      options.profile_explicit = true;
      if (options.profile != "debug" && options.profile != "release") {
        throw UsageError("--config must be debug or release");
      }
    } else if (option == "--profile" && command != "open") {
      if (options.profiling) {
        throw UsageError("--profile may be specified only once");
      }
      options.profiling = true;
    } else if (option == "--watch" && command == "run") {
      if (options.watch) throw UsageError("--watch may be specified only once");
      options.watch = true;
    } else if (option == "--abi") {
      if (!options.abis.empty()) {
        throw UsageError("--abi may be specified only once");
      }
      const auto abis = SplitList(read_value(), "ABI");
      options.abis.assign(abis.begin(), abis.end());
    } else if (command == "package" && (option == "--format" || option == "-f")) {
      if (options.package_format) {
        throw UsageError("--format may be specified only once");
      }
      const auto format = read_value();
      if (format != "appimage" && format != "deb" && format != "rpm") {
        throw UsageError("Linux package format must be appimage, deb, or rpm");
      }
      options.package_format = format;
    } else if (option == "--optimization") {
      if (options.optimization) {
        throw UsageError("--optimization may be specified only once");
      }
      const auto optimization = read_value();
      if (optimization != "default" && optimization != "size" && optimization != "minsize") {
        throw UsageError("optimization must be default, size, or minsize");
      }
      options.optimization = optimization;
    } else if (option == "--lto") {
      if (options.lto) {
        throw UsageError("--lto may be specified only once");
      }
      const auto lto = read_value();
      if (lto != "on" && lto != "off") {
        throw UsageError("lto must be on or off");
      }
      options.lto = lto == "on";
    } else if (option == "--device" || option == "-d") {
      if (!options.device.empty()) {
        throw UsageError("--device may be specified only once");
      }
      options.device = read_value();
    } else if (option == "--build-system") {
      if (options.build_system) {
        throw UsageError("--build-system may be specified only once");
      }
      const auto name = read_value();
      options.build_system = ParseBuildSystem(name);
      if (!options.build_system) {
        throw UsageError("unknown build system: " + std::string(name) + "; expected cmake or mcpp");
      }
    } else if (option == "--no-shell" && command != "open") {
      if (options.no_shell) {
        throw UsageError("--no-shell may be specified only once");
      }
      options.no_shell = true;
    } else if (option == "--generator") {
      options.cmake_generator = read_value();
    } else if (option == "--sdk") {
      if (options.sdk_override) {
        throw UsageError("--sdk may be specified only once");
      }
      options.sdk_override = std::filesystem::path(read_value());
    } else if (option == "--java-home") {
      if (options.java_home) {
        throw UsageError("--java-home may be specified only once");
      }
      options.java_home = std::filesystem::path(read_value());
    } else if (option.starts_with('-')) {
      throw UsageError("unknown option: " + std::string(option));
    } else if (!options.platforms) {
      options.platforms = option;
    } else {
      throw UsageError("unexpected argument: " + std::string(option));
    }
  }
  if (command != "build" && !options.platforms) {
    throw UsageError(std::string(command) + " requires one platform");
  }
  return options;
}

std::filesystem::path ResolveCMakeBuildSdk(const std::filesystem::path& sdk_home, const BuildOptions& options,
                                          const std::filesystem::path& working_directory) {
  if (options.no_shell) {
    throw UsageError("--no-shell is not available for a CMake project: the CMake integration builds and packages "
                     "Android and iOS through the standard shell");
  }
  if (options.profiling && (!options.sdk_override || !IsSourceHome(working_directory / *options.sdk_override))) {
    throw UsageError("--profile requires --sdk <path> pointing to a HuxerUI source checkout");
  }
  return ResolveBuildHome(sdk_home, options.sdk_override, working_directory);
}

/// How a command treats the standard platform shells under `platform/<platform-id>`.
enum class ShellUse {
  /// The command drives the shell, so a selected platform must be enabled and its shell complete.
  Required,
  /// The integration builds without the standard shell, selected explicitly with `--no-shell`; its build rules read
  /// whatever shell inputs exist and report what they lack.
  Optional,
};

/// Whether a platform's application core is consumed by a standard shell that builds and packages the application:
/// Gradle on Android and Xcode on iOS. Every integration builds these platforms through the shell by default.
bool HasCoreShell(const PlatformDriver& platform) {
  return platform.Id() == "android" || platform.Id() == "ios";
}

std::vector<const PlatformDriver*> ResolveBuildPlatforms(const Project& project, BuildOptions& options,
                                                         std::string_view command,
                                                         ShellUse shells = ShellUse::Required) {
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
  if (options.package_format && (platforms.size() != 1 || platforms.front()->Id() != "linux")) {
    throw UsageError("--format requires Linux as the sole packaging platform");
  }
  if (!options.abis.empty()) {
    if (platforms.size() != 1) {
      throw UsageError("--abi requires exactly one build platform");
    }
    const PlatformDriver& platform = *platforms.front();
    for (std::string& abi : options.abis) {
      const std::string_view normalized = detail::NormalizeTargetAbi(platform, abi);
      if (normalized.empty()) {
        throw UsageError("unsupported ABI for " + std::string(platform.Id()) + ": " + abi);
      }
      abi = normalized;
    }
    std::ranges::sort(options.abis);
    options.abis.erase(std::unique(options.abis.begin(), options.abis.end()), options.abis.end());
    if (command == "run" && platform.Id() == "linux" && options.abis.size() > 1) {
      throw UsageError("run linux accepts one ABI");
    }
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
    if (shells == ShellUse::Required &&
        std::find(project.platforms.begin(), project.platforms.end(), platform->Id()) == project.platforms.end()) {
      throw std::runtime_error("platform is not enabled by this project: " + std::string(platform->Id()));
    }
    if (!platform->SupportsCurrentHost()) {
      throw std::runtime_error(
          "platform " + std::string(platform->Id()) + " cannot be built from host " + std::string(CurrentHostId())
      );
    }
    if (shells == ShellUse::Optional) {
      continue;
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

/// The context of one platform build. The platform's own environment, such as the JDK and the Android SDK, is
/// resolved for either integration; CMake and the framework location only for the CMake path, since an mcpp build
/// takes its engine from PATH and the framework from its manifest.
PlatformCommandContext MakeCommandContext(const Project& project, const PlatformDriver& platform,
                                          const std::filesystem::path& huxerui_home, const BuildOptions& options,
                                          std::span<const std::string> abis = {},
                                          BuildSystem build_system = BuildSystem::CMake) {
  std::string build_variant(platform.Id());
  if (platform.Id() == "ios") {
    build_variant +=
        options.selected_device && options.selected_device->kind == DeviceKind::Physical ? "-device" : "-simulator";
  }
  if (!abis.empty()) {
    build_variant += "-" + detail::JoinTargetAbis(abis, '+');
  }
  std::string build_profile = options.profile;
  if (options.optimization) {
    build_profile += "-" + *options.optimization;
  }
  if (options.lto) {
    build_profile += *options.lto ? "-lto" : "-no-lto";
  }
  if (options.profiling) {
    build_profile += "-profiling";
  }
  if (options.watch) build_profile += "-watch";
  const std::filesystem::path build_directory =
      project.root / ".huxerui/build" / BuildHomeKey(huxerui_home) / build_variant / build_profile;
  EnvironmentRequest request;
  request.operation = options.package ? EnvironmentOperation::Package : EnvironmentOperation::Build;
  request.project_root = project.root;
  request.sdk_home = huxerui_home;
  request.java_home = options.java_home.value_or(std::filesystem::path{});
  request.generator = options.cmake_generator;
  request.package_format = options.package_format.value_or("");
  // The platform's toolchain builds for CMake and the standard shells; mcpp provisions its own for the programs it
  // builds, such as MSVC on Windows and Emscripten on the Web.
  auto environment = build_system == BuildSystem::CMake || HasCoreShell(platform) ? platform.ResolveEnvironment(request)
                                                                                  : ProcessEnvironment{};
  if (build_system == BuildSystem::CMake) {
    environment.emplace_back("HUXERUI_HOME", huxerui_home.string());
    const auto cmake =
        detail::ResolveCMake(environment, detail::EnvironmentValue(environment, "HUXERUI_CMAKE_EXECUTABLE"));
    if (cmake.path.empty()) {
      throw std::runtime_error("CMake is unavailable; run 'huxerui doctor " + std::string(platform.Id()) + "'");
    }
    if (cmake.status != EnvironmentDiagnosticStatus::Ready) {
      throw std::runtime_error("CMake " + cmake.required_version + " or later is required: " + cmake.detail);
    }
    detail::PrependPath(environment, cmake.path.parent_path());
    environment.emplace_back("HUXERUI_CMAKE_EXECUTABLE", cmake.path.string());
    environment.emplace_back("HUXERUI_CMAKE_VERSION", cmake.version);
  }
  std::string cmake_generator = options.cmake_generator;
  if (platform.Id() == "windows") {
    if (!cmake_generator.empty()) {
      throw UsageError("Windows builds select an MSVC-compatible CMake generator automatically");
    }
    if (!std::filesystem::is_regular_file(build_directory / "CMakeCache.txt")) {
      cmake_generator = FindExecutable("ninja", environment) ? "Ninja" : "NMake Makefiles";
    }
  } else if (platform.Id() != "android" && platform.Id() != "ios" && cmake_generator.empty() &&
             !std::filesystem::is_regular_file(build_directory / "CMakeCache.txt") &&
             !ReadEnvironmentVariable("CMAKE_GENERATOR") && FindExecutable("ninja", environment)) {
    cmake_generator = "Ninja";
  }
  return {
      project.root,
      huxerui_home,
      build_directory,
      std::move(cmake_generator),
      options.profile,
      options.selected_device,
      options.package,
      options.optimization,
      options.lto,
      {abis.begin(), abis.end()},
      options.package_format,
      options.profiling,
      options.watch,
      build_system,
      std::move(environment),
  };
}

std::vector<PlatformCommandContext> MakeBuildContexts(const Project& project, const PlatformDriver& platform,
                                                      const std::filesystem::path& huxerui_home,
                                                      const BuildOptions& options) {
  std::vector<PlatformCommandContext> contexts;
  if (platform.Id() == "linux" && options.abis.size() > 1) {
    for (const std::string& abi : options.abis) {
      contexts.push_back(MakeCommandContext(project, platform, huxerui_home, options, std::span(&abi, 1)));
    }
  } else {
    contexts.push_back(MakeCommandContext(project, platform, huxerui_home, options, options.abis));
  }
  for (const PlatformCommandContext& context : contexts) {
    platform.ValidateBuildAbis(context);
  }
  return contexts;
}

void ExecuteCommands(std::span<const ProcessCommand> commands, std::ostream& output,
                     const ProcessEnvironment& environment) {
  for (const ProcessCommand& planned : commands) {
    const auto command = detail::WithEnvironment(planned, environment);
    console.Command(output, DescribeProcess(command), true);
    const int result = RunProcess(command);
    console.CommandEnded(output);
    if (result != 0) {
      throw std::runtime_error(
          "command failed with exit code " + std::to_string(result) + ": " + DescribeProcess(command)
      );
    }
  }
}

void BuildPlatform(const PlatformDriver& platform, const PlatformCommandContext& context,
                   const BuildOptions& options, std::ostream& output) {
  std::string step = "Building " + std::string(platform.Id()) + " (" + context.profile;
  if (!context.abis.empty()) {
    step += ", " + detail::JoinTargetAbis(context.abis, ',');
  }
  // The integration the command dispatched to, as the mcpp-only paths already state it.
  console.Step(output, step + ") with " + std::string(BuildSystemName(context.build_system)));
  auto library_graph = platform.LibraryGraphCommands(context);
  if (options.watch && library_graph.empty()) library_graph = detail::LibraryGraphConfigureCommands(context);
  ExecuteCommands(library_graph, output, context.environment);
  platform.UpdateProjectIntegration(context);
  ExecuteCommands(platform.BuildCommands(context), output, context.environment);
  platform.VerifyBuildAbis(context);
}

std::optional<PlatformDevice> SelectDevice(const PlatformDriver& platform, std::string_view requested,
                                           const std::filesystem::path& project_root) {
  if (!platform.SupportsDeviceDiscovery()) {
    if (!requested.empty()) {
      throw UsageError("--device is not supported for platform " + std::string(platform.Id()));
    }
    return std::nullopt;
  }

  EnvironmentRequest request;
  request.operation = EnvironmentOperation::Devices;
  request.project_root = project_root;
  const std::vector<PlatformDevice> devices = platform.DiscoverDevices(request);
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

/// The integration a command dispatches to: the one `--build-system` selects, the project's only one, or CMake for a
/// project that declares both.
BuildSystem SelectBuildSystem(const Project& project, const BuildOptions& options) {
  if (options.build_system) {
    if (!project.Declares(*options.build_system)) {
      throw UsageError("this project has no " + std::string(BuildSystemName(*options.build_system)) +
                       " declaration: " + project.root.string());
    }
    return *options.build_system;
  }
  return project.DefaultBuildSystem();
}

struct BuildProject {
  Project project;
  BuildSystem build_system;
  std::filesystem::path huxerui_home;
};

BuildProject ResolveBuildProject(const std::filesystem::path& working_directory, const std::filesystem::path& sdk_home,
                                 const BuildOptions& options, std::ostream& output) {
  Project project;
  try {
    project = DiscoverProject(working_directory);
  } catch (const std::runtime_error&) {
    // Without a project to select an integration, CMake's SDK and option diagnostics come first.
    static_cast<void>(ResolveCMakeBuildSdk(sdk_home, options, working_directory));
    throw;
  }
  project = ResolveApplicationProject(project);
  const BuildSystem build_system = SelectBuildSystem(project, options);
  if (build_system == BuildSystem::Mcpp) {
    return {std::move(project), build_system, {}};
  }
  if (!options.build_system && project.Declares(BuildSystem::Mcpp)) {
    console.Note(output, "Using cmake, the default (or --build-system mcpp)");
  }
  auto huxerui_home = ResolveCMakeBuildSdk(sdk_home, options, working_directory);
  if (!project.unknown_platforms.empty()) {
    throw std::runtime_error("unknown platform directory: " + project.unknown_platforms.front());
  }
  return {std::move(project), build_system, std::move(huxerui_home)};
}

/// Applies the contract's rule for options the mcpp integration cannot honor: an option that would change the
/// product is invalid usage, and an option that only steers the CMake driver is reported and not applied.
/// `through_shell` is true when every selected platform builds through its standard shell, which takes the ABIs,
/// the device, and the JDK as it does for CMake.
void ApplyMcppOptionRules(BuildOptions& options, bool through_shell, std::ostream& error) {
  const auto refuse = [](std::string_view option, std::string_view reason) {
    throw UsageError(std::string(option) + " " + std::string(reason) + " for an mcpp project");
  };
  if (options.sdk_override) {
    throw UsageError("--sdk does not apply to an mcpp project: it takes HuxerUI from the huxerui dependency in "
                     "mcpp.toml, so change that entry instead");
  }
  // adb-run, the runner of an Android package without the shell, takes its device from ANDROID_SERIAL.
  if (!options.device.empty() && !through_shell && !(options.no_shell && options.platforms == "android")) {
    refuse("--device", "is not available without the standard shell");
  }
  if (options.watch && options.no_shell) {
    refuse("--watch", "is not available without the standard shell");
  }
  if (options.java_home && !through_shell) {
    error << "huxerui: warning: --java-home selects the JDK of the Gradle shell, which this command does not run; "
             "it is not applied\n";
    options.java_home.reset();
  }
}

/// The mcpp profile and features a command selects: `--optimization` and `--lto` select a release profile the
/// manifest declares, and `--profile` opens the framework's profiling feature.
mcpp::CommandOptions McppOptions(const Project& project, std::string_view platform, const BuildOptions& options) {
  try {
    mcpp::CommandOptions result = mcpp::ResolveOptions(project.root, platform, options.profile, options.optimization,
                                                       options.lto, options.profiling);
    result.abis = options.abis;
    return result;
  } catch (const std::invalid_argument& error) {
    throw UsageError(error.what());
  }
}

/// Whether the mcpp integration builds a platform through its standard shell for this command.
bool McppUsesShell(const PlatformDriver& platform, const BuildOptions& options) {
  return !options.no_shell && HasCoreShell(platform);
}

struct McppBuild {
  std::vector<const PlatformDriver*> platforms;
  mcpp::CommandOptions options;
};

McppBuild PrepareMcppBuild(const Project& project, BuildOptions& options, std::string_view command,
                           const std::filesystem::path& working_directory, std::ostream& error) {
  // An option that only steers the CMake driver is reported before anything can stop the command.
  if (!options.cmake_generator.empty()) {
    error << "huxerui: warning: --generator selects a CMake generator and does not apply to an mcpp project; "
             "it is not applied\n";
    options.cmake_generator.clear();
  }
  // The profile the release overrides select is a usage question, answered before the platforms are checked.
  auto mcpp_options = McppOptions(project, {}, options);
  if (!project.unknown_platforms.empty()) {
    throw std::runtime_error("unknown platform directory: " + project.unknown_platforms.front());
  }
  std::vector<const PlatformDriver*> platforms =
      ResolveBuildPlatforms(project, options, command, options.no_shell ? ShellUse::Optional : ShellUse::Required);
  if (options.no_shell) {
    for (const PlatformDriver* platform : platforms) {
      if (!HasCoreShell(*platform)) {
        throw UsageError("--no-shell applies to platforms with a standard shell, android and ios; " +
                         std::string(platform->Id()) + " has none");
      }
    }
  }
  const bool through_shell = std::ranges::all_of(platforms, [&options](const PlatformDriver* platform) {
    return McppUsesShell(*platform, options);
  });
  // A standard shell builds every ABI it is given; mcpp's own path builds the ABIs it has a target for.
  if (!through_shell) {
    for (const std::string& abi : options.abis) {
      try {
        static_cast<void>(mcpp::AbiTriple(platforms.front()->Id(), abi));
      } catch (const std::invalid_argument& error) {
        throw UsageError("--abi " + abi + " is not available for an mcpp project: " + error.what());
      }
    }
  }
  ApplyMcppOptionRules(options, through_shell, error);
  ResolveJavaHome(options, working_directory);
  mcpp_options.abis = options.abis;
  return {std::move(platforms), std::move(mcpp_options)};
}

/// The command context of a platform the mcpp integration builds through its standard shell. The shell reads the
/// framework's location from the record the build program writes, so no SDK is resolved.
PlatformCommandContext McppShellContext(const Project& project, const PlatformDriver& platform,
                                        const BuildOptions& options, mcpp::CommandOptions mcpp_options) {
  PlatformCommandContext context =
      MakeCommandContext(project, platform, project.root, options, options.abis, BuildSystem::Mcpp);
  platform.ValidateBuildAbis(context);
  mcpp_options.platform = platform.Id();
  context.mcpp_options = std::move(mcpp_options);
  return context;
}

/// Builds a program with mcpp and writes the application-integration record the CMake build writes, which the
/// watcher stages the program from: the program, or on macOS the bundle the rules assemble beside it. On the Web the
/// watcher serves the page beside the module with emrun, so the build also asks mcpp for the emrun of the
/// Emscripten toolchain it builds with, the program of its `web` runner, and puts its directory on `context`'s PATH.
void BuildMcppProgram(PlatformCommandContext& context, const mcpp::CommandOptions& options, std::ostream& output) {
  const bool web = options.platform == "web";
  ProcessCommand command = web ? mcpp::LaunchCommand(options, "web") : mcpp::BuildCommand(options);
  if (!web) {
    command.arguments.insert(command.arguments.end(), {"--message-format", "json"});
  }
  console.Command(output, DescribeProcess(command), false);
  const ProcessResult result = RunProcessCapture(command);
  if (result.exit_code != 0) {
    throw std::runtime_error("command failed with exit code " + std::to_string(result.exit_code) + ": " +
                             DescribeProcess(command));
  }
  if (web) {
    const std::filesystem::path toolchain = mcpp::ParseLaunchProgram(result.output).parent_path();
    if (detail::EnvironmentValue(context.environment, "PATH").find(toolchain.string()) != 0) {
      detail::PrependPath(context.environment, toolchain);
    }
  }
  std::string name;
  std::filesystem::path artifact = mcpp::ParseBuiltProgram(result.output, name).path;
  const std::filesystem::path bundle = artifact.parent_path() / (name + ".app");
  const bool bundled = std::filesystem::is_directory(bundle);
  if (bundled) {
    artifact = bundle / "Contents/MacOS" / name;
  }
  const std::filesystem::path plan = context.build_directory / "huxerui-integration/app.json";
  std::filesystem::create_directories(plan.parent_path());
  std::ofstream(plan, std::ios::binary | std::ios::trunc)
      << "{\n  \"schema\": 1,\n  \"target\": " << JsonQuote(name) << ",\n  \"artifact\": "
      << JsonQuote(artifact.generic_string()) << ",\n  \"bundle\": "
      << JsonQuote(bundled ? bundle.generic_string() : std::string{}) << "\n}\n";
}

/// An mcpp command stops before it prints or starts anything when the engine is missing, as a CMake build does
/// without CMake; `doctor` names the installation.
void RequireMcpp(std::span<const PlatformDriver* const> platforms) {
  if (detail::ResolveMcpp().status == EnvironmentDiagnosticStatus::Ready) {
    return;
  }
  throw std::runtime_error("mcpp is unavailable; run 'huxerui doctor " + JoinPlatformIds(platforms) + "'");
}

int RunMcppCommand(const Project& project, BuildOptions& options, std::string_view command,
                   const std::filesystem::path& working_directory, std::ostream& output, std::ostream& error) {
  auto [platforms, mcpp_options] = PrepareMcppBuild(
      project, options, command == "build" ? std::string_view{} : command, working_directory, error);
  RequireMcpp(platforms);
  if (options.watch) {
    const PlatformDriver& platform = *platforms.front();
    if (McppUsesShell(platform, options)) {
      options.selected_device = SelectDevice(platform, options.device, project.root);
      auto context = McppShellContext(project, platform, options, std::move(mcpp_options));
      bool first_build = true;
      const auto build = [&] {
        // Later builds pick up manifest edits before either mcpp or the shell uses the profile and features.
        if (!std::exchange(first_build, false)) {
          context.mcpp_options = McppOptions(project, platform.Id(), options);
        }
        BuildPlatform(platform, context, options, output);
      };
      return WatchApplication(platform, context, build, output, error);
    }
    PlatformCommandContext context =
        MakeCommandContext(project, platform, project.root, options, {}, BuildSystem::Mcpp);
    mcpp_options.platform = platform.Id();
    return WatchApplication(platform, context, [&] { BuildMcppProgram(context, mcpp_options, output); }, output,
                            error);
  }
  for (const PlatformDriver* platform : platforms) {
    if (McppUsesShell(*platform, options)) {
      if (command == "run") {
        options.selected_device = SelectDevice(*platform, options.device, project.root);
      }
      const PlatformCommandContext context = McppShellContext(project, *platform, options, mcpp_options);
      BuildPlatform(*platform, context, options, output);
      if (command == "run") {
        console.Step(output, "Running " + std::string(platform->Id()));
        ExecuteCommands(platform->RunCommands(context), output, context.environment);
      }
      continue;
    }
    mcpp_options.platform = platform->Id();
    if (command == "run") {
      if (!options.device.empty()) {
        console.Note(output, "Device: " + options.device);
        SetProcessEnvironmentVariable("ANDROID_SERIAL", options.device);
      }
      console.Step(output, "Running " + std::string(platform->Id()) + " (" + options.profile + ") with mcpp");
      const ProcessCommand run = mcpp::RunCommand(mcpp_options);
      ExecuteCommands(std::span(&run, 1), output, {});
      continue;
    }
    console.Step(output, "Building " + std::string(platform->Id()) + " (" + options.profile + ") with mcpp");
    const ProcessCommand build = mcpp::BuildCommand(mcpp_options);
    ExecuteCommands(std::span(&build, 1), output, {});
  }
  return 0;
}

void PublishPackageArtifacts(std::span<const PackageArtifact> artifacts, const std::filesystem::path& destination,
                             std::ostream& output);
void PackagePlatform(const PlatformDriver& platform, std::span<const PlatformCommandContext> contexts,
                     const BuildOptions& options, std::ostream& output);

/// Packages each platform: through its standard shell where it has one, and otherwise, or with `--no-shell`, with
/// `mcpp pack` in the format the platform's distributable uses. Either way the artifacts are published below
/// `dist/<platform>` through the same validated publication as the CMake path.
int RunMcppPackage(const Project& project, BuildOptions& options, const std::filesystem::path& working_directory,
                   std::ostream& output, std::ostream& error) {
  auto [platforms, mcpp_options] = PrepareMcppBuild(project, options, {}, working_directory, error);
  RequireMcpp(platforms);
  for (const PlatformDriver* platform : platforms) {
    if (McppUsesShell(*platform, options)) {
      const PlatformCommandContext context = McppShellContext(project, *platform, options, mcpp_options);
      PackagePlatform(*platform, std::span(&context, 1), options, output);
      continue;
    }
    const std::string format = mcpp::PackageFormat(platform->Id(), options.package_format.value_or(""));
    mcpp_options.platform = platform->Id();
    const ProcessCommand command = mcpp::PackCommand(mcpp_options, format);
    console.Step(output, "Packaging " + std::string(platform->Id()) + " (" + options.profile + ", " + format +
                             ") with mcpp");
    console.Command(output, DescribeProcess(command), false);
    const ProcessResult result = RunProcessCapture(command);
    if (result.exit_code != 0) {
      throw std::runtime_error("command failed with exit code " + std::to_string(result.exit_code) + ": " +
                               DescribeProcess(command));
    }
    std::vector<PackageArtifact> artifacts;
    for (const mcpp::PackedArtifact& packed : mcpp::ParsePackArtifacts(result.output)) {
      if (format != "web") {
        artifacts.push_back({packed.path, packed.path.filename()});
        continue;
      }
      // mcpp reports the Web page as the distributable; the page loads the module, its WebAssembly, and the
      // preloaded data beside it, so the directory is what is published.
      for (const std::filesystem::directory_entry& entry :
           std::filesystem::directory_iterator(packed.path.parent_path())) {
        if (entry.is_regular_file()) {
          artifacts.push_back({entry.path(), entry.path().filename()});
        }
      }
    }
    std::ranges::sort(artifacts, {}, &PackageArtifact::destination);
    PublishPackageArtifacts(artifacts, project.root / "dist" / platform->Id(), output);
  }
  return 0;
}

int RunCMakeApplication(const Project& project, const PlatformDriver& platform,
                        const std::filesystem::path& huxerui_home, const BuildOptions& options,
                        std::ostream& output, std::ostream& error) {
  if (const auto& device = options.selected_device) {
    output << "Device: " << device->id;
    if (!device->name.empty()) {
      output << " (" << device->name << ')';
    }
    output << '\n';
  }
  const auto contexts = MakeBuildContexts(project, platform, huxerui_home, options);
  const auto& context = contexts.front();
  if (options.watch) {
    return WatchApplication(platform, context, [&] { BuildPlatform(platform, context, options, output); }, output, error);
  }
  BuildPlatform(platform, context, options, output);

  console.Step(output, "Running " + std::string(platform.Id()));
  const std::vector<ProcessCommand> commands = platform.RunCommands(context);
  ExecuteCommands(commands, output, context.environment);
  return 0;
}

void CopyPackageArtifacts(std::span<const PackageArtifact> artifacts, const std::filesystem::path& destination_root) {
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
  }
}

void PublishPackageArtifacts(std::span<const PackageArtifact> artifacts, const std::filesystem::path& destination,
                             std::ostream& output) {
  const std::filesystem::path parent = destination.parent_path();
  const std::filesystem::path staging = parent / ("." + destination.filename().string() + ".publishing");
  const std::filesystem::path previous = parent / ("." + destination.filename().string() + ".previous");
  std::error_code error;
  std::filesystem::remove_all(staging, error);
  if (error) {
    throw std::runtime_error("cannot prepare package publication: " + error.message());
  }
  if (!std::filesystem::exists(destination) && std::filesystem::exists(previous)) {
    std::filesystem::rename(previous, destination, error);
    if (error) {
      throw std::runtime_error("cannot restore previous package directory: " + error.message());
    }
  }
  std::filesystem::remove_all(previous, error);
  if (error) {
    throw std::runtime_error("cannot prepare previous package backup: " + error.message());
  }
  CopyPackageArtifacts(artifacts, staging);

  const bool had_previous = std::filesystem::exists(destination);
  if (had_previous) {
    std::filesystem::rename(destination, previous, error);
    if (error) {
      std::error_code cleanup_error;
      std::filesystem::remove_all(staging, cleanup_error);
      throw std::runtime_error("cannot preserve existing package directory: " + error.message());
    }
  }
  std::filesystem::rename(staging, destination, error);
  if (error) {
    std::string message = "cannot publish package directory: " + error.message();
    if (had_previous) {
      std::error_code restore_error;
      std::filesystem::rename(previous, destination, restore_error);
      if (restore_error) {
        message += "; previous package remains at " + previous.string() + ": " + restore_error.message();
      }
    }
    std::error_code cleanup_error;
    std::filesystem::remove_all(staging, cleanup_error);
    throw std::runtime_error(message);
  }
  if (had_previous) {
    std::filesystem::remove_all(previous, error);
    if (error) {
      output << "Warning: cannot remove previous package directory: " << error.message() << '\n';
    }
  }
  for (const PackageArtifact& artifact : artifacts) {
    console.Done(output, "Packaged " + (destination / artifact.destination).string());
  }
}

void PackagePlatform(const PlatformDriver& platform, std::span<const PlatformCommandContext> contexts,
                     const BuildOptions& options, std::ostream& output) {
  std::vector<PackageArtifact> artifacts;
  for (const PlatformCommandContext& context : contexts) {
    BuildPlatform(platform, context, options, output);
    ExecuteCommands(platform.PackageCommands(context), output, context.environment);
    auto produced = platform.PackageArtifacts(context);
    for (PackageArtifact& artifact : produced) {
      if (contexts.size() > 1) {
        artifact.destination = std::filesystem::path(context.abis.front()) / artifact.destination;
      }
      artifacts.push_back(std::move(artifact));
    }
  }
  PublishPackageArtifacts(artifacts, contexts.front().project_root / "dist" / platform.Id(), output);
}

int RunBuildCommand(std::string_view command, std::span<const std::string_view> arguments,
                    const std::filesystem::path& working_directory, const std::filesystem::path& sdk_home,
                    std::ostream& output, std::ostream& error) {
  BuildOptions options = ParseBuildOptions(arguments, command);
  options.package = command == "package";
  if (options.package && !options.profile_explicit) {
    options.profile = "release";
  }
  const auto [project, build_system, huxerui_home] = ResolveBuildProject(working_directory, sdk_home, options, output);
  if (build_system == BuildSystem::Mcpp) {
    return options.package ? RunMcppPackage(project, options, working_directory, output, error)
                           : RunMcppCommand(project, options, command, working_directory, output, error);
  }
  const bool run = command == "run";
  const auto platforms = ResolveBuildPlatforms(project, options, run ? command : std::string_view{});
  ResolveJavaHome(options, working_directory);
  if (run || !options.device.empty()) {
    options.selected_device = SelectDevice(*platforms.front(), options.device, project.root);
  }
  if (run) {
    return RunCMakeApplication(project, *platforms.front(), huxerui_home, options, output, error);
  }
  for (const PlatformDriver* platform : platforms) {
    const auto contexts = MakeBuildContexts(project, *platform, huxerui_home, options);
    if (options.package) {
      PackagePlatform(*platform, contexts, options, output);
    } else {
      for (const PlatformCommandContext& context : contexts) {
        BuildPlatform(*platform, context, options, output);
      }
    }
  }
  return 0;
}

int RunOpen(std::span<const std::string_view> arguments, const std::filesystem::path& working_directory,
            const std::filesystem::path& sdk_home, std::ostream& output) {
  BuildOptions options = ParseBuildOptions(arguments, "open");
  if (!options.platforms || *options.platforms != "ios") {
    throw UsageError("open usage: huxerui open ios [--build-system cmake|mcpp] [--sdk <path>]");
  }
  if (options.profile_explicit || !options.device.empty() || !options.cmake_generator.empty() || options.java_home ||
      options.optimization || options.lto || !options.abis.empty()) {
    throw UsageError("open accepts only --sdk <path> and --build-system");
  }
  const auto [project, build_system, huxerui_home] = ResolveBuildProject(working_directory, sdk_home, options, output);
  const bool use_mcpp = build_system == BuildSystem::Mcpp;
  if (use_mcpp && options.sdk_override) {
    throw UsageError("--sdk does not apply to an mcpp project: it takes HuxerUI from the huxerui dependency in "
                     "mcpp.toml, so change that entry instead");
  }
  const auto platforms = ResolveBuildPlatforms(project, options, "open");
  if (use_mcpp) {
    RequireMcpp(platforms);
  }
  const PlatformDriver& platform = *platforms.front();
  console.Step(output, "Opening ios Xcode project");
  const auto context = use_mcpp ? McppShellContext(project, platform, options, McppOptions(project, "ios", options))
                                : MakeCommandContext(project, platform, huxerui_home, options);
  ExecuteCommands(platform.LibraryGraphCommands(context), output, context.environment);
  platform.UpdateProjectIntegration(context);
  ExecuteCommands(platform.OpenCommands(context), output, context.environment);
  return 0;
}

int RunSdk(std::span<const std::string_view> arguments, const SdkLocation& sdk, std::ostream& output) {
  if (arguments.empty() || (arguments.size() == 1 && (arguments[0] == "--help" || arguments[0] == "-h"))) {
    output << "Manage installed HuxerUI SDK versions.\n\nUsage:\n" << sdk_commands;
    return 0;
  }

  const auto command = arguments.front();
  arguments = arguments.subspan(1);
  if (command == "list" && arguments.empty()) {
    return RunSdkInstaller(sdk, command, {}, false, false, output);
  }
  bool check = false;
  bool yes = false;
  std::string_view target_version;
  if (command == "use") {
    if (arguments.empty()) {
      throw UsageError("sdk use requires a major.minor.patch version");
    }
    target_version = arguments.front();
    auto number = target_version;
    if (number.starts_with('v')) {
      number.remove_prefix(1);
    }
    if (std::ranges::count(number, '.') != 2 || number.starts_with('.') || number.ends_with('.') ||
        number.find_first_not_of("0123456789.") != number.npos || number.find("..") != number.npos) {
      throw UsageError("sdk use requires a major.minor.patch version");
    }
    arguments = arguments.subspan(1);
  } else if (command != "update") {
    throw UsageError("expected sdk list, sdk use <version>, or sdk update");
  }
  for (const auto argument : arguments) {
    if (command == "update" && argument == "--check" && !check) {
      check = true;
    } else if (argument == "--yes" && !yes) {
      yes = true;
    } else {
      throw UsageError("unknown or repeated sdk option: " + std::string(argument));
    }
  }
  return RunSdkInstaller(sdk, command, target_version, check, yes, output);
}

} // namespace

int Run(std::span<const std::string_view> arguments, const std::filesystem::path& working_directory,
        const SdkLocation& sdk, std::istream& input, std::ostream& output, std::ostream& error,
        prompt::Prompter* prompter) {
  try {
    console = prompter != nullptr ? prompt::Console(prompter->OutputStyle()) : prompt::Console();
    if (arguments.empty() || arguments[0] == "--help" || arguments[0] == "-h") {
      std::ostringstream help;
      PrintHelp(help);
      WriteHelp(help.str(), output);
      return 0;
    }
    if (arguments[0] == "--version") {
      output << "huxerui " << version << '\n';
      return 0;
    }
    std::string_view command = arguments.front();
    for (const auto& [alias, canonical] : command_aliases) {
      if (command == alias) {
        command = canonical;
        break;
      }
    }
    if (arguments.size() == 2 && (arguments[1] == "--help" || arguments[1] == "-h") &&
        (command == "build" || command == "run" || command == "package" || command == "open")) {
      std::ostringstream help;
      PrintBuildHelp(help, command);
      WriteHelp(help.str(), output);
      return 0;
    }
    if (command == "create") {
      return RunCreate(arguments, working_directory, sdk.home, prompter, output);
    }
    if (command == "platform") {
      return RunPlatform(arguments, working_directory, output);
    }
    if (command == "doctor") {
      return RunEnvironment(arguments, working_directory, sdk, false, input, output);
    }
    if (command == "setup") {
      return RunEnvironment(arguments, working_directory, sdk, true, input, output);
    }
    if (command == "sdk") {
      return RunSdk(arguments.subspan(1), sdk, output);
    }
    if (command == "devices") {
      return RunDevices(arguments, working_directory, output);
    }
    if (command == "build" || command == "run" || command == "package") {
      return RunBuildCommand(command, arguments, working_directory, sdk.home, output, error);
    }
    if (command == "open") {
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
