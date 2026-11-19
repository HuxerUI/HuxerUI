#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "platform.h"

namespace huxerui::cli {

/// Generated project shape.
enum class ProjectKind {
  /// Standalone application project.
  App,
  /// Reusable library with a preview application.
  Library,
};

/// What `huxerui create` composes: one source style and one native declaration per selected build system.
struct CreateSelection {
  /// How the sources reach HuxerUI.
  SourceStyle style = SourceStyle::Headers;
  /// Selected build systems, without duplicates, in `BuildSystem` order.
  std::vector<BuildSystem> build_systems;
  /// Whether a headers library also exports its headers as a module, generated as `modules/<package name>.cppm`.
  bool module_interface = false;

  /// Whether the selection carries a build system's declaration.
  [[nodiscard]] bool Has(BuildSystem build_system) const noexcept;
};

/// The command-line spelling of a build system, such as `cmake`.
[[nodiscard]] std::string_view BuildSystemName(BuildSystem build_system) noexcept;

/// Whether a build system provisions its own toolchain and packages, as mcpp does through its registry, instead of
/// taking compilers, libraries, and packaging tools from the host as CMake does.
[[nodiscard]] bool ProvisionsToolchain(BuildSystem build_system) noexcept;

/// Parses a build system's command-line spelling.
/// @return The build system, or nothing for an unknown name.
[[nodiscard]] std::optional<BuildSystem> ParseBuildSystem(std::string_view name) noexcept;

/// The command-line spelling of a source style: `headers` or `modules`.
[[nodiscard]] std::string_view SourceStyleName(SourceStyle style) noexcept;

/// The style a created project has: its source style, or `headers + modules` for a library exporting both.
[[nodiscard]] std::string_view CreateStyleName(const CreateSelection& selection) noexcept;

/// The module a library's headers are exported as: its package name, which also names its interface file under
/// `modules/`.
[[nodiscard]] std::string ModuleInterfaceName(const LibraryTemplateContext& library);

/// Applies `huxerui create`'s defaults to the explicit build-system choice.
///
/// The source style follows the build systems: mcpp alone is C++ modules, the only integration offering the
/// `modules` capability, and any list naming CMake is headers. Without a choice an application uses CMake and a
/// library declares both CMake and mcpp.
/// @param kind Application or library.
/// @param build_systems Explicit build systems; empty when none were given.
/// @return The complete selection.
[[nodiscard]] CreateSelection ResolveCreateSelection(ProjectKind kind, std::span<const BuildSystem> build_systems);

/// Validated template identity for either an application or a reusable library.
using ProjectTemplate = std::variant<ProjectTemplateContext, LibraryTemplateContext>;

/// Destination layout used when copying the application-development Skill.
enum class AgentSkillDirectory {
  /// Shared `.agents/skills` layout.
  Shared,
  /// Claude `.claude/skills` layout.
  Claude,
  /// ZCode `.zcode/skills` layout.
  ZCode,
};

/// Discovered HuxerUI project and its platform directories.
struct Project {
  /// Project root carrying at least one native build declaration.
  std::filesystem::path root;
  /// Recognized platform directory names.
  std::vector<std::string> platforms;
  /// Unrecognized entries found in the platform directory.
  std::vector<std::string> unknown_platforms;
  /// Build systems whose native declaration the root carries, in `BuildSystem` order.
  std::vector<BuildSystem> build_systems;

  /// Whether the root carries a build system's declaration.
  [[nodiscard]] bool Declares(BuildSystem build_system) const noexcept;
  /// The build system a command dispatches to without `--build-system`: the only one declared, or CMake for a root
  /// declaring both.
  [[nodiscard]] BuildSystem DefaultBuildSystem() const noexcept;
};

/// Checks whether a name is valid for a generated application project.
/// @param name Candidate name.
/// @return `true` when the name starts with a letter and otherwise contains only letters, digits, `_`, or `-`.
bool IsValidProjectName(std::string_view name) noexcept;

/// Checks whether a name is valid for a generated library project.
/// @param name Candidate name.
/// @return `true` when the name consists of non-empty alphanumeric segments separated by single `_` or `-` characters.
bool IsValidLibraryProjectName(std::string_view name) noexcept;

/// Checks whether a string is a lowercase reverse-domain project identifier.
/// @param id Candidate identifier, such as `org.example.notes`.
/// @return `true` when the identifier has at least two letter-prefixed lowercase alphanumeric segments.
bool IsValidProjectId(std::string_view id) noexcept;

/// Creates normalized template values for an application project.
/// @param project_name Valid project name.
/// @param project_id Optional explicit identifier; an empty value derives `com.example.<name>`.
/// @return Template values containing the original name, normalized target, and resolved identifier.
/// @throws std::invalid_argument if the name or identifier is invalid.
ProjectTemplateContext MakeProjectTemplateContext(std::string_view project_name, std::string_view project_id = {});

/// Creates normalized template values for a library project.
/// @param project_name Valid segmented library name.
/// @param cpp_namespace Optional exact C++ namespace; an empty value derives it from `project_name`.
/// @param public_target Optional exact public CMake target; an empty value derives `Product::Product`.
/// @param project_id Optional explicit identifier; an empty value derives `com.example.<name>`.
/// @return Validated project and independently owned library identities.
/// @throws std::invalid_argument if any supplied identity is invalid.
LibraryTemplateContext MakeLibraryTemplateContext(std::string_view project_name,
    std::string_view cpp_namespace = {}, std::string_view public_target = {}, std::string_view project_id = {});

/// Converts a segmented library name to its PascalCase product name.
/// @param library_name Valid library project name, such as `data-grid`.
/// @return Product name, such as `DataGrid`.
/// @throws std::invalid_argument if `library_name` is invalid.
std::string MakeLibraryProductName(std::string_view library_name);

/// Searches a path and its ancestors for a HuxerUI project.
///
/// The nearest directory carrying a native declaration is the project: `CMakeLists.txt` beside `platform/` or
/// `include/`, or `mcpp.toml` beside `build.mcpp`.
/// @param start File or directory from which discovery starts.
/// @return Discovered project and platform names.
/// @throws std::runtime_error if no project is found or the start path cannot be resolved.
Project DiscoverProject(const std::filesystem::path& start);

/// Loads project identity through the project's CMake planning contract, or from the
/// `[package.metadata.huxerui]` table of a project that only declares mcpp.
/// @param project Project whose identity should be read.
/// @return Validated application or library template values.
/// @throws std::runtime_error if planning fails or emits invalid metadata.
ProjectTemplate LoadProjectTemplate(const Project& project);

/// What `import` reaches of a library, which `doctor` reports whatever build system builds it.
struct LibraryModules {
  enum class State {
    /// The root is an application's, which has no importer.
    NotLibrary,
    /// Importers name `module`, as the `[lib]` unit declares it.
    Module,
    /// A headers library that declares mcpp but is not a module yet; `reason` says what is missing.
    NotExported,
    /// A headers library without an mcpp declaration, which is what builds the module.
    NoMcpp,
  };
  State state = State::NotLibrary;
  /// The module importers name, as the `[lib]` unit declares it.
  std::string module = {};
  /// The modules the `[lib]` unit re-exports under its own name, such as `<module>.macros`.
  std::vector<std::string> reexports = {};
  /// The package identity the documentation recommends as the module name, when the module is named otherwise.
  std::string recommended = {};
  /// What a headers library still needs to be a module.
  std::string reason = {};
};

/// Reads a project root's C++ modules state from its declarations and files, without building.
/// @param project Discovered application or library project.
/// @return `NotLibrary` for an application.
LibraryModules DescribeLibraryModules(const Project& project);

/// Selects the runnable application for a project.
/// @param project Discovered application or library project.
/// @return The project itself for applications, or its preview application for libraries.
Project ResolveApplicationProject(const Project& project);

/// Generates a complete project and publishes it atomically at the destination.
/// @param destination New project directory, which must not already exist.
/// @param project_template Validated application or library template values.
/// @param application_platforms Platform shells for an application or library Preview.
/// @param library_platforms Platform packages owned by a reusable library.
/// @param skill_source Canonical application-development Skill directory.
/// @param agent_skill_directories Agent layouts that should receive the Skill.
/// @param selection Source style and the build systems whose declarations the project carries.
/// @param huxerui_home Resolved SDK home. An mcpp declaration created against a
///        source checkout depends on it by path rather than by published
///        version, which is what makes it buildable before HuxerUI is
///        published.
/// @param template_name mcpp package template under `templates/` for a C++ modules project. Empty selects
///        `app` for an application and `library` for a library, which is what
///        `mcpp new --template huxerui.huxerui:<name>` instantiates.
/// @throws std::invalid_argument if an application or library Preview has no platform.
/// @throws std::runtime_error if generation or publication fails.
void CreateProject(const std::filesystem::path& destination, const ProjectTemplate& project_template,
    std::span<const PlatformDriver* const> application_platforms,
    std::span<const PlatformDriver* const> library_platforms, const std::filesystem::path& skill_source,
    std::span<const AgentSkillDirectory> agent_skill_directories, const CreateSelection& selection = {
        SourceStyle::Headers, {BuildSystem::CMake}},
    const std::filesystem::path& huxerui_home = {}, std::string_view template_name = {});

/// Version of the HuxerUI package an mcpp project depends on.
std::string_view McppPackageVersion() noexcept;

/// Adds missing platform shells and library packages to an existing project.
/// @param project Existing project.
/// @param project_template Existing application or library template values.
/// @param platforms Platform drivers whose missing generated trees should be added.
/// @throws std::invalid_argument if no platform is supplied.
/// @throws std::runtime_error if every requested platform is already complete or publication fails.
void AddProjectPlatforms(const Project& project, const ProjectTemplate& project_template,
    std::span<const PlatformDriver* const> platforms);

} // namespace huxerui::cli
