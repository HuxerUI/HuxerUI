#pragma once

#include <memory>
#include <string_view>

#include <SDL3/SDL.h>

#include <huxerui/text.h>

namespace huxerui::detail {

class TextLayout;

struct LinuxRenderedText final {
  std::unique_ptr<SDL_Surface, void (*)(SDL_Surface*)> surface{nullptr, SDL_DestroySurface};
  TextLayoutMetrics metrics;
  float raster_scale = 1.0F;
};

class LinuxTextRenderer final {
public:
  struct State;

  LinuxTextRenderer();
  ~LinuxTextRenderer();

  LinuxTextRenderer(const LinuxTextRenderer&) = delete;
  LinuxTextRenderer& operator=(const LinuxTextRenderer&) = delete;

  [[nodiscard]] FontMetrics Metrics(const Font& font);
  [[nodiscard]] TextRunMetrics
  MeasureRun(std::string_view text, const TextStyle& style, const TextShapingOptions& options);
  [[nodiscard]] TextLayoutMetrics
  MeasureText(std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options);
  [[nodiscard]] std::unique_ptr<TextLayout>
  CreateTextLayout(std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options);
  [[nodiscard]] LinuxRenderedText Render(
      std::string_view text,
      const TextStyle& style,
      float max_width,
      const TextLayoutOptions& options,
      float raster_scale = 1.0F
  );

private:
  std::shared_ptr<State> state_;
};

} // namespace huxerui::detail
