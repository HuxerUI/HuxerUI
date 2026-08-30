#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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
  /// Project root containing `CMakeLists.txt`.
  std::filesystem::path root;
  /// Recognized platform directory names.
  std::vector<std::string> platforms;
  /// Unrecognized entries found in the platform directory.
  std::vector<std::string> unknown_platforms;
};

/// Checks whether a name is valid for a generated application project.
/// @param name Candidate name.
/// @return `true` when the name starts with a letter and otherwise contains only letters, digits, `_`, or `-`.
[[nodiscard]] bool IsValidProjectName(std::string_view name) noexcept;

/// Checks whether a name is valid for a generated library project.
/// @param name Candidate name.
/// @return `true` when the name consists of non-empty alphanumeric segments separated by single `_` or `-` characters.
[[nodiscard]] bool IsValidLibraryProjectName(std::string_view name) noexcept;

/// Checks whether a string is a lowercase reverse-domain project identifier.
/// @param id Candidate identifier, such as `org.example.notes`.
/// @return `true` when the identifier has at least two letter-prefixed lowercase alphanumeric segments.
[[nodiscard]] bool IsValidProjectId(std::string_view id) noexcept;

/// Creates normalized template values for an application project.
/// @param project_name Valid project name.
/// @param project_id Optional explicit identifier; an empty value derives `com.example.<name>`.
/// @return Template values containing the original name, normalized target, and resolved identifier.
/// @throws std::invalid_argument if the name or identifier is invalid.
[[nodiscard]] ProjectTemplateContext
MakeProjectTemplateContext(std::string_view project_name, std::string_view project_id = {});

/// Creates normalized template values for a library project.
/// @param project_name Valid segmented library name.
/// @param project_id Optional explicit identifier; an empty value derives `com.example.<name>`.
/// @return Template values containing the original name, normalized library target, and resolved identifier.
/// @throws std::invalid_argument if the name or identifier is invalid.
[[nodiscard]] ProjectTemplateContext
MakeLibraryProjectTemplateContext(std::string_view project_name, std::string_view project_id = {});

/// Converts a segmented library name to its PascalCase product name.
/// @param library_name Valid library project name, such as `data-grid`.
/// @return Product name, such as `DataGrid`.
/// @throws std::invalid_argument if `library_name` is invalid.
[[nodiscard]] std::string MakeLibraryProductName(std::string_view library_name);

/// Searches a path and its ancestors for a HuxerUI project.
/// @param start File or directory from which discovery starts.
/// @return Discovered project and platform names.
/// @throws std::runtime_error if no project is found or the start path cannot be resolved.
[[nodiscard]] Project DiscoverProject(const std::filesystem::path& start);

/// Loads project identity through the project's CMake planning contract.
/// @param project Project whose root CMake file should be queried.
/// @return Project kind and validated template values.
/// @throws std::runtime_error if planning fails or emits invalid metadata.
[[nodiscard]] std::pair<ProjectKind, ProjectTemplateContext> LoadProjectTemplateContext(const Project& project);

/// Selects the runnable application for a project.
/// @param project Discovered application or library project.
/// @return The project itself for applications, or its preview application for libraries.
[[nodiscard]] Project ResolveApplicationProject(const Project& project);

/// Generates a complete project and publishes it atomically at the destination.
/// @param destination New project directory, which must not already exist.
/// @param kind Application or library project shape.
/// @param context Validated project template values.
/// @param platforms Platform shells to generate.
/// @param skill_source Canonical application-development Skill directory.
/// @param agent_skill_directories Agent layouts that should receive the Skill.
/// @throws std::invalid_argument if an application has no platform.
/// @throws std::runtime_error if generation or publication fails.
void CreateProject(const std::filesystem::path& destination, ProjectKind kind, const ProjectTemplateContext& context,
                   std::span<const PlatformDriver* const> platforms, const std::filesystem::path& skill_source,
                   std::span<const AgentSkillDirectory> agent_skill_directories);

/// Adds platform shells to an existing application or library preview.
/// @param project Existing project.
/// @param kind Project shape.
/// @param context Existing project template values.
/// @param platforms New platform drivers to add.
/// @throws std::invalid_argument if no platform is supplied.
/// @throws std::runtime_error if a requested platform already exists or publication fails.
void AddProjectPlatforms(const Project& project, ProjectKind kind, const ProjectTemplateContext& context,
                         std::span<const PlatformDriver* const> platforms);

} // namespace huxerui::cli
