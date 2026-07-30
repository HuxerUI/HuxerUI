#pragma once

#include <cstddef>
#include <optional>
#include <string>

#include <huxerui/modifier.h>
#include <huxerui/text_input.h>
#include <huxerui/theme.h>
#include <huxerui/validation.h>

namespace huxerui {
class PlatformHost;
}

namespace huxerui::detail {

struct ResolvedTextFieldStyle {
  using Value = TextFieldStyle;
};

struct TextFieldModifier {
  static const ModifierDescriptor& Descriptor();

  TextEditingValue value;
  std::string placeholder;
  TextInputConfiguration configuration;
  std::size_t min_lines = 1;
  std::optional<std::size_t> max_lines;
  std::optional<std::size_t> max_length;
  ValidationResult validation;
};

[[nodiscard]] Size MeasureTextField(MountedNode& node, PlatformHost& platform, Constraints constraints);
bool SelectTextFieldWord(MountedNode& node, Point position);
bool ExtendTextFieldSelection(MountedNode& node, Point position, bool start_handle);
bool QueryTextFieldSelectionGeometry(const MountedNode& node, Rect& start, Rect& end);
Color TextFieldSelectionHandleColor(const MountedNode& node);

} // namespace huxerui::detail
