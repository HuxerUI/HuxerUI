#pragma once

#include <optional>
#include <string_view>

#include <huxerui/text_input.h>

namespace huxerui::detail {

struct LinuxDeleteSurroundingPlan {
  TextOffset before = 0;
  TextOffset after = 0;

  bool operator==(const LinuxDeleteSurroundingPlan&) const = default;
};

[[nodiscard]] std::optional<TextOffset> LinuxUtf8ByteToUtf16(std::string_view text, int byte_offset) noexcept;

[[nodiscard]] std::optional<int> LinuxUtf16ToUtf8Byte(std::string_view text, TextOffset offset) noexcept;

[[nodiscard]] std::optional<LinuxDeleteSurroundingPlan>
ResolveLinuxDeleteSurrounding(int offset, int characters) noexcept;

} // namespace huxerui::detail
