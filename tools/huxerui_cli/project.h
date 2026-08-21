#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "platform.h"

namespace huxerui::cli {

enum class ProjectKind {
  App,
  Library,
};

struct Project {
  std::filesystem::path root;
  std::vector<std::string> platforms;
  std::vector<std::string> unknown_platforms;
};

[[nodiscard]] bool IsValidProjectName(std::string_view name) noexcept;
[[nodiscard]] bool IsValidLibraryProjectName(std::string_view name) noexcept;
[[nodiscard]] bool IsValidProjectId(std::string_view id) noexcept;
[[nodiscard]] ProjectTemplateContext
MakeProjectTemplateContext(std::string_view project_name, std::string_view project_id = {});
[[nodiscard]] ProjectTemplateContext
MakeLibraryProjectTemplateContext(std::string_view project_name, std::string_view project_id = {});
[[nodiscard]] std::string MakeLibraryProductName(std::string_view library_name);
[[nodiscard]] Project DiscoverProject(const std::filesystem::path& start);
[[nodiscard]] std::pair<ProjectKind, ProjectTemplateContext> LoadProjectTemplateContext(const Project& project);
[[nodiscard]] Project ResolveApplicationProject(const Project& project);
void CreateProject(
    const std::filesystem::path& destination,
    ProjectKind kind,
    const ProjectTemplateContext& context,
    std::span<const PlatformDriver* const> platforms
);
void AddProjectPlatforms(
    const Project& project,
    ProjectKind kind,
    const ProjectTemplateContext& context,
    std::span<const PlatformDriver* const> platforms
);

} // namespace huxerui::cli
