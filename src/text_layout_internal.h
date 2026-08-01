#pragma once

#include <vector>

#include <huxerui/geometry.h>
#include <huxerui/text_input.h>

namespace huxerui::detail {

class TextLayout {
public:
  virtual ~TextLayout() = default;

  [[nodiscard]] virtual Size Measure() const = 0;
  [[nodiscard]] virtual TextPosition HitTest(Point point) const = 0;
  [[nodiscard]] virtual Rect CaretRect(TextOffset offset, TextAffinity affinity) const = 0;
  [[nodiscard]] virtual std::vector<Rect> RangeRects(TextRange range) const = 0;
  [[nodiscard]] virtual TextOffset PreviousCaretOffset(TextOffset offset) const = 0;
  [[nodiscard]] virtual TextOffset NextCaretOffset(TextOffset offset) const = 0;
};

} // namespace huxerui::detail
