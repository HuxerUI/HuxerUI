#include <huxerui/clipboard.h>

#include <stdexcept>

#include "text/text_input_internal.h"

namespace huxerui {

bool Clipboard::IsAvailable() const noexcept {
  return platform_ != nullptr;
}

std::optional<std::string> Clipboard::ReadText() const {
  if (platform_ == nullptr) {
    return std::nullopt;
  }
  std::optional<std::string> text = platform_->ReadText();
  if (text.has_value() && !detail::Utf16Length(*text).has_value()) {
    return std::nullopt;
  }
  return text;
}

bool Clipboard::WriteText(std::string_view text) const {
  if (!detail::Utf16Length(text).has_value()) {
    throw std::invalid_argument("HuxerUI clipboard text must contain valid UTF-8");
  }
  return platform_ != nullptr && platform_->WriteText(text);
}

} // namespace huxerui
