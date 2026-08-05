#include <huxerui/indication.h>

#include <algorithm>
#include <cmath>
#include <limits>

#include <huxerui/theme.h>

#include "internal.h"
#include "indication_internal.h"

namespace huxerui {

namespace detail {

IndicationSpec ResolveDefaultIndication(const ThemeSpec& theme) {
  if (theme.interactions.indication == IndicationKind::Ripple) {
    return RippleIndication{
        .color = theme.interactions.ripple,
        .expansion_duration = theme.motion.reduced_motion ? 0.0 : theme.motion.slow,
        .fade_out_duration = theme.motion.reduced_motion ? 0.0 : theme.motion.normal,
        .hover_color = theme.interactions.hover_overlay,
        .hover_fade_in_duration = theme.motion.reduced_motion ? 0.0 : theme.motion.fast,
        .hover_fade_out_duration = theme.motion.reduced_motion ? 0.0 : theme.motion.normal,
    };
  }
  return StateOverlayIndication{
      .color = theme.interactions.pressed_overlay,
      .fade_in_duration = theme.motion.reduced_motion ? 0.0 : theme.motion.fast,
      .fade_out_duration = theme.motion.reduced_motion ? 0.0 : theme.motion.normal,
      .hover_color = theme.interactions.hover_overlay,
  };
}

void IndicationState::Update(IndicationSpec spec) {
  spec_ = std::move(spec);
  if (std::holds_alternative<NoIndication>(spec_)) {
    Reset();
  }
}

void IndicationState::Reset() {
  opacity_.Set(0.0F);
  hover_opacity_.Set(0.0F);
  pressed_pointers_.clear();
  ripples_.clear();
  hovered_ = false;
  overlay_target_pending_ = false;
  released_visual_ = false;
}

void IndicationState::SetHovered(bool hovered) {
  if (hovered_ == hovered) {
    return;
  }
  hovered_ = hovered;
  overlay_target_pending_ = true;
}

void IndicationState::Press(std::int64_t pointer_id, Point local_origin) {
  if (std::holds_alternative<NoIndication>(spec_)) {
    return;
  }
  released_visual_ = false;
  if (std::holds_alternative<StateOverlayIndication>(spec_)) {
    pressed_pointers_.insert(pointer_id);
    overlay_target_pending_ = true;
    return;
  }
  ripples_.push_back(
      IndicationRippleState{
          pointer_id,
          local_origin,
          std::nullopt,
          std::nullopt,
          false,
      }
  );
}

void IndicationState::Release(std::int64_t pointer_id) {
  if (pressed_pointers_.erase(pointer_id) > 0) {
    if (opacity_.Value() <= 0.0F) {
      opacity_.Set(1.0F);
    }
    released_visual_ = true;
    overlay_target_pending_ = true;
  }
  for (IndicationRippleState& ripple : ripples_) {
    if (ripple.pointer_id == pointer_id && !ripple.released_at.has_value()) {
      ripple.release_pending = true;
    }
  }
}

bool IndicationState::Advance(const FrameInfo& frame) {
  last_frame_timestamp_ = frame.timestamp;
  bool needs_frame = false;
  if (const auto* overlay = std::get_if<StateOverlayIndication>(&spec_)) {
    if (overlay_target_pending_) {
      const bool visible = hovered_ || !pressed_pointers_.empty();
      opacity_.Update(
          visible ? 1.0F : 0.0F,
          TweenSpec{visible ? overlay->fade_in_duration : overlay->fade_out_duration}
      );
      overlay_target_pending_ = false;
    }
    needs_frame = opacity_.Advance(frame.timestamp, frame.delta_time);
    if (!opacity_.IsRunning() && opacity_.Value() <= 0.0F) {
      released_visual_ = false;
    }
  }

  if (std::holds_alternative<RippleIndication>(spec_)) {
    const auto& ripple_spec = std::get<RippleIndication>(spec_);
    hover_opacity_.Update(
        hovered_ ? 1.0F : 0.0F,
        TweenSpec{hovered_ ? ripple_spec.hover_fade_in_duration : ripple_spec.hover_fade_out_duration}
    );
    needs_frame = hover_opacity_.Advance(frame.timestamp, frame.delta_time) || needs_frame;
    overlay_target_pending_ = false;
    for (IndicationRippleState& ripple : ripples_) {
      if (!ripple.started_at.has_value()) {
        ripple.started_at = frame.timestamp;
      }
      if (ripple.release_pending) {
        ripple.release_pending = false;
        ripple.released_at = frame.timestamp;
      }
      const bool expanding =
          ripple_spec.expansion_duration > 0.0 && frame.timestamp - *ripple.started_at < ripple_spec.expansion_duration;
      const bool fading = ripple.released_at.has_value() && ripple_spec.fade_out_duration > 0.0 &&
                          frame.timestamp - *ripple.released_at < ripple_spec.fade_out_duration;
      needs_frame = needs_frame || expanding || fading;
    }
    std::erase_if(ripples_, [&](const IndicationRippleState& ripple) {
      return ripple.released_at.has_value() && (ripple_spec.fade_out_duration <= 0.0 ||
                                                frame.timestamp - *ripple.released_at >= ripple_spec.fade_out_duration);
    });
  } else {
    hover_opacity_.Set(0.0F);
    ripples_.clear();
  }
  return needs_frame;
}

void IndicationState::Paint(PaintContext& context, Rect frame, CornerRadii corner_radii, float opacity) const {
  if (const auto* overlay = std::get_if<StateOverlayIndication>(&spec_); overlay && opacity_.Value() > 0.0F) {
    const bool pressed = !pressed_pointers_.empty() || (released_visual_ && !hovered_);
    Color color = pressed ? overlay->color : overlay->hover_color;
    color.alpha *= opacity_.Value() * opacity;
    context.DrawRect(frame, color, corner_radii);
    return;
  }
  const auto* ripple_spec = std::get_if<RippleIndication>(&spec_);
  if (!ripple_spec) {
    return;
  }

  if (hover_opacity_.Value() > 0.0F && ripple_spec->hover_color.alpha > 0.0F) {
    Color hover_color = ripple_spec->hover_color;
    hover_color.alpha *= hover_opacity_.Value() * opacity;
    context.DrawRect(frame, hover_color, corner_radii);
  }

  if (ripples_.empty()) {
    return;
  }
  context.PushClip(frame, corner_radii);
  for (const IndicationRippleState& ripple : ripples_) {
    if (!ripple.started_at.has_value()) {
      continue;
    }
    const double expansion =
        ripple_spec->expansion_duration <= 0.0
            ? 1.0
            : std::clamp((last_frame_timestamp_ - *ripple.started_at) / ripple_spec->expansion_duration, 0.0, 1.0);
    float alpha = ripple_spec->color.alpha;
    if (ripple.released_at.has_value()) {
      alpha *= static_cast<float>(
          1.0 - std::clamp(
                    (last_frame_timestamp_ - *ripple.released_at) / std::max(0.001, ripple_spec->fade_out_duration),
                    0.0,
                    1.0
                )
      );
    }
    Color color = ripple_spec->color;
    color.alpha = alpha * opacity;
    const float radius = std::hypot(frame.width, frame.height) * static_cast<float>(expansion);
    context.DrawCircle(
        {
            frame.x + ripple.local_origin.x,
            frame.y + ripple.local_origin.y,
        },
        radius,
        color
    );
  }
  context.PopClip();
}

bool IndicationState::HasVisuals() const noexcept {
  return opacity_.Value() > 0.0F || hover_opacity_.Value() > 0.0F || !pressed_pointers_.empty() || !ripples_.empty() ||
         overlay_target_pending_;
}

} // namespace detail

namespace {

Rect ResolveIndicationFrame(const MountedNode& node) {
  const auto& mounted = static_cast<const detail::MountedNode&>(node);
  if (mounted.indication_frame.has_value()) {
    return *mounted.indication_frame;
  }
  Rect frame = node.Bounds();
  if (!mounted.properties.indication_size.has_value()) {
    return frame;
  }
  const Size size = *mounted.properties.indication_size;
  return {
      frame.x + (frame.width - size.width) * 0.5F,
      frame.y + (frame.height - size.height) * 0.5F,
      size.width,
      size.height,
  };
}

class IndicationExtension final : public NodeExtension {
public:
  IndicationExtension(MountedNode& node, const Indication& modifier) {
    Update(node, modifier);
  }

  IndicationExtension(MountedNode& node, const detail::DefaultIndication& modifier) {
    Update(node, modifier);
  }

  void Update(MountedNode& node, const Indication& modifier) {
    const auto& mounted = static_cast<const detail::MountedNode&>(node);
    spec_ = modifier.value.value_or(detail::ResolveDefaultIndication(detail::ResolveThemeSpec(mounted.environment)));
    indication_.Update(spec_);
    if (std::holds_alternative<NoIndication>(spec_)) {
      keyboard_pressed_ = false;
    }
  }

  void Update(MountedNode& node, const detail::DefaultIndication& modifier) {
    static_cast<void>(modifier);
    const auto& mounted = static_cast<const detail::MountedNode&>(node);
    if (mounted.properties.indication_override.has_value()) {
      Update(node, Indication{*mounted.properties.indication_override});
    } else {
      Update(node, Indication{});
    }
  }

  bool HitTest(MountedNode& node, Point position) const override {
    return !std::holds_alternative<NoIndication>(spec_) && node.IsEnabled() && node.Bounds().Contains(position);
  }

  bool HoverHitTest(MountedNode& node, Point position) const override {
    return HitTest(node, position);
  }

  void OnHoverChanged(MountedNode& node, bool hovered) override {
    static_cast<void>(node);
    indication_.SetHovered(hovered);
    InvalidatePaint();
  }

  void OnFocusChanged(MountedNode& node, bool focused) override {
    static_cast<void>(node);
    if (!focused && keyboard_pressed_) {
      keyboard_pressed_ = false;
      indication_.Release(keyboard_pointer_id_);
      InvalidatePaint();
    }
  }

  void OnKey(MountedNode& node, const KeyEvent& event) override {
    if (!node.IsEnabled() || std::holds_alternative<NoIndication>(spec_) ||
        (event.key != Key::Enter && event.key != Key::Space)) {
      return;
    }
    const bool pressed = event.type == KeyEventType::Down;
    if (pressed && event.repeat) {
      return;
    }
    if (keyboard_pressed_ == pressed) {
      return;
    }
    keyboard_pressed_ = pressed;
    if (pressed) {
      const Rect frame = ResolveIndicationFrame(node);
      indication_.Press(
          keyboard_pointer_id_,
          {
              frame.width * 0.5F,
              frame.height * 0.5F,
          }
      );
    } else {
      indication_.Release(keyboard_pointer_id_);
    }
    InvalidatePaint();
  }

  NodeExtension::PointerResult OnPointer(MountedNode& node, const PointerEvent& event) override {
    if (!node.IsEnabled() || std::holds_alternative<NoIndication>(spec_)) {
      return NodeExtension::PointerResult::Ignored;
    }
    if (event.type == PointerEventType::Down) {
      const Rect frame = ResolveIndicationFrame(node);
      const Point origin = frame.Contains(event.position)
                               ? Point{event.position.x - frame.x, event.position.y - frame.y}
                               : Point{frame.width * 0.5F, frame.height * 0.5F};
      indication_.Press(event.pointer_id, origin);
      InvalidatePaint();
      return NodeExtension::PointerResult::Observe;
    }
    if (event.type == PointerEventType::Up || event.type == PointerEventType::Cancel) {
      indication_.Release(event.pointer_id);
      InvalidatePaint();
    }
    return NodeExtension::PointerResult::Handled;
  }

  NodeExtension::FrameResult OnFrame(MountedNode& node, const FrameInfo& frame) override {
    if (!node.IsEnabled()) {
      const bool had_visuals = indication_.HasVisuals();
      indication_.Reset();
      keyboard_pressed_ = false;
      if (had_visuals) {
        InvalidatePaint();
      }
      return {};
    }
    const bool needs_frame = indication_.Advance(frame);
    if (needs_frame || paint_frame_active_) {
      InvalidatePaint();
    }
    paint_frame_active_ = needs_frame;
    return {needs_frame, std::nullopt};
  }

  void Paint(const MountedNode& node, PaintContext& context) const override {
    const auto& mounted = static_cast<const detail::MountedNode&>(node);
    const Rect frame = ResolveIndicationFrame(node);
    const CornerRadii corner_radii = mounted.properties.indication_size.has_value()
                                         ? CornerRadii{mounted.properties.indication_corner_radius}
                                         : mounted.properties.corner_radii;
    indication_.Paint(context, frame, corner_radii);
  }

private:
  static constexpr std::int64_t keyboard_pointer_id_ = std::numeric_limits<std::int64_t>::min();

  IndicationSpec spec_ = StateOverlayIndication{};
  detail::IndicationState indication_;
  bool keyboard_pressed_ = false;
  bool paint_frame_active_ = false;
};

} // namespace

const detail::ModifierDescriptor& Indication::Descriptor() {
  return detail::ModifierDescriptorFor<Indication, IndicationExtension>();
}

namespace detail {

const ModifierDescriptor& DefaultIndication::Descriptor() {
  return ModifierDescriptorFor<DefaultIndication, IndicationExtension>();
}

bool IsDefaultIndicationDescriptor(const ModifierDescriptor* descriptor) noexcept {
  return descriptor == &DefaultIndication::Descriptor();
}

bool IsExplicitIndicationDescriptor(const ModifierDescriptor* descriptor) noexcept {
  return descriptor == &Indication::Descriptor();
}

} // namespace detail

} // namespace huxerui
