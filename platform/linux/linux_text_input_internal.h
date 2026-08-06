#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <huxerui/event.h>
#include <huxerui/text_input.h>

namespace huxerui::detail {

[[nodiscard]] std::optional<int> Utf8CodePointCount(std::string_view text) noexcept;

[[nodiscard]] std::optional<TextOffset> Utf8PrefixUtf16Length(std::string_view text, int code_point_count) noexcept;

[[nodiscard]] std::optional<std::string>
ApplyXimPreeditEdit(std::string_view current, int chg_first, int chg_length, std::string_view replacement);

// True only for keys that may insert text through an input method. Editing,
// navigation, shortcut, and modifier keys are dispatched to the focused view
// instead of the XIM composition path so Backspace and friends are never
// swallowed or committed as mask characters by the input method.
[[nodiscard]] bool IsTextProducingKey(Key key) noexcept;

} // namespace huxerui::detail
