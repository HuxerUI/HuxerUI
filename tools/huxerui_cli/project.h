#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "platform.h"

namespace huxerui::cli {

struct Project {
  std::filesystem::path root;
  std::vector<std::string> platforms;
  std::vector<std::string> unknown_platforms;
};

[[nodiscard]] bool IsValidProjectName(std::string_view name) noexcept;
[[nodiscard]] ProjectTemplateContext MakeProjectTemplateContext(std::string_view project_name);
[[nodiscard]] Project DiscoverProject(const std::filesystem::path& start);
void CreateProject(
    const std::filesystem::path& destination,
    const ProjectTemplateContext& context,
    std::span<const PlatformDriver* const> platforms
);
void AddProjectPlatforms(
    const Project& project, const ProjectTemplateContext& context, std::span<const PlatformDriver* const> platforms
);

} // namespace huxerui::cli
