#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>

#include <huxerui/geometry.h>
#include <huxerui/text.h>

namespace huxerui {
class PlatformAdapter;
class Runtime;
struct WindowTitleBarMetrics;
}

namespace huxerui::detail {

struct MountedNode;
struct ModifierDescriptor;

// Block identity survives virtualization and reordering; the offset is local to that block's committed body.
struct LogicalTextPosition {
  TextBlockId block = 0;
  TextPosition position;

  bool operator==(const LogicalTextPosition&) const = default;
};

struct LogicalTextRange {
  LogicalTextPosition start;
  LogicalTextPosition end;
  std::size_t start_index = 0;
  std::size_t end_index = 0;
};

// Owns snapshot-relative selection only. Copy and endpoint updates never require mounted nodes or platform layouts.
class LogicalTextSelection {
public:
  void SetSource(std::shared_ptr<const TextSelectionSource> source);
  void Clear() noexcept;
  void Select(LogicalTextPosition anchor, LogicalTextPosition active);
  void Extend(LogicalTextPosition active);
  bool SelectAll();
  [[nodiscard]] const std::optional<LogicalTextPosition>& Anchor() const noexcept { return anchor_; }
  [[nodiscard]] std::optional<LogicalTextRange> Range() const;
  [[nodiscard]] std::optional<std::string> Copy() const;
  [[nodiscard]] const std::shared_ptr<const TextSelectionSource>& Source() const noexcept { return source_; }

private:
  std::shared_ptr<const TextSelectionSource> source_;
  std::optional<LogicalTextPosition> anchor_;
  std::optional<LogicalTextPosition> active_;
};

struct SelectionAreaModifier {
  std::shared_ptr<const TextSelectionSource> source;
  static const ModifierDescriptor& Descriptor();

  bool operator==(const SelectionAreaModifier&) const = default;
};

struct TextSelectionBlockKey {
  using Value = TextBlockId;
};

Size MeasureSelectionArea(MountedNode& node, PlatformAdapter& platform, Runtime& runtime,
    const Constraints& constraints, EdgeInsets safe_area, const WindowTitleBarMetrics* title_bar_metrics);

} // namespace huxerui::detail
