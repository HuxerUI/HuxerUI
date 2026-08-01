#pragma once

#include <windows.h>

#include <memory>
#include <string_view>

#include <huxerui/geometry.h>
#include <huxerui/render_scene.h>

namespace huxerui::detail {

class TextLayout;

enum class Win32RenderResult {
  Presented,
  Skipped,
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
  [[nodiscard]] Size MeasureText(std::string_view text, float font_size, float max_width);
  [[nodiscard]] std::unique_ptr<TextLayout> CreateTextLayout(std::string_view text, float font_size, float max_width);
  [[nodiscard]] Win32RenderResult Render(HWND window, float dpi, const RenderFrame& frame, const RECT& paint_rect);

private:
  struct State;
  std::unique_ptr<State> state_;
};

} // namespace huxerui::detail
