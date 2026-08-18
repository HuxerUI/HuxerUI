#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <huxerui/text_input.h>

namespace huxerui::detail {

[[nodiscard]] bool
ShouldFilterXimEvent(int event_type, bool input_context_available, bool active, bool secure) noexcept;

[[nodiscard]] std::optional<int> Utf8CodePointCount(std::string_view text) noexcept;

[[nodiscard]] std::optional<TextOffset> Utf8PrefixUtf16Length(std::string_view text, int code_point_count) noexcept;

[[nodiscard]] std::optional<TextOffset> Utf8BytePrefixUtf16Length(std::string_view text, int byte_count) noexcept;

[[nodiscard]] std::optional<std::size_t> Utf16OffsetToUtf8Byte(std::string_view text, TextOffset offset) noexcept;

[[nodiscard]] bool
ShouldUseFcitxFrontend(const char* xmodifiers, const char* gtk_im_module, const char* qt_im_module) noexcept;

[[nodiscard]] bool ShouldFocusXim(bool focused, bool fcitx_available, bool active, bool secure) noexcept;

[[nodiscard]] std::optional<std::string>
ApplyXimPreeditEdit(std::string_view current, int chg_first, int chg_length, std::string_view replacement);

} // namespace huxerui::detail
