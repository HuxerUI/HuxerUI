#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include <emscripten/val.h>

#include <huxerui/render_scene.h>
#include <huxerui/text.h>

namespace huxerui::detail {

class TextLayout;
class WebTextLayout;

class WebRenderer final {
public:
  WebRenderer(std::uintptr_t session_id, emscripten::val canvas);
  ~WebRenderer();

  void SetViewport(Size viewport, float display_scale);
  void Invalidate() noexcept;
  [[nodiscard]] bool TakeInvalidation() noexcept;
  void BeginFrame();

  [[nodiscard]] FontMetrics Metrics(const Font& font);
  [[nodiscard]] TextRunMetrics
  MeasureRun(std::string_view text, const TextStyle& style, const TextShapingOptions& options = {});
  [[nodiscard]] TextLayoutMetrics
  MeasureText(std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options = {});
  [[nodiscard]] std::unique_ptr<TextLayout> CreateTextLayout(
      std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options = {}
  );

  void Draw(const RenderFrame& frame);
  void DrawSlice(
      const emscripten::val& canvas,
      const RenderFrame& frame,
      std::size_t first_command,
      std::size_t command_count,
      bool draw_background,
      bool force_redraw
  );

private:
  struct CachedExternalTexture;
  struct CommandRange;

  [[nodiscard]] const WebTextLayout&
  ParagraphFor(std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options);
  [[nodiscard]] const emscripten::val* FrameFor(const ExternalTexture& texture);
  void DrawTarget(
      const emscripten::val& canvas,
      const RenderFrame& frame,
      CommandRange* range,
      bool draw_background,
      bool force_redraw
  );
  void RenderSceneNode(const RenderNode& node, CommandRange* range);
  void RenderSequence(const PaintSequence& sequence, CommandRange* range);
  void RenderCommand(const DrawRectCommand& command);
  void RenderCommand(const DrawLinearGradientCommand& command);
  void RenderCommand(const DrawRadialGradientCommand& command);
  void RenderCommand(const DrawTextCommand& command);
  void RenderCommand(const DrawTextRunsCommand& command);
  void RenderCommand(const DrawImageCommand& command);
  void RenderCommand(const DrawExternalTextureCommand& command);
  void RenderCommand(const DrawCircleCommand& command);
  void RenderCommand(const DrawLineCommand& command);
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
  void RenderCommand(const PlacePlatformViewCommand& command);

  emscripten::val canvas_;
  emscripten::val context_;
  std::vector<std::unique_ptr<WebTextLayout>> paragraph_cache_;
  std::vector<std::unique_ptr<CachedExternalTexture>> external_textures_;
  Size viewport_;
  float display_scale_ = 1.0F;
  std::uintptr_t session_id_ = 0;
  std::uint64_t external_texture_draw_epoch_ = 0;
  bool force_redraw_ = true;
};

} // namespace huxerui::detail
