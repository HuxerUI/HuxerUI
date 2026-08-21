#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace huxerui::cli {

struct GeneratedFile {
  std::filesystem::path path;
  std::string content;
  bool executable = false;
};

struct ProjectTemplateContext {
  std::string project_name;
  std::string target_name;
  std::string project_id;

  [[nodiscard]] std::string Render(std::string_view value) const;
};

struct TemplateReplacement {
  std::string_view token;
  std::string_view value;
};

[[nodiscard]] std::vector<GeneratedFile> RenderTemplateTree(
    std::string_view root, const ProjectTemplateContext& context, std::span<const TemplateReplacement> replacements = {}
);
[[nodiscard]] std::vector<GeneratedFile> CopyTemplateTree(std::string_view root);

} // namespace huxerui::cli
