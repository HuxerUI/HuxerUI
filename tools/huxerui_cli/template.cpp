#include "template.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include <cmrc/cmrc.hpp>

CMRC_DECLARE(huxerui_cli_templates);

namespace huxerui::cli {
namespace {

void ReplaceAll(std::string& value, std::string_view token, std::string_view replacement) {
  std::size_t position = 0;
  while ((position = value.find(token, position)) != std::string::npos) {
    value.replace(position, token.size(), replacement);
    position += replacement.size();
  }
}

std::string Render(
    std::string_view value, const ProjectTemplateContext& context, std::span<const TemplateReplacement> replacements
) {
  std::string rendered = context.Render(value);
  std::string project_id_path = context.project_id;
  std::replace(project_id_path.begin(), project_id_path.end(), '.', '/');
  ReplaceAll(rendered, "@PROJECT_ID_PATH@", project_id_path);
  for (const TemplateReplacement& replacement : replacements) {
    if (replacement.token.empty()) {
      throw std::logic_error("HuxerUI CLI template replacement token is empty");
    }
  }
  static constexpr std::string_view replacement_prefixes[]{
      "@PROJECT_",
      "@TARGET_NAME@",
      "@MODULE_",
      "@ANDROID_",
      "@PACKAGE_DEPENDENCIES@",
      "@PRODUCT_DEPENDENCIES@",
  };
  for (const std::string_view prefix : replacement_prefixes) {
    std::size_t position = 0;
    while ((position = rendered.find(prefix, position)) != std::string::npos) {
      const std::size_t end = rendered.find('@', position + 1);
      if (end == std::string::npos) {
        throw std::logic_error("HuxerUI CLI template contains a malformed replacement: " + std::string(prefix));
      }
      const std::string_view token(rendered.data() + position, end - position + 1);
      const bool has_replacement =
          std::any_of(replacements.begin(), replacements.end(), [token](const TemplateReplacement& replacement) {
            return replacement.token == token;
          });
      if (!has_replacement) {
        throw std::logic_error("HuxerUI CLI template contains an unresolved replacement: " + std::string(token));
      }
      position = end + 1;
    }
  }
  for (const TemplateReplacement& replacement : replacements) {
    ReplaceAll(rendered, replacement.token, replacement.value);
  }
  return rendered;
}

void CollectTemplateFiles(
    const cmrc::embedded_filesystem& filesystem,
    std::string_view root,
    std::string_view relative_directory,
    const ProjectTemplateContext* context,
    std::span<const TemplateReplacement> replacements,
    std::vector<GeneratedFile>& files
) {
  std::string directory(root);
  if (!relative_directory.empty()) {
    directory += "/";
    directory += relative_directory;
  }
  for (const cmrc::directory_entry entry : filesystem.iterate_directory(directory)) {
    std::string relative_path(relative_directory);
    if (!relative_path.empty()) {
      relative_path += "/";
    }
    relative_path += entry.filename();
    if (entry.is_directory()) {
      CollectTemplateFiles(filesystem, root, relative_path, context, replacements, files);
      continue;
    }
    const cmrc::file resource = filesystem.open(std::string(root) + "/" + relative_path);
    const std::string_view content(resource.begin(), static_cast<std::size_t>(resource.end() - resource.begin()));
    if (context != nullptr) {
      files.push_back({Render(relative_path, *context, replacements), Render(content, *context, replacements)});
    } else {
      files.push_back({relative_path, std::string(content)});
    }
  }
}

std::vector<GeneratedFile> LoadTemplateTree(
    std::string_view root, const ProjectTemplateContext* context, std::span<const TemplateReplacement> replacements
) {
  const cmrc::embedded_filesystem filesystem = cmrc::huxerui_cli_templates::get_filesystem();
  if (!filesystem.is_directory(std::string(root))) {
    throw std::logic_error("HuxerUI CLI template directory is missing: " + std::string(root));
  }

  std::vector<GeneratedFile> files;
  try {
    CollectTemplateFiles(filesystem, root, {}, context, replacements, files);
  } catch (const std::system_error& error) {
    throw std::logic_error("HuxerUI CLI cannot read template directory " + std::string(root) + ": " + error.what());
  }
  std::sort(files.begin(), files.end(), [](const GeneratedFile& left, const GeneratedFile& right) {
    return left.path.generic_string() < right.path.generic_string();
  });
  return files;
}

} // namespace

std::string ProjectTemplateContext::Render(std::string_view value) const {
  std::string rendered(value);
  ReplaceAll(rendered, "@PROJECT_NAME@", project_name);
  ReplaceAll(rendered, "@TARGET_NAME@", target_name);
  ReplaceAll(rendered, "@PROJECT_ID@", project_id);
  return rendered;
}

std::vector<GeneratedFile> RenderTemplateTree(
    std::string_view root, const ProjectTemplateContext& context, std::span<const TemplateReplacement> replacements
) {
  return LoadTemplateTree(root, &context, replacements);
}

std::vector<GeneratedFile> CopyTemplateTree(std::string_view root) {
  return LoadTemplateTree(root, nullptr, {});
}

} // namespace huxerui::cli
