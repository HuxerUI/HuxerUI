#pragma once

#include <windows.h>

#include <memory>
#include <string_view>

#include <huxerui/geometry.h>
#include <huxerui/render_scene.h>
#include <huxerui/text.h>

namespace huxerui::detail {

class TextLayout;

enum class Win32RenderResult {
  Presented,
  Skipped,
  Retry,
  Recreate,
};

class Win32Renderer final {
public:
  Win32Renderer();
  ~Win32Renderer();

  Win32Renderer(const Win32Renderer&) = delete;
  Win32Renderer& operator=(const Win32Renderer&) = delete;

  void Initialize();
  void Discard() noexcept;
  void ResetDeviceResources() noexcept;
  void Resize(HWND window, float dpi);
  void DpiChanged(HWND window, float dpi);
  [[nodiscard]] bool EnablePlatformComposition(HWND window);
  [[nodiscard]] FontMetrics Metrics(const Font& font);
  [[nodiscard]] TextRunMetrics
  MeasureRun(std::string_view text, const TextStyle& style, const TextShapingOptions& options);
  [[nodiscard]] TextLayoutMetrics
  MeasureText(std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options);
  [[nodiscard]] std::unique_ptr<TextLayout>
  CreateTextLayout(std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options);
  [[nodiscard]] Win32RenderResult Render(HWND window, float dpi, const RenderFrame& frame, const RECT& paint_rect);

private:
  struct State;
  std::unique_ptr<State> state_;
};

} // namespace huxerui::detail
