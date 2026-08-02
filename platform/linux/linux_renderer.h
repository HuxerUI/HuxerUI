#pragma once

#include <memory>
#include <string_view>

#include "linux_internal.h"

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

// Rasterizes the committed RenderScene with Cairo into a retained device-pixel
// bitmap, then presents it through a Vulkan swap chain backed by an X11 window.
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

  // Damage rectangles are pixel-aligned and clamped to the client area.
  [[nodiscard]] LinuxRenderResult Render(
      Display* display,
      Window window,
      float dpi,
      const RenderFrame& frame,
      const XRectangle* damage_rects,
      unsigned long damage_count
  );

public:
  struct State;

  void RenderSceneNode(const RenderNode& node);
  [[nodiscard]] bool EnsureVulkan(Display* display, Window window);
  [[nodiscard]] bool PresentRetainedBitmap();

  std::unique_ptr<State> state_;
};

} // namespace huxerui::detail
