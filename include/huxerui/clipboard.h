#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace huxerui {

enum class TextEditingAction {
  Cut,
  Copy,
  Paste,
  SelectAll,
};

struct TextSelectionMenuLabels {
  std::string cut;
  std::string copy;
  std::string paste;
  std::string select_all;

  static TextSelectionMenuLabels Default() {
    return {};
  }

  bool operator==(const TextSelectionMenuLabels&) const = default;
};

class PlatformClipboard {
public:
  virtual ~PlatformClipboard() = default;

  [[nodiscard]] virtual std::optional<std::string> ReadText() = 0;
  virtual bool WriteText(std::string_view text) = 0;
};

} // namespace huxerui
