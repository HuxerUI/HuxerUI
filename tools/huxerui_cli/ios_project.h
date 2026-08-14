#pragma once

#include <filesystem>
#include <vector>

#include "platform.h"

namespace huxerui::cli {

[[nodiscard]] std::vector<GeneratedFile> CreateIosProject(const ProjectTemplateContext& context);
[[nodiscard]] std::vector<GeneratedFile> CreateIosModulePackage(const ProjectTemplateContext& context);
void UpdateIosModuleIntegration(const std::filesystem::path& project_root);
void ConfigureIosLocalHome(const std::filesystem::path& project_root, const std::filesystem::path& huxerui_home);

} // namespace huxerui::cli
