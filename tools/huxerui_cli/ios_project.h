#pragma once

#include <filesystem>
#include <vector>

#include "platform.h"

namespace huxerui::cli {

[[nodiscard]] std::vector<GeneratedFile> CreateIosProject(const ProjectTemplateContext& context);
void ConfigureIosLocalSdk(const std::filesystem::path& project_root, const std::filesystem::path& sdk_root);

} // namespace huxerui::cli
