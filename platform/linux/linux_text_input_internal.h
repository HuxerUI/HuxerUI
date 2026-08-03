#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <huxerui/text_input.h>

namespace huxerui::detail {

[[nodiscard]] std::optional<int> Utf8CodePointCount(std::string_view text) noexcept;

[[nodiscard]] std::optional<TextOffset> Utf8PrefixUtf16Length(std::string_view text, int code_point_count) noexcept;

[[nodiscard]] std::optional<std::string>
ApplyXimPreeditEdit(std::string_view current, int chg_first, int chg_length, std::string_view replacement);

} // namespace huxerui::detail
