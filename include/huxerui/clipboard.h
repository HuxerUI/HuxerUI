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
  std::string cut = "Cut";
  std::string copy = "Copy";
  std::string paste = "Paste";
  std::string select_all = "Select all";
};

struct TextSelectionMenuLabelsKey {
  using Value = TextSelectionMenuLabels;

  static Value Default() {
    return {};
  }
};

class PlatformClipboard {
public:
  virtual ~PlatformClipboard() = default;

  [[nodiscard]] virtual std::optional<std::string> ReadText() = 0;
  virtual bool WriteText(std::string_view text) = 0;
};

} // namespace huxerui
