#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_set>

#include <huxerui/app.h>
#include <huxerui/window.h>

namespace huxerui::detail {

inline constexpr float kLinuxCaptionButtonWidth = 46.0F;
inline constexpr float kLinuxMinTitleBarHeight = 32.0F;

inline WindowTitleBarMetrics ResolveLinuxTitleBarMetrics(
    float preferred_height, Size viewport, bool maximized
) noexcept {
  const float width = std::isfinite(viewport.width) ? std::max(0.0F, viewport.width) : 0.0F;
  const float viewport_height = std::isfinite(viewport.height) ? std::max(0.0F, viewport.height) : 0.0F;
  const float preferred = std::isfinite(preferred_height) ? std::max(0.0F, preferred_height) : 0.0F;
  return {
      .height = std::min(viewport_height, std::max(preferred, kLinuxMinTitleBarHeight)),
      .left_inset = 0.0F,
      .right_inset = std::min(width, 3.0F * kLinuxCaptionButtonWidth),
      .maximized = maximized,
  };
}

inline ApplicationLifecycleState
ResolveLinuxApplicationLifecycleState(bool mapped, bool active, bool minimized) noexcept {
  if (!mapped || minimized) {
    return ApplicationLifecycleState::Background;
  }
  return active ? ApplicationLifecycleState::Active : ApplicationLifecycleState::Inactive;
}

struct LinuxKeyPressResult {
  bool dispatch = false;
  bool repeat = false;

  bool operator==(const LinuxKeyPressResult&) const = default;
};

class LinuxKeyTracker final {
public:
  LinuxKeyPressResult Press(std::uint32_t key_code, bool filtered_by_input_method) {
    if (filtered_by_input_method) {
      filtered_keys_.insert(key_code);
      pressed_keys_.erase(key_code);
      return {};
    }
    filtered_keys_.erase(key_code);
    return {.dispatch = true, .repeat = !pressed_keys_.insert(key_code).second};
  }

  bool Release(std::uint32_t key_code, bool filtered_by_input_method) {
    const bool filtered_press = filtered_keys_.erase(key_code) != 0;
    pressed_keys_.erase(key_code);
    return !filtered_by_input_method && !filtered_press;
  }

  void Reset() noexcept {
    pressed_keys_.clear();
    filtered_keys_.clear();
  }

private:
  std::unordered_set<std::uint32_t> pressed_keys_;
  std::unordered_set<std::uint32_t> filtered_keys_;
};

} // namespace huxerui::detail
