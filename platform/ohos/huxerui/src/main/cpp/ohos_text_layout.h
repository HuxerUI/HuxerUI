#pragma once

#include <memory>
#include <string_view>

#include <huxerui/paint.h>
#include <huxerui/text.h>

struct OH_Drawing_Canvas;

namespace huxerui::detail {

class TextLayoutBackend;

FontMetrics MeasureOhosFont(const Font& font);
TextRunMetrics MeasureOhosRun(std::string_view text, const TextStyle& style, const TextShapingOptions& options);
TextLayoutMetrics MeasureOhosText(const AttributedText& text, const TextStyle& style, float width,
                                  const TextLayoutOptions& options);
std::unique_ptr<TextLayoutBackend> CreateOhosTextLayout(const AttributedText& text, const TextStyle& style, float width,
                                                       const TextLayoutOptions& options);
void PaintOhosText(OH_Drawing_Canvas* canvas, const DrawTextCommand& command, const TextLayoutBackend* prepared);
void PaintOhosTextRuns(OH_Drawing_Canvas* canvas, const DrawTextRunsCommand& command);

} // namespace huxerui::detail
