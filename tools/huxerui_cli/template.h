#pragma once

#include <span>
#include <string_view>
#include <vector>

#include "platform.h"

namespace huxerui::cli {

struct TemplateReplacement {
  std::string_view token;
  std::string_view value;
};

[[nodiscard]] std::vector<GeneratedFile> RenderTemplateTree(
    std::string_view root, const ProjectTemplateContext& context, std::span<const TemplateReplacement> replacements = {}
);

} // namespace huxerui::cli
