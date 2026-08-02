#pragma once

#include <cstddef>
#include <optional>
#include <string>

#include <huxerui/modifier.h>
#include <huxerui/text_input.h>
#include <huxerui/theme.h>
#include <huxerui/validation.h>

namespace huxerui {
class PlatformAdapter;
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

  bool operator==(const TextFieldModifier&) const = default;
  static bool LayoutEquals(const TextFieldModifier& left, const TextFieldModifier& right);
};

[[nodiscard]] Size MeasureTextField(MountedNode& node, PlatformAdapter& platform, Constraints constraints);

} // namespace huxerui::detail
