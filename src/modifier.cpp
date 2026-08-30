#include "internal.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>

#include <huxerui/theme.h>

namespace huxerui {

namespace {

ScrollBarStyle ResolveScrollBarStyle(
    const std::shared_ptr<const Environment>& environment, const std::optional<ScrollBarStyle>& explicit_style
) {
  if (explicit_style.has_value()) {
    return *explicit_style;
  }
  if (const std::any* value = detail::FindThemeStyleValue(environment, typeid(ScrollBarStyle))) {
    if (const auto* style = std::any_cast<ScrollBarStyle>(value)) {
      return *style;
    }
    throw std::logic_error("HuxerUI scroll bar style environment value has an invalid type");
  }
  const ThemeSpec& theme = detail::ResolveThemeSpec(environment);
  ScrollBarStyle style = ScrollBarStyle::Default();
  style.fade_in_duration = theme.motion.reduced_motion ? 0.0F : static_cast<float>(theme.motion.fast);
  style.fade_out_duration = theme.motion.reduced_motion ? 0.0F : static_cast<float>(theme.motion.normal);
  style.track_color = theme.colors.on_surface;
  style.track_color.alpha *= 0.08F;
  style.thumb_color = theme.colors.on_surface;
  style.thumb_color.alpha *= 0.55F;
  return style;
}

std::optional<detail::ScrollBarGeometry> ResolveLocalScrollBarGeometry(const MountedNode& node) {
  return detail::ResolveScrollBarGeometry(static_cast<const detail::MountedNode&>(node));
}

class ScrollBarExtension final : public NodeExtension {
public:
  ScrollBarExtension(MountedNode& node, const ScrollBar& modifier) {
    Update(node, modifier);
  }

  void Update(MountedNode& node, const ScrollBar& modifier) {
    static_cast<void>(node);
    if (!modifier.style.has_value()) {
      throw std::logic_error("HuxerUI compiled scroll bar style is missing");
    }
    style_ = *modifier.style;
  }

  NodeExtension::FrameResult OnFrame(MountedNode& node, const FrameInfo& frame) override {
    auto& mounted = static_cast<detail::MountedNode&>(node);
    const float previous_opacity = opacity_.Value();
    const auto finish = [&](NodeExtension::FrameResult result) {
      if (opacity_.Value() != previous_opacity) {
        InvalidatePaint();
      }
      return result;
    };
    if (!node.IsEnabled() || !detail::ResolveScrollBarGeometry(mounted).has_value()) {
      opacity_.Set(0.0F);
      initialized_ = false;
      return finish({});
    }

    if (!initialized_) {
      opacity_.Set(1.0F);
      hide_deadline_ = frame.timestamp + style_.fade_out_delay;
      initialized_ = true;
    }
    if (activity_pending_) {
      opacity_.AnimateTo(1.0F, TweenSpec{style_.fade_in_duration});
      hide_deadline_ = frame.timestamp + style_.fade_out_delay;
      activity_pending_ = false;
    }
    if (hide_delay_pending_) {
      hide_deadline_ = frame.timestamp + style_.fade_out_delay;
      hide_delay_pending_ = false;
    }

    const bool held = hovered_ || pointer_dragging_ || scroll_dragging_;
    if (held) {
      opacity_.AnimateTo(1.0F, TweenSpec{style_.fade_in_duration});
    } else if (frame.timestamp >= hide_deadline_) {
      opacity_.AnimateTo(0.0F, TweenSpec{style_.fade_out_duration});
    }

    const MotionAdvanceResult opacity_result = opacity_.Advance(frame);
    if (opacity_result.needs_frame) {
      return finish({
          true,
          opacity_result.wake_after,
      });
    }
    if (!held && opacity_.Value() > 0.0F && frame.timestamp < hide_deadline_) {
      return finish({
          false,
          hide_deadline_ - frame.timestamp,
      });
    }
    return finish({});
  }

  void OnScrollActivity(MountedNode& node) override {
    static_cast<void>(node);
    activity_pending_ = true;
    InvalidatePaint();
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

  void OnHover(MountedNode& node, const HoverEvent& event) override {
    static_cast<void>(node);
    const bool hovered = event.type != HoverEventType::Leave;
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
      const float current = pointer_axis_ == Axis::Vertical ? mounted.scroll_state->offset_y : mounted.scroll_state->offset_x;
      if (detail::ScrollNodeBy(mounted, desired - current) != 0.0F) {
        activity_pending_ = true;
        InvalidatePaint();
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

  void PaintAboveContent(const MountedNode& node, PaintContext& context) const override {
    const auto geometry = ResolveLocalScrollBarGeometry(node);
    if (!geometry.has_value() || opacity_.Value() <= 0.0F) {
      return;
    }

    Color track_color = geometry->style.track_color;
    track_color.alpha *= opacity_.Value();
    Color thumb_color = geometry->style.thumb_color;
    thumb_color.alpha *= opacity_.Value();
    if (track_color.alpha > 0.0F) {
      context.DrawRect(geometry->track, track_color, geometry->style.corner_radius);
    }
    if (thumb_color.alpha > 0.0F) {
      context.DrawRect(geometry->thumb, thumb_color, geometry->style.corner_radius);
    }
  }

private:
  ScrollBarStyle style_;
  MotionController opacity_{1.0F};
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

ScrollBar CompileScrollBar(
    detail::ViewSpec& spec, const std::shared_ptr<const Environment>& environment, const ScrollBar& modifier
) {
  const ScrollBarStyle style = ResolveScrollBarStyle(environment, modifier.style);
  ValidateScrollBarStyle(style);
  spec.layout_values.insert_or_assign(typeid(detail::ScrollBarBinding), detail::MakeErasedLayoutValue(style));
  return ScrollBar{style};
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
  spec.layout_values.insert_or_assign(typeid(ScrollPhysics), detail::MakeErasedLayoutValue(physics));
}

} // namespace

ScrollBarStyle ScrollBarStyle::Default() {
  return {};
}

const detail::ModifierDescriptor& ScrollPhysics::Descriptor() {
  static const detail::ModifierDescriptor descriptor{
      [](detail::ViewSpec& spec,
         detail::ModifierSpec& modifier,
         const std::shared_ptr<const Environment>&,
         detail::AppResources&) { ApplyScrollPhysics(spec, *static_cast<const ScrollPhysics*>(modifier.value.get())); },
      nullptr,
      nullptr,
  };
  return descriptor;
}

const detail::ModifierDescriptor& ScrollBar::Descriptor() {
  static const detail::ModifierDescriptor descriptor{
      [](detail::ViewSpec& spec,
         detail::ModifierSpec& modifier,
         const std::shared_ptr<const Environment>& environment,
         detail::AppResources&) {
        modifier.value = std::make_shared<ScrollBar>(
            CompileScrollBar(spec, environment, *static_cast<const ScrollBar*>(modifier.value.get()))
        );
      },
      [](MountedNode& node, const void* value) -> std::unique_ptr<NodeExtension> {
        return std::make_unique<ScrollBarExtension>(node, *static_cast<const ScrollBar*>(value));
      },
      [](NodeExtension& extension, MountedNode& node, const void* value) {
        static_cast<ScrollBarExtension&>(extension).Update(node, *static_cast<const ScrollBar*>(value));
      },
      false,
      detail::ErasedEqualsFor<ScrollBar>(),
  };
  return descriptor;
}

} // namespace huxerui
