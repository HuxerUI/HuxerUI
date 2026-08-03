#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include <emscripten/val.h>

#include <huxerui/render_scene.h>
#include <huxerui/text.h>

namespace huxerui::detail {

class TextLayout;

class WebRenderer final {
public:
  WebRenderer(std::uintptr_t session_id, emscripten::val canvas);

  void SetViewport(Size viewport, float display_scale);
  void Invalidate() noexcept;

  [[nodiscard]] FontMetrics Metrics(const Font& font);
  [[nodiscard]] TextRunMetrics
  MeasureRun(std::string_view text, const TextStyle& style, const TextShapingOptions& options = {});
  [[nodiscard]] TextLayoutMetrics
  MeasureText(std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options = {});
  [[nodiscard]] std::unique_ptr<TextLayout> CreateTextLayout(
      std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options = {}
  );

  void Draw(const RenderFrame& frame);

private:
  void RenderSceneNode(const RenderNode& node);
  void RenderSequence(const PaintSequence& sequence);
  void RenderCommand(const DrawRectCommand& command);
  void RenderCommand(const DrawTextCommand& command);
  void RenderCommand(const DrawTextRunsCommand& command);
  void RenderCommand(const DrawImageCommand& command);
  void RenderCommand(const DrawCircleCommand& command);
  void RenderCommand(const DrawArcCommand& command);
  void RenderCommand(const DrawBorderCommand& command);
  void RenderCommand(const DrawShadowCommand& command);
  void RenderCommand(const FillPathCommand& command);
  void RenderCommand(const StrokePathCommand& command);
  void RenderCommand(const DrawPathShadowCommand& command);
  void RenderCommand(const PushClipCommand& command);
  void RenderCommand(const PushPathClipCommand& command);
  void RenderCommand(const PopClipCommand& command);
  void RenderCommand(const PushTransformCommand& command);
  void RenderCommand(const PopTransformCommand& command);

  emscripten::val canvas_;
  emscripten::val context_;
  Size viewport_;
  float display_scale_ = 1.0F;
  std::uintptr_t session_id_ = 0;
  bool force_redraw_ = true;
};

} // namespace huxerui::detail
