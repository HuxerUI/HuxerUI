#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace huxerui::cli {

/// A file generated from an embedded CLI template.
struct GeneratedFile {
  /// Path relative to the destination tree.
  std::filesystem::path path;
  /// Complete file contents.
  std::string content;
  /// Whether Unix-like hosts should add owner, group, and other execute permissions.
  bool executable = false;
};

/// Values shared by generated project and platform templates.
struct ProjectTemplateContext {
  /// User-facing project name.
  std::string project_name;
  /// Sanitized CMake target and product identifier.
  std::string target_name;
  /// Reverse-domain application or library identifier.
  std::string project_id;

  /// Replaces the standard project tokens in a string.
  /// @param value Template text containing tokens such as `@PROJECT_NAME@` or `@PROJECT_ID@`.
  /// @return Rendered text.
  [[nodiscard]] std::string Render(std::string_view value) const;
};

/// Additional token replacement applied while rendering a template tree.
struct TemplateReplacement {
  /// Token including its delimiters, such as `@ANDROID_COMPILE_SDK@`.
  std::string_view token;
  /// Text substituted for each token occurrence.
  std::string_view value;
};

/// Loads an embedded template tree and renders every path and text file.
/// @param root Embedded template directory relative to the CLI template root.
/// @param context Standard project replacement values.
/// @param replacements Additional platform-specific replacements.
/// @return Generated files ordered by relative path.
/// @throws std::logic_error if the embedded template tree is missing or cannot be read.
[[nodiscard]] std::vector<GeneratedFile>
RenderTemplateTree(std::string_view root, const ProjectTemplateContext& context,
                   std::span<const TemplateReplacement> replacements = {});

/// Loads an embedded template tree without token substitution.
/// @param root Embedded template directory relative to the CLI template root.
/// @return Copied files ordered by relative path.
/// @throws std::logic_error if the embedded template tree is missing or cannot be read.
[[nodiscard]] std::vector<GeneratedFile> CopyTemplateTree(std::string_view root);

} // namespace huxerui::cli
