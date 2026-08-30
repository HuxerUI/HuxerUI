#pragma once

namespace huxerui::detail {

constexpr bool IsAsciiAlpha(char value) noexcept {
  return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
}

constexpr bool IsAsciiDigit(char value) noexcept {
  return value >= '0' && value <= '9';
}

constexpr int HexDigitValue(char value) noexcept {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'A' && value <= 'F') {
    return value - 'A' + 10;
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  return -1;
}

constexpr bool IsUriUnreserved(char value) noexcept {
  return IsAsciiAlpha(value) || IsAsciiDigit(value) || value == '-' || value == '.' || value == '_' || value == '~';
}

constexpr bool IsUriSubDelimiter(char value) noexcept {
  switch (value) {
  case '!':
  case '$':
  case '&':
  case '\'':
  case '(':
  case ')':
  case '*':
  case '+':
  case ',':
  case ';':
  case '=':
    return true;
  default:
    return false;
  }
}

} // namespace huxerui::detail
