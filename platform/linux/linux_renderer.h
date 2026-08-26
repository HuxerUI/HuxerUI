#pragma once

#include <memory>
#include <string_view>

#include <cairo/cairo.h>

#include <huxerui/render_scene.h>
#include <huxerui/text.h>

namespace huxerui::detail {

class TextLayout;

// Replays the platform-neutral scene into a GTK-owned Cairo context. Window
// surfaces, invalidation, and frame scheduling remain owned by the GTK host.
class LinuxRenderer final {
public:
  LinuxRenderer();
  ~LinuxRenderer();

  LinuxRenderer(const LinuxRenderer&) = delete;
  LinuxRenderer& operator=(const LinuxRenderer&) = delete;

  void Initialize();
  void Discard() noexcept;

  [[nodiscard]] FontMetrics Metrics(const Font& font);
  [[nodiscard]] TextRunMetrics
  MeasureRun(std::string_view text, const TextStyle& style, const TextShapingOptions& options);
  [[nodiscard]] TextLayoutMetrics
  MeasureText(std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options);
  [[nodiscard]] std::unique_ptr<TextLayout>
  CreateTextLayout(std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options);

  void Draw(cairo_t* context, const RenderFrame& frame);

public:
  struct State;

private:
  std::unique_ptr<State> state_;
};

} // namespace huxerui::detail
