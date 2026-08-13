#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

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
  // Layout results depend only on text, font, the maximum width, and the
  // layout options. Caching them avoids rebuilding grapheme boundaries and
  // re-measuring every cluster through the JS bridge on every frame.
  struct TextLayoutCacheKey {
    std::string text;
    Font font;
    float max_width = 0.0F;
    TextLayoutOptions options;

    bool operator==(const TextLayoutCacheKey&) const = default;
  };

  struct TextLayoutCacheKeyHash {
    [[nodiscard]] std::size_t operator()(const TextLayoutCacheKey& key) const noexcept {
      std::size_t seed = std::hash<std::string>{}(key.text);
      const auto mix = [&seed](std::size_t value) {
        seed ^= value + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
      };
      mix(std::hash<std::string_view>{}(key.font.FamilyName()));
      mix(std::hash<int>{}(static_cast<int>(key.font.FamilyKind())));
      mix(std::hash<float>{}(key.font.Size()));
      mix(std::hash<int>{}(static_cast<int>(key.font.Weight())));
      mix(std::hash<int>{}(static_cast<int>(key.font.Slant())));
      mix(std::hash<float>{}(key.max_width));
      mix(std::hash<int>{}(static_cast<int>(key.options.align)));
      mix(std::hash<int>{}(static_cast<int>(key.options.wrap)));
      mix(std::hash<int>{}(static_cast<int>(key.options.shaping.direction)));
      mix(std::hash<std::string>{}(key.options.shaping.locale));
      return seed;
    }
  };

  [[nodiscard]] const TextLayout* FindOrCreateTextLayout(const TextLayoutCacheKey& key);

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
  void RenderCommand(const PlacePlatformViewCommand& command);

  emscripten::val canvas_;
  emscripten::val context_;
  Size viewport_;
  float display_scale_ = 1.0F;
  std::uintptr_t session_id_ = 0;
  bool force_redraw_ = true;
  std::unordered_map<TextLayoutCacheKey, std::shared_ptr<TextLayout>, TextLayoutCacheKeyHash>
      text_layout_cache_;
};

} // namespace huxerui::detail
