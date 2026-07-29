#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <huxerui/text_input.h>

namespace huxerui::detail {

struct TextCompositionBaseline {
  TextRange range;
  std::string text;
  TextSelection selection;

  bool operator==(const TextCompositionBaseline&) const = default;
};

struct TextFieldEditingState {
  TextEditingValue value;
  std::optional<TextCompositionBaseline> composition_baseline;

  bool operator==(const TextFieldEditingState&) const = default;
};

enum class TextInputReductionStatus {
  Accepted,
  Rejected,
};

struct TextInputReductionResult {
  TextInputReductionStatus status = TextInputReductionStatus::Rejected;
  TextFieldEditingState state;
  bool changed = false;
};

[[nodiscard]] std::optional<TextOffset> Utf16Length(std::string_view text) noexcept;
[[nodiscard]] std::optional<std::string> Utf8TextInRange(std::string_view text, TextRange range);
[[nodiscard]] std::optional<TextRange> WordRangeAt(std::string_view text, TextOffset offset);
[[nodiscard]] bool IsValidTextEditingValue(const TextEditingValue& value) noexcept;
[[nodiscard]] TextInputReductionResult
ReduceTextInputCommands(const TextFieldEditingState& state, const std::vector<TextInputCommand>& commands);

} // namespace huxerui::detail
