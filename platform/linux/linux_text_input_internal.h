#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <huxerui/text_input.h>

namespace huxerui::detail {

// Pure helpers shared by the XIM adapter and its headless tests. XIM reports
// preedit changes and caret positions in Unicode code points while the shared
// text editing protocol addresses text in UTF-16 code units; these helpers
// translate between the two spaces without touching any Xlib state.

// Counts the code points in a UTF-8 string. Returns std::nullopt when the
// string is not valid UTF-8 or the count would overflow an int.
[[nodiscard]] std::optional<int> Utf8CodePointCount(std::string_view text) noexcept;

// Converts a prefix of code_point_count code points of a UTF-8 string into a
// UTF-16 code unit offset. Returns std::nullopt for invalid UTF-8, a negative
// count, or a count beyond the end of the string.
[[nodiscard]] std::optional<TextOffset> Utf8PrefixUtf16Length(std::string_view text, int code_point_count) noexcept;

// Applies an XIM preedit draw edit: replaces the code-point range
// [chg_first, chg_first + chg_length) of `current` with `replacement`.
// Returns std::nullopt when either string is not valid UTF-8 or the range is
// out of bounds.
[[nodiscard]] std::optional<std::string>
ApplyXimPreeditEdit(std::string_view current, int chg_first, int chg_length, std::string_view replacement);

} // namespace huxerui::detail
