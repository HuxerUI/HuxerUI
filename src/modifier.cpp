#include "internal.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>

#include <huxerui/theme.h>

namespace huxerui {

namespace {

ScrollBarStyle ResolveScrollBarStyle(
    const std::shared_ptr<const detail::EnvironmentFrame>& environment,
    const std::optional<ScrollBarStyle>& explicit_style
) {
  if (explicit_style.has_value()) {
    return *explicit_style;
  }
  if (const std::any* value = detail::FindThemeStyleValue(environment, typeid(ScrollBarStyleKey))) {
    if (const auto* style = std::any_cast<ScrollBarStyle>(value)) {
      return *style;
    }
    throw std::logic_error("HuxerUI scroll bar style environment value has an invalid type");
  }
  const ThemeSpec theme = detail::ResolveThemeSpec(environment);
  ScrollBarStyle style = ScrollBarStyleKey::Default();
  style.fade_in_duration = theme.motion.reduced_motion ? 0.0F : static_cast<float>(theme.motion.fast);
  style.fade_out_duration = theme.motion.reduced_motion ? 0.0F : static_cast<float>(theme.motion.normal);
  style.track_color = theme.colors.on_surface;
  style.track_color.alpha *= 0.08F;
  style.thumb_color = theme.colors.on_surface;
  style.thumb_color.alpha *= 0.55F;
  return style;
}

std::optional<detail::ScrollBarGeometry> ResolveLocalScrollBarGeometry(MountedNode& node) {
  return detail::ResolveScrollBarGeometry(static_cast<detail::MountedNode&>(node));
}

class ScrollBarExtension final : public NodeExtension {
public:
  ScrollBarExtension(MountedNode& node, const ScrollBar& modifier) {
    Update(node, modifier);
  }

  void Update(MountedNode& node, const ScrollBar& modifier) {
    const auto& mounted = static_cast<const detail::MountedNode&>(node);
    style_ = ResolveScrollBarStyle(mounted.environment, modifier.style);
  }

  NodeExtension::FrameResult OnFrame(MountedNode& node, const FrameInfo& frame) override {
    auto& mounted = static_cast<detail::MountedNode&>(node);
    if (!node.IsEnabled() || !detail::ResolveScrollBarGeometry(mounted).has_value()) {
      opacity_.Set(0.0F);
      initialized_ = false;
      return {};
    }

    if (!initialized_) {
      opacity_.Set(1.0F);
      hide_deadline_ = frame.timestamp + style_.fade_out_delay;
      initialized_ = true;
    }
    if (activity_pending_) {
      opacity_.Update(1.0F, TweenSpec{style_.fade_in_duration});
      hide_deadline_ = frame.timestamp + style_.fade_out_delay;
      activity_pending_ = false;
    }
    if (hide_delay_pending_) {
      hide_deadline_ = frame.timestamp + style_.fade_out_delay;
      hide_delay_pending_ = false;
    }

    const bool held = hovered_ || pointer_dragging_ || scroll_dragging_;
    if (held) {
      opacity_.Update(1.0F, TweenSpec{style_.fade_in_duration});
    } else if (frame.timestamp >= hide_deadline_) {
      opacity_.Update(0.0F, TweenSpec{style_.fade_out_duration});
    }

    opacity_.Advance(frame.timestamp, frame.delta_time);
    if (opacity_.IsRunning()) {
      return {
          true,
          std::nullopt,
      };
    }
    if (!held && opacity_.Value() > 0.0F && frame.timestamp < hide_deadline_) {
      return {
          false,
          hide_deadline_ - frame.timestamp,
      };
    }
    return {};
  }

  void OnScrollActivity(MountedNode& node) override {
    static_cast<void>(node);
    activity_pending_ = true;
  }

  void OnScrollGesture(MountedNode& node, bool active) override {
    static_cast<void>(node);
    if (scroll_dragging_ == active) {
      return;
    }
    scroll_dragging_ = active;
    if (active) {
      activity_pending_ = true;
    } else {
      hide_delay_pending_ = true;
    }
  }

  bool HitTest(MountedNode& node, Point position) const override {
    const auto geometry = ResolveLocalScrollBarGeometry(node);
    return node.IsEnabled() && geometry.has_value() && opacity_.Value() > 0.01F && geometry->track.Contains(position);
  }

  bool HoverHitTest(MountedNode& node, Point position) const override {
    return HitTest(node, position);
  }

  void OnHoverChanged(MountedNode& node, bool hovered) override {
    static_cast<void>(node);
    if (hovered_ == hovered) {
      return;
    }
    hovered_ = hovered;
    if (hovered) {
      activity_pending_ = true;
    } else {
      hide_delay_pending_ = true;
    }
  }

  NodeExtension::PointerResult OnPointer(MountedNode& node, const PointerEvent& event) override {
    auto& mounted = static_cast<detail::MountedNode&>(node);
    if (!node.IsEnabled()) {
      pointer_id_.reset();
      pointer_dragging_ = false;
      pointer_draggable_ = false;
      return NodeExtension::PointerResult::Ignored;
    }
    if (event.type == PointerEventType::Down) {
      const auto geometry = ResolveLocalScrollBarGeometry(node);
      if (!geometry.has_value() || opacity_.Value() <= 0.01F || !geometry->track.Contains(event.position)) {
        return NodeExtension::PointerResult::Ignored;
      }

      pointer_id_ = event.pointer_id;
      pointer_axis_ = geometry->axis;
      pointer_origin_ = geometry->axis == Axis::Vertical ? event.position.y : event.position.x;
      offset_origin_ = geometry->scroll_offset;
      maximum_offset_ = geometry->maximum_offset;
      thumb_travel_ = geometry->thumb_travel;
      pointer_draggable_ = geometry->thumb_travel > 0.0F && geometry->thumb.Contains(event.position);
      pointer_dragging_ = true;
      activity_pending_ = true;
      return NodeExtension::PointerResult::Capture;
    }

    if (!pointer_id_.has_value() || *pointer_id_ != event.pointer_id) {
      return NodeExtension::PointerResult::Ignored;
    }

    if (event.type == PointerEventType::Move && pointer_draggable_ && thumb_travel_ > 0.0F) {
      const float pointer = pointer_axis_ == Axis::Vertical ? event.position.y : event.position.x;
      const float desired = std::clamp(
          offset_origin_ + (pointer - pointer_origin_) * maximum_offset_ / thumb_travel_,
          0.0F,
          maximum_offset_
      );
      const float current = pointer_axis_ == Axis::Vertical ? mounted.scroll->offset_y : mounted.scroll->offset_x;
      if (detail::ScrollNodeBy(mounted, desired - current) != 0.0F) {
        activity_pending_ = true;
      }
      return NodeExtension::PointerResult::Handled;
    }

    if (event.type == PointerEventType::Up || event.type == PointerEventType::Cancel) {
      pointer_id_.reset();
      pointer_dragging_ = false;
      pointer_draggable_ = false;
      hide_delay_pending_ = true;
      return NodeExtension::PointerResult::Handled;
    }

    return NodeExtension::PointerResult::Handled;
  }

  void Paint(const MountedNode& node, DisplayList& display_list) const override {
    auto& mutable_node = const_cast<MountedNode&>(node);
    const auto geometry = ResolveLocalScrollBarGeometry(mutable_node);
    if (!geometry.has_value() || opacity_.Value() <= 0.0F) {
      return;
    }

    Color track_color = geometry->style.track_color;
    track_color.alpha *= opacity_.Value() * node.PresentationOpacity();
    Color thumb_color = geometry->style.thumb_color;
    thumb_color.alpha *= opacity_.Value() * node.PresentationOpacity();
    if (track_color.alpha > 0.0F) {
      display_list.DrawRect(geometry->track, track_color, geometry->style.corner_radius);
    }
    if (thumb_color.alpha > 0.0F) {
      display_list.DrawRect(geometry->thumb, thumb_color, geometry->style.corner_radius);
    }
  }

private:
  ScrollBarStyle style_;
  detail::AnimatedValue<float> opacity_{1.0F};
  double hide_deadline_ = 0.0;
  bool initialized_ = false;
  bool activity_pending_ = false;
  bool hide_delay_pending_ = false;
  bool hovered_ = false;
  bool pointer_dragging_ = false;
  bool scroll_dragging_ = false;
  std::optional<std::int64_t> pointer_id_;
  Axis pointer_axis_ = Axis::Vertical;
  float pointer_origin_ = 0.0F;
  float offset_origin_ = 0.0F;
  float maximum_offset_ = 0.0F;
  float thumb_travel_ = 0.0F;
  bool pointer_draggable_ = false;
};

void ValidateScrollBarStyle(const ScrollBarStyle& style) {
  if (!std::isfinite(style.thickness) || style.thickness <= 0.0F || !std::isfinite(style.minimum_thumb_extent) ||
      style.minimum_thumb_extent <= 0.0F || !std::isfinite(style.margin) || style.margin < 0.0F ||
      !std::isfinite(style.corner_radius) || style.corner_radius < 0.0F || !std::isfinite(style.fade_in_duration) ||
      style.fade_in_duration < 0.0F || !std::isfinite(style.fade_out_delay) || style.fade_out_delay < 0.0F ||
      !std::isfinite(style.fade_out_duration) || style.fade_out_duration < 0.0F) {
    throw std::invalid_argument("HuxerUI scroll bar style values must be finite and valid");
  }
}

void ApplyScrollBar(detail::ViewSpec& spec, const ScrollBar& modifier) {
  const ScrollBarStyle style = ResolveScrollBarStyle(spec.environment, modifier.style);
  ValidateScrollBarStyle(style);
  spec.layout_values.insert_or_assign(typeid(detail::ScrollBarBinding), style);
}

void ValidateScrollPhysics(const ScrollPhysics& physics) {
  if (!std::isfinite(physics.deceleration_rate) || physics.deceleration_rate <= 0.0F ||
      !std::isfinite(physics.minimum_fling_velocity) || physics.minimum_fling_velocity <= 0.0F ||
      !std::isfinite(physics.maximum_fling_velocity) ||
      physics.maximum_fling_velocity < physics.minimum_fling_velocity) {
    throw std::invalid_argument("HuxerUI scroll physics values must be finite and valid");
  }
}

void ApplyScrollPhysics(detail::ViewSpec& spec, const ScrollPhysics& physics) {
  ValidateScrollPhysics(physics);
  spec.layout_values.insert_or_assign(typeid(ScrollPhysics), physics);
}

} // namespace

const detail::ModifierDescriptor& ScrollPhysics::Descriptor() {
  static const detail::ModifierDescriptor descriptor{
      typeid(ScrollPhysics),
      [](detail::ViewSpec& spec, const void* value) {
        ApplyScrollPhysics(spec, *static_cast<const ScrollPhysics*>(value));
      },
      nullptr,
      nullptr,
  };
  return descriptor;
}

const detail::ModifierDescriptor& ScrollBar::Descriptor() {
  static const detail::ModifierDescriptor descriptor{
      typeid(ScrollBar),
      [](detail::ViewSpec& spec, const void* value) { ApplyScrollBar(spec, *static_cast<const ScrollBar*>(value)); },
      [](MountedNode& node, const void* value) -> std::unique_ptr<NodeExtension> {
        return std::make_unique<ScrollBarExtension>(node, *static_cast<const ScrollBar*>(value));
      },
      [](NodeExtension& extension, MountedNode& node, const void* value) {
        static_cast<ScrollBarExtension&>(extension).Update(node, *static_cast<const ScrollBar*>(value));
      },
  };
  return descriptor;
}

} // namespace huxerui
