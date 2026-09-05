#pragma once

#include <limits>
#include <optional>

#include <huxerui/text_input.h>

#include "text/text_input_internal.h"

namespace huxerui::detail {

[[nodiscard]] inline std::optional<TextSelection> AndroidCursorSelection(
    const TextInputContext& context, TextRange target, TextOffset inserted_length, TextOffset new_cursor_position
) {
  if (target.end > context.total_length || inserted_length < 0) {
    return std::nullopt;
  }
  const TextOffset retained_length = context.total_length - target.Length();
  if (inserted_length > std::numeric_limits<TextOffset>::max() - retained_length) {
    return std::nullopt;
  }
  const TextOffset result_length = retained_length + inserted_length;

  TextOffset cursor = target.start;
  if (new_cursor_position > 0) {
    const TextOffset insertion_end = target.start + inserted_length;
    const TextOffset delta = new_cursor_position - 1;
    cursor = delta > result_length - insertion_end ? result_length : insertion_end + delta;
  } else if (new_cursor_position < 0) {
    const TextOffset magnitude = new_cursor_position == std::numeric_limits<TextOffset>::min()
                                     ? std::numeric_limits<TextOffset>::max()
                                     : -new_cursor_position;
    cursor = magnitude > target.start ? 0 : target.start - magnitude;
  }
  return TextSelection{cursor, cursor};
}

} // namespace huxerui::detail
