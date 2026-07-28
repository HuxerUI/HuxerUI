#include <huxerui/interaction.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <unordered_set>
#include <vector>

#include <huxerui/theme.h>

#include "internal.h"

namespace huxerui {

namespace {

IndicationSpec ResolveIndication(
    const detail::MountedNode &node,
    const Indication &modifier) {
  if (modifier.value.has_value()) {
    return *modifier.value;
  }
  const ThemeSpec theme =
      detail::ResolveThemeSpec(node.environment);
  if (theme.interactions.indication ==
      IndicationKind::Ripple) {
    return RippleIndication{
        .color = theme.interactions.ripple,
        .expansion_duration =
            theme.motion.reduced_motion
                ? 0.0
                : theme.motion.slow,
        .fade_out_duration =
            theme.motion.reduced_motion
                ? 0.0
                : theme.motion.normal,
        .hover_color = theme.interactions.hover_overlay,
        .hover_fade_in_duration =
            theme.motion.reduced_motion
                ? 0.0
                : theme.motion.fast,
        .hover_fade_out_duration =
            theme.motion.reduced_motion
                ? 0.0
                : theme.motion.normal,
    };
  }
  return StateOverlayIndication{
      .color = theme.interactions.pressed_overlay,
      .fade_in_duration =
          theme.motion.reduced_motion ? 0.0 : theme.motion.fast,
      .fade_out_duration =
          theme.motion.reduced_motion ? 0.0 : theme.motion.normal,
      .hover_color = theme.interactions.hover_overlay,
  };
}

struct RippleState {
  std::int64_t pointer_id = 0;
  Point local_origin;
  std::optional<double> started_at;
  std::optional<double> released_at;
  bool release_pending = false;
};

class MountedIndication final : public MountedModifier {
public:
  MountedIndication(
      MountedNode &node, const Indication &modifier) {
    Update(node, modifier);
  }

  MountedIndication(
      MountedNode &node,
      const detail::DefaultIndication &modifier) {
    Update(node, modifier);
  }

  void Update(MountedNode &node, const Indication &modifier) {
    spec_ = ResolveIndication(
        static_cast<detail::MountedNode &>(node), modifier);
    if (std::holds_alternative<NoIndication>(spec_)) {
      opacity_.Set(0.0F);
      hover_opacity_.Set(0.0F);
      pressed_pointers_.clear();
      ripples_.clear();
      keyboard_pressed_ = false;
    }
  }

  void Update(
      MountedNode &node,
      const detail::DefaultIndication &modifier) {
    static_cast<void>(modifier);
    Update(node, Indication{});
  }

  bool HitTest(MountedNode &node, Point position) const override {
    return node.IsEnabled() &&
           node.PresentationFrame().Contains(position);
  }

  bool HoverHitTest(
      MountedNode &node, Point position) const override {
    return !std::holds_alternative<NoIndication>(spec_) &&
           HitTest(node, position);
  }

  void OnHoverChanged(
      MountedNode &node, bool hovered) override {
    static_cast<void>(node);
    if (hovered_ == hovered) {
      return;
    }
    hovered_ = hovered;
    overlay_target_pending_ = true;
  }

  void OnFocusChanged(
      MountedNode &node, bool focused) override {
    static_cast<void>(node);
    if (!focused && keyboard_pressed_) {
      keyboard_pressed_ = false;
      ReleaseKeyboardRipple();
      overlay_target_pending_ = true;
    }
  }

  void OnKey(
      MountedNode &node, const KeyEvent &event) override {
    if (!node.IsEnabled() ||
        std::holds_alternative<NoIndication>(spec_) ||
        (event.key != Key::Enter &&
         event.key != Key::Space)) {
      return;
    }
    const bool pressed =
        event.type == KeyEventType::Down;
    if (pressed && event.repeat) {
      return;
    }
    if (keyboard_pressed_ == pressed) {
      return;
    }
    keyboard_pressed_ = pressed;
    if (std::holds_alternative<StateOverlayIndication>(spec_)) {
      overlay_target_pending_ = true;
      return;
    }
    if (pressed) {
      const Rect frame = node.PresentationFrame();
      ripples_.push_back(RippleState{
          keyboard_pointer_id_,
          {
              frame.width * 0.5F,
              frame.height * 0.5F,
          },
          std::nullopt,
          std::nullopt,
          false,
      });
    } else {
      ReleaseKeyboardRipple();
    }
  }

  ModifierPointerResult OnPointer(
      MountedNode &node, const PointerEvent &event) override {
    if (!node.IsEnabled() ||
        std::holds_alternative<NoIndication>(spec_)) {
      return ModifierPointerResult::Ignored;
    }
    if (event.type == PointerEventType::Down) {
      if (std::holds_alternative<StateOverlayIndication>(spec_)) {
        pressed_pointers_.insert(event.pointer_id);
        overlay_target_pending_ = true;
      } else {
        const Rect frame = node.PresentationFrame();
        ripples_.push_back(RippleState{
            event.pointer_id,
            {
                event.position.x - frame.x,
                event.position.y - frame.y,
            },
            std::nullopt,
            std::nullopt,
            false,
        });
      }
      return ModifierPointerResult::Observe;
    }
    if (event.type == PointerEventType::Up ||
        event.type == PointerEventType::Cancel) {
      if (pressed_pointers_.erase(event.pointer_id) > 0) {
        overlay_target_pending_ = true;
      }
      for (RippleState &ripple : ripples_) {
        if (ripple.pointer_id == event.pointer_id &&
            !ripple.released_at.has_value()) {
          ripple.release_pending = true;
        }
      }
    }
    return ModifierPointerResult::Handled;
  }

  ModifierFrameResult OnFrame(
      MountedNode &node, const FrameInfo &frame) override {
    if (!node.IsEnabled()) {
      opacity_.Set(0.0F);
      hover_opacity_.Set(0.0F);
      pressed_pointers_.clear();
      ripples_.clear();
      keyboard_pressed_ = false;
      overlay_target_pending_ = false;
      return {};
    }
    last_frame_timestamp_ = frame.timestamp;
    bool needs_frame = false;
    if (const auto *overlay =
            std::get_if<StateOverlayIndication>(&spec_)) {
      if (overlay_target_pending_) {
        const bool visible =
            hovered_ || keyboard_pressed_ ||
            !pressed_pointers_.empty();
        opacity_.AnimateTo(
            visible ? 1.0F : 0.0F,
            frame.timestamp,
            visible
                ? overlay->fade_in_duration
                : overlay->fade_out_duration);
        overlay_target_pending_ = false;
      }
      needs_frame = opacity_.Advance(frame.timestamp);
    }

    if (std::holds_alternative<RippleIndication>(spec_)) {
      const auto &ripple_spec =
          std::get<RippleIndication>(spec_);
      hover_opacity_.AnimateTo(
          hovered_ ? 1.0F : 0.0F,
          frame.timestamp,
          hovered_
              ? ripple_spec.hover_fade_in_duration
              : ripple_spec.hover_fade_out_duration);
      needs_frame =
          hover_opacity_.Advance(frame.timestamp) || needs_frame;
      overlay_target_pending_ = false;
      for (RippleState &ripple : ripples_) {
        if (!ripple.started_at.has_value()) {
          ripple.started_at = frame.timestamp;
        }
        if (ripple.release_pending) {
          ripple.release_pending = false;
          ripple.released_at = frame.timestamp;
        }
        const bool expanding =
            ripple_spec.expansion_duration > 0.0 &&
            frame.timestamp - *ripple.started_at <
            ripple_spec.expansion_duration;
        const bool fading =
            ripple.released_at.has_value() &&
            ripple_spec.fade_out_duration > 0.0 &&
            frame.timestamp - *ripple.released_at <
                ripple_spec.fade_out_duration;
        needs_frame = needs_frame || expanding || fading;
      }
      std::erase_if(
          ripples_, [&](const RippleState &ripple) {
            return ripple.released_at.has_value() &&
                   (ripple_spec.fade_out_duration <= 0.0 ||
                    frame.timestamp - *ripple.released_at >=
                        ripple_spec.fade_out_duration);
          });
    } else {
      hover_opacity_.Set(0.0F);
      ripples_.clear();
    }
    return {needs_frame, std::nullopt};
  }

  void Paint(
      const MountedNode &node,
      DisplayList &display_list) const override {
    const Rect frame = node.PresentationFrame();
    const float presentation_opacity =
        node.PresentationOpacity();
    if (const auto *overlay =
        std::get_if<StateOverlayIndication>(&spec_);
        overlay && opacity_.Value() > 0.0F) {
      Color color =
          pressed_pointers_.empty() && !keyboard_pressed_
                        ? overlay->hover_color
                        : overlay->color;
      color.alpha *= opacity_.Value() * presentation_opacity;
      const auto &mounted =
          static_cast<const detail::MountedNode &>(node);
      display_list.DrawRect(
          frame, color, mounted.style.corner_radius);
      return;
    }
    const auto *ripple_spec =
        std::get_if<RippleIndication>(&spec_);
    if (!ripple_spec) {
      return;
    }

    if (hover_opacity_.Value() > 0.0F &&
        ripple_spec->hover_color.alpha > 0.0F) {
      Color hover_color = ripple_spec->hover_color;
      hover_color.alpha *=
          hover_opacity_.Value() * presentation_opacity;
      const auto &mounted =
          static_cast<const detail::MountedNode &>(node);
      display_list.DrawRect(
          frame, hover_color, mounted.style.corner_radius);
    }

    const double now = last_frame_timestamp_;
    const auto &mounted =
        static_cast<const detail::MountedNode &>(node);
    display_list.PushClip(
        frame, mounted.style.corner_radius);
    for (const RippleState &ripple : ripples_) {
      if (!ripple.started_at.has_value()) {
        continue;
      }
      const double expansion =
          ripple_spec->expansion_duration <= 0.0
              ? 1.0
              : std::clamp(
                    (now - *ripple.started_at) /
                        ripple_spec->expansion_duration,
                    0.0, 1.0);
      float alpha = ripple_spec->color.alpha;
      if (ripple.released_at.has_value()) {
        alpha *= static_cast<float>(1.0 - std::clamp(
            (now - *ripple.released_at) /
                std::max(0.001, ripple_spec->fade_out_duration),
            0.0, 1.0));
      }
      Color color = ripple_spec->color;
      color.alpha = alpha * presentation_opacity;
      const float radius =
          std::hypot(frame.width, frame.height) *
          static_cast<float>(expansion);
      display_list.DrawCircle(
          {
              frame.x + ripple.local_origin.x,
              frame.y + ripple.local_origin.y,
          },
          radius, color);
    }
    display_list.PopClip();
  }

private:
  void ReleaseKeyboardRipple() {
    for (RippleState &ripple : ripples_) {
      if (ripple.pointer_id == keyboard_pointer_id_ &&
          !ripple.released_at.has_value()) {
        ripple.release_pending = true;
      }
    }
  }

  static constexpr std::int64_t keyboard_pointer_id_ =
      std::numeric_limits<std::int64_t>::min();

  IndicationSpec spec_ = StateOverlayIndication{};
  detail::AnimatedValue<float> opacity_{0.0F};
  detail::AnimatedValue<float> hover_opacity_{0.0F};
  std::unordered_set<std::int64_t> pressed_pointers_;
  std::vector<RippleState> ripples_;
  bool hovered_ = false;
  bool keyboard_pressed_ = false;
  bool overlay_target_pending_ = false;
  mutable double last_frame_timestamp_ = 0.0;
};

} // namespace

const detail::ModifierDescriptor &Indication::Descriptor() {
  return detail::ModifierDescriptorFor<
      Indication, MountedIndication>();
}

namespace detail {

const ModifierDescriptor &DefaultIndication::Descriptor() {
  return ModifierDescriptorFor<
      DefaultIndication, MountedIndication>();
}

bool IsDefaultIndicationDescriptor(
    const ModifierDescriptor *descriptor) noexcept {
  return descriptor == &DefaultIndication::Descriptor();
}

bool IsExplicitIndicationDescriptor(
    const ModifierDescriptor *descriptor) noexcept {
  return descriptor == &Indication::Descriptor();
}

} // namespace detail

} // namespace huxerui
