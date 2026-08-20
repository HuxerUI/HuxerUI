#pragma once

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <huxerui/render_scene.h>
#include <huxerui/window.h>

namespace huxerui::detail {

inline std::wstring Utf8ToWide(std::string_view text) {
  if (text.empty()) {
    return {};
  }
  const int input_size =
      static_cast<int>(std::min<std::size_t>(text.size(), static_cast<std::size_t>(std::numeric_limits<int>::max())));
  const int output_size = MultiByteToWideChar(CP_UTF8, 0, text.data(), input_size, nullptr, 0);
  if (output_size <= 0) {
    return {};
  }
  std::wstring result(static_cast<std::size_t>(output_size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), input_size, result.data(), output_size);
  return result;
}

inline std::string WideToUtf8(std::wstring_view text) {
  if (text.empty()) {
    return {};
  }
  const int input_size =
      static_cast<int>(std::min<std::size_t>(text.size(), static_cast<std::size_t>(std::numeric_limits<int>::max())));
  const int output_size = WideCharToMultiByte(CP_UTF8, 0, text.data(), input_size, nullptr, 0, nullptr, nullptr);
  if (output_size <= 0) {
    return {};
  }
  std::string result(static_cast<std::size_t>(output_size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), input_size, result.data(), output_size, nullptr, nullptr);
  return result;
}

inline std::optional<std::wstring> StrictUtf8ToWide(std::string_view text) {
  if (text.empty()) {
    return std::wstring{};
  }
  if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return std::nullopt;
  }
  const int input_size = static_cast<int>(text.size());
  const int output_size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), input_size, nullptr, 0);
  if (output_size <= 0) {
    return std::nullopt;
  }
  std::wstring result(static_cast<std::size_t>(output_size), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), input_size, result.data(), output_size) !=
      output_size) {
    return std::nullopt;
  }
  return result;
}

inline std::optional<std::string> StrictWideToUtf8(std::wstring_view text) {
  if (text.empty()) {
    return std::string{};
  }
  if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return std::nullopt;
  }
  const int input_size = static_cast<int>(text.size());
  const int output_size =
      WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), input_size, nullptr, 0, nullptr, nullptr);
  if (output_size <= 0) {
    return std::nullopt;
  }
  std::string result(static_cast<std::size_t>(output_size), '\0');
  if (WideCharToMultiByte(
          CP_UTF8,
          WC_ERR_INVALID_CHARS,
          text.data(),
          input_size,
          result.data(),
          output_size,
          nullptr,
          nullptr
      ) != output_size) {
    return std::nullopt;
  }
  return result;
}

struct Win32DamageRegion {
  bool full = false;
  std::vector<RECT> rects;
};

inline RECT InsetWin32MaximizedClientRect(RECT proposed_window, LONG horizontal_inset, LONG vertical_inset) noexcept {
  const LONG resolved_horizontal_inset = std::max(0L, horizontal_inset);
  const LONG resolved_vertical_inset = std::max(0L, vertical_inset);
  proposed_window.left += resolved_horizontal_inset;
  proposed_window.top += resolved_vertical_inset;
  proposed_window.right -= resolved_horizontal_inset;
  proposed_window.bottom -= resolved_vertical_inset;
  return proposed_window;
}

inline float ResolveWin32CaptionButtonWidth(float system_width) noexcept {
  constexpr float modern_caption_button_width = 46.0F;
  return std::max(modern_caption_button_width, system_width);
}

inline WindowTitleBarMetrics ConstrainWin32TitleBarMetrics(WindowTitleBarMetrics metrics, Size viewport) noexcept {
  metrics.height = std::clamp(metrics.height, 0.0F, std::max(0.0F, viewport.height));
  metrics.left_inset = std::clamp(metrics.left_inset, 0.0F, std::max(0.0F, viewport.width));
  metrics.right_inset = std::clamp(metrics.right_inset, 0.0F, std::max(0.0F, viewport.width - metrics.left_inset));
  return metrics;
}

inline Win32DamageRegion ResolveWin32Damage(const DamageRegion& damage, float scale, const RECT& client) noexcept {
  Win32DamageRegion result;
  if (damage.full || !std::isfinite(scale) || scale <= 0.0F) {
    result.full = true;
    return result;
  }

  for (const Rect& rect : damage.rects) {
    if (!std::isfinite(rect.x) || !std::isfinite(rect.y) || !std::isfinite(rect.width) || !std::isfinite(rect.height)) {
      result.full = true;
      result.rects.clear();
      return result;
    }
    if (rect.width <= 0.0F || rect.height <= 0.0F) {
      continue;
    }

    const double x = rect.x;
    const double y = rect.y;
    const double width = rect.width;
    const double height = rect.height;
    const double scale_value = scale;
    const double left = std::clamp(
        std::floor(x * scale_value) + client.left,
        static_cast<double>(client.left),
        static_cast<double>(client.right)
    );
    const double top = std::clamp(
        std::floor(y * scale_value) + client.top,
        static_cast<double>(client.top),
        static_cast<double>(client.bottom)
    );
    const double right = std::clamp(
        std::ceil((x + width) * scale_value) + client.left,
        static_cast<double>(client.left),
        static_cast<double>(client.right)
    );
    const double bottom = std::clamp(
        std::ceil((y + height) * scale_value) + client.top,
        static_cast<double>(client.top),
        static_cast<double>(client.bottom)
    );
    RECT pixel_rect{
        static_cast<LONG>(left),
        static_cast<LONG>(top),
        static_cast<LONG>(right),
        static_cast<LONG>(bottom),
    };
    if (pixel_rect.left < pixel_rect.right && pixel_rect.top < pixel_rect.bottom) {
      result.rects.push_back(pixel_rect);
    }
  }
  return result;
}

inline Rect Win32PixelRectToDips(const RECT& rect, float scale) noexcept {
  if (!std::isfinite(scale) || scale <= 0.0F) {
    return {};
  }
  return {
      static_cast<float>(rect.left) / scale,
      static_cast<float>(rect.top) / scale,
      static_cast<float>(rect.right - rect.left) / scale,
      static_cast<float>(rect.bottom - rect.top) / scale,
  };
}

inline bool Win32RectCovers(const RECT& rect, const RECT& bounds) noexcept {
  return rect.left <= bounds.left && rect.top <= bounds.top && rect.right >= bounds.right &&
         rect.bottom >= bounds.bottom;
}

} // namespace huxerui::detail
