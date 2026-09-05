#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include <huxerui/text.h>

namespace huxerui::detail {

inline bool IsWebHardLineBreak(std::string_view text) noexcept {
  return text == "\n" || text == "\r" || text == "\r\n";
}

inline bool DecodeWebCodePoint(std::string_view text, std::size_t& index, std::uint32_t& result) noexcept {
  if (index >= text.size()) {
    return false;
  }
  const auto first = static_cast<unsigned char>(text[index]);
  if (first < 0x80) {
    result = first;
    ++index;
    return true;
  }

  std::size_t length = 0;
  std::uint32_t value = 0;
  if ((first & 0xE0) == 0xC0) {
    length = 2;
    value = first & 0x1F;
  } else if ((first & 0xF0) == 0xE0) {
    length = 3;
    value = first & 0x0F;
  } else if ((first & 0xF8) == 0xF0) {
    length = 4;
    value = first & 0x07;
  } else {
    ++index;
    return false;
  }
  if (index + length > text.size()) {
    index = text.size();
    return false;
  }
  for (std::size_t offset = 1; offset < length; ++offset) {
    const auto continuation = static_cast<unsigned char>(text[index + offset]);
    if ((continuation & 0xC0) != 0x80) {
      ++index;
      return false;
    }
    value = (value << 6U) | (continuation & 0x3FU);
  }
  const bool overlong =
      (length == 2 && value < 0x80) || (length == 3 && value < 0x800) || (length == 4 && value < 0x10000);
  if (overlong || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
    ++index;
    return false;
  }
  index += length;
  result = value;
  return true;
}

inline bool IsWebTextNeutral(std::uint32_t value) noexcept {
  return value <= 0x20 || (value >= '0' && value <= '9') || (value >= 0x21 && value <= 0x40) ||
         (value >= 0x5B && value <= 0x60) || (value >= 0x7B && value <= 0xA0) || (value >= 0x300 && value <= 0x36F) ||
         (value >= 0x591 && value <= 0x5BD) || value == 0x5BF || (value >= 0x5C1 && value <= 0x5C2) ||
         (value >= 0x5C4 && value <= 0x5C5) || value == 0x5C7 || (value >= 0x610 && value <= 0x61A) ||
         (value >= 0x64B && value <= 0x65F) || value == 0x670 || (value >= 0x660 && value <= 0x669) ||
         (value >= 0x6D6 && value <= 0x6ED) || (value >= 0x6F0 && value <= 0x6F9) ||
         (value >= 0x1AB0 && value <= 0x1AFF) || (value >= 0x1DC0 && value <= 0x1DFF) ||
         (value >= 0x2000 && value <= 0x2BFF) || (value >= 0xFE00 && value <= 0xFE0F) ||
         (value >= 0xFE20 && value <= 0xFE2F) || (value >= 0x1F000 && value <= 0x1FAFF);
}

inline bool IsWebTextRightToLeft(std::uint32_t value) noexcept {
  return (value >= 0x590 && value <= 0x8FF) || (value >= 0xFB1D && value <= 0xFDFF) ||
         (value >= 0xFE70 && value <= 0xFEFF) || (value >= 0x10800 && value <= 0x10FFF) ||
         (value >= 0x1E800 && value <= 0x1EEFF);
}

inline TextDirection ResolveWebTextDirection(std::string_view text, TextDirection direction) noexcept {
  if (direction != TextDirection::Auto) {
    return direction;
  }
  std::size_t index = 0;
  while (index < text.size()) {
    std::uint32_t value = 0;
    if (!DecodeWebCodePoint(text, index, value) || IsWebTextNeutral(value)) {
      continue;
    }
    return IsWebTextRightToLeft(value) ? TextDirection::RightToLeft : TextDirection::LeftToRight;
  }
  return TextDirection::LeftToRight;
}

} // namespace huxerui::detail
