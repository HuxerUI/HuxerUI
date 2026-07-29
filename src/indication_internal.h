#pragma once

#include "internal.h"

#include <optional>
#include <unordered_set>
#include <vector>

#include <huxerui/indication.h>
#include <huxerui/theme.h>

namespace huxerui::detail {

struct IndicationRippleState {
  std::int64_t pointer_id = 0;
  Point local_origin;
  std::optional<double> started_at;
  std::optional<double> released_at;
  bool release_pending = false;
};

class IndicationState {
public:
  void Update(IndicationSpec spec);
  void Reset();
  void SetHovered(bool hovered);
  void Press(std::int64_t pointer_id, Point local_origin);
  void Release(std::int64_t pointer_id);
  [[nodiscard]] bool Advance(const FrameInfo& frame);
  void Paint(DisplayList& display_list, Rect frame, float corner_radius, float opacity = 1.0F) const;
  [[nodiscard]] bool HasVisuals() const noexcept;

private:
  IndicationSpec spec_ = StateOverlayIndication{};
  AnimatedValue<float> opacity_{0.0F};
  AnimatedValue<float> hover_opacity_{0.0F};
  std::unordered_set<std::int64_t> pressed_pointers_;
  std::vector<IndicationRippleState> ripples_;
  bool hovered_ = false;
  bool overlay_target_pending_ = false;
  bool released_visual_ = false;
  double last_frame_timestamp_ = 0.0;
};

IndicationSpec ResolveDefaultIndication(const ThemeSpec& theme);

} // namespace huxerui::detail
