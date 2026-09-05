#pragma once

#include "runtime/mounted_node_internal.h"

#include <optional>
#include <vector>

#include <huxerui/indication.h>
#include <huxerui/theme.h>

namespace huxerui::detail {

struct DefaultIndication {
  static const ModifierDescriptor& Descriptor();

  std::optional<Indication> value;

  bool operator==(const DefaultIndication&) const = default;
};

bool IsDefaultIndicationDescriptor(const ModifierDescriptor* descriptor) noexcept;
bool IsExplicitIndicationDescriptor(const ModifierDescriptor* descriptor) noexcept;

struct IndicationRippleState {
  std::uint64_t press_id = 0;
  Point local_origin;
  RippleEffect effect;
  MotionController expansion{0.0F};
  MotionController opacity{1.0F};
  bool expansion_pending = true;
  bool release_pending = false;
  bool released = false;
};

class IndicationState {
public:
  void Update(Indication spec);
  void Reset();
  void SetInteraction(const InteractionState& state, const std::optional<InteractionEvent>& event, Rect frame);
  [[nodiscard]] MotionAdvanceResult Advance(const FrameInfo& frame);
  void Paint(PaintContext& context, Rect frame, CornerRadii corner_radii, IndicationPlacement placement,
             float opacity = 1.0F) const;
  [[nodiscard]] NodeExtension::PaintInvalidation ActivePaintPhases() const noexcept;
  [[nodiscard]] bool HasVisuals() const noexcept;

private:
  void RetargetLayer(MotionController& controller, bool visible, const std::optional<IndicationLayer>& layer);

  Indication spec_;
  InteractionState interaction_;
  MotionController focus_opacity_{0.0F};
  MotionController hover_opacity_{0.0F};
  MotionController press_opacity_{0.0F};
  std::vector<IndicationRippleState> ripples_;
};

} // namespace huxerui::detail
