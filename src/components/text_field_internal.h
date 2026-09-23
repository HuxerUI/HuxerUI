#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <variant>

#include <huxerui/modifier.h>
#include <huxerui/resource.h>
#include <huxerui/text_input.h>
#include <huxerui/theme.h>
#include <huxerui/validation.h>
#include <huxerui/vector.h>

namespace huxerui {
class UiWindow;
}

namespace huxerui::detail {

struct TextFieldStyleBinding {
  using Value = TextFieldStyle;
};

struct TextFieldTrailingIconActionStyleBinding {
  using Value = IconButtonStyle;
};

inline const TextFieldVariantStyle&
ResolveTextFieldVariantStyle(const TextFieldStyle& style, TextFieldVariant variant) noexcept {
  if (variant == TextFieldVariant::Filled) {
    return style.filled;
  }
  if (variant == TextFieldVariant::Outlined) {
    return style.outlined;
  }
  return style.standard;
}

inline CornerRadii ResolveTextFieldCornerRadii(const TextFieldStyle& style, TextFieldVariant variant) noexcept {
  return ResolveTextFieldVariantStyle(style, variant).corner_radii;
}

struct ResolvedValidationResult {
  ValidationStatus status = ValidationStatus::None;
  std::string message;

  [[nodiscard]] bool IsInvalid() const noexcept {
    return status == ValidationStatus::Invalid;
  }

  bool operator==(const ResolvedValidationResult&) const = default;
};

struct TextFieldModifier {
  static const ModifierDescriptor& Descriptor();

  TextEditingValue value;
  StringVariant label;
  StringVariant placeholder;
  std::optional<ImageVariant> leading_icon;
  std::optional<ImageVariant> trailing_icon;
  std::optional<StringVariant> trailing_icon_semantic_label;
  std::optional<TextFieldVariant> variant;
  TextInputConfiguration configuration;
  TextLayoutOptions text_layout_options;
  std::size_t min_lines = 1;
  std::optional<std::size_t> max_lines;
  std::optional<std::size_t> max_length;
  ValidationResult validation;

  bool operator==(const TextFieldModifier&) const = default;
  static bool LayoutEquals(const TextFieldModifier& left, const TextFieldModifier& right);
};

[[nodiscard]] Size MeasureTextField(MountedNode& node, UiWindow& ui_window, Constraints constraints);

} // namespace huxerui::detail
