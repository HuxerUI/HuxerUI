#pragma once

#include <memory>
#include <string_view>

#include "linux_internal.h"

#include <cairo/cairo.h>

#include <huxerui/geometry.h>
#include <huxerui/render_scene.h>
#include <huxerui/text.h>

namespace huxerui::detail {

class TextLayout;

enum class LinuxRenderResult {
  Presented,
  Skipped,
  Recreate,
};

class LinuxRenderer final {
public:
  LinuxRenderer();
  ~LinuxRenderer();

  LinuxRenderer(const LinuxRenderer&) = delete;
  LinuxRenderer& operator=(const LinuxRenderer&) = delete;

  void Initialize();
  void Discard() noexcept;
  void ResetDeviceResources() noexcept;
  void Resize(Display* display, Window window, int width, int height, float dpi);
  void DpiChanged(Display* display, Window window, float dpi);

  [[nodiscard]] FontMetrics Metrics(const Font& font);
  [[nodiscard]] TextRunMetrics
  MeasureRun(std::string_view text, const TextStyle& style, const TextShapingOptions& options);
  [[nodiscard]] TextLayoutMetrics
  MeasureText(std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options);
  [[nodiscard]] std::unique_ptr<TextLayout>
  CreateTextLayout(std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options);

  [[nodiscard]] LinuxRenderResult Render(
      Display* display,
      Window window,
      float dpi,
      const RenderFrame& frame,
      const XRectangle* damage_rects,
      unsigned long damage_count
  );

  void Draw(cairo_t* context, const RenderFrame& frame);

public:
  struct State;

  [[nodiscard]] bool InitializePresentation(Display* display);
  [[nodiscard]] bool EnsureGl(Display* display, Window window);
  [[nodiscard]] bool PresentGl();
  [[nodiscard]] bool CanPresentRetained() const noexcept;
  [[nodiscard]] LinuxRenderResult PresentRetained();
  [[nodiscard]] bool HasPresentation() const noexcept;
  [[nodiscard]] unsigned long XVisualId() const noexcept;

  std::unique_ptr<State> state_;
};

} // namespace huxerui::detail
