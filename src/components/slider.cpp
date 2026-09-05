#include <huxerui/view.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>

#include <huxerui/semantics.h>
#include <huxerui/animation.h>
#include <huxerui/theme.h>

#include "runtime/mounted_node_internal.h"

namespace huxerui {

namespace {

using detail::ResolveStyleOverride;
using detail::AppliesDisabledAppearance;
using detail::ValidateFocusRing;

struct SliderStyleBinding {
  using Value = SliderStyle;
};

struct SliderVisual {
  static const detail::ModifierDescriptor& Descriptor();

  float value;
  float minimum;
  float maximum;
  std::optional<float> step;

  bool operator==(const SliderVisual&) const = default;
};

class SliderVisualExtension final : public NodeExtension {
public:
  SliderVisualExtension(ViewNode& node, const SliderVisual& modifier) {
    Update(node, modifier);
  }

  void Update(ViewNode& node, const SliderVisual& modifier) {
    style_ = node.LayoutValueOr<SliderStyleBinding>(SliderStyle::Default());
    if (!node.IsEnabled()) {
      pointer_id_.reset();
      hovered_ = false;
      pressed_ = false;
    }
    value_ = std::clamp(modifier.value, modifier.minimum, modifier.maximum);
    minimum_ = modifier.minimum;
    maximum_ = modifier.maximum;
    step_ = modifier.step;
    last_emitted_value_ = value_;
    UpdateThumbSize(node.IsEnabled());
  }

  void BuildSemantics(SemanticBuilder& builder) const override {
    Semantics semantics;
    semantics.role = SemanticRole::Slider;
    semantics.range = SemanticRange{
        minimum_,
        maximum_,
        value_,
        step_.has_value() ? std::optional<double>{*step_} : std::nullopt,
    };
    builder.SetOwner(std::move(semantics));
    builder.AddAction(0, SemanticActionKind::SetValue);
    builder.AddAction(0, SemanticActionKind::Increment);
    builder.AddAction(0, SemanticActionKind::Decrement);
  }

  bool OnSemanticAction(std::uint64_t local_id, const SemanticAction& action) override {
    if (local_id != 0) {
      return false;
    }
    float requested = value_;
    if (action.kind == SemanticActionKind::SetValue) {
      const auto* value = std::get_if<double>(&action.value);
      if (value == nullptr || !std::isfinite(*value)) {
        return false;
      }
      requested = static_cast<float>(*value);
    } else if (action.kind == SemanticActionKind::Increment) {
      requested += step_.value_or((maximum_ - minimum_) / 100.0F);
    } else if (action.kind == SemanticActionKind::Decrement) {
      requested -= step_.value_or((maximum_ - minimum_) / 100.0F);
    } else {
      return false;
    }
    const float snapped = Snap(requested);
    if (snapped != last_emitted_value_) {
      last_emitted_value_ = snapped;
      EmitEvent<SliderEvents::Changed>(snapped);
    }
    return true;
  }

  NodeExtension::FrameResult OnFrame(ViewNode& node, const FrameInfo& frame) override {
    static_cast<void>(node);
    const float previous_width = thumb_width_.Value();
    const float previous_height = thumb_height_.Value();
    const MotionAdvanceResult width_result = thumb_width_.Advance(frame);
    const MotionAdvanceResult height_result = thumb_height_.Advance(frame);
    if (thumb_width_.Value() != previous_width || thumb_height_.Value() != previous_height) {
      InvalidatePaint();
    }
    return {
        .needs_frame = width_result.needs_frame || height_result.needs_frame,
        .wake_after = detail::EarliestWakeAfter(width_result.wake_after, height_result.wake_after),
    };
  }

  [[nodiscard]] bool HitTest(ViewNode& node, Point position) const override {
    return node.IsEnabled() && node.Bounds().Contains(position);
  }

  [[nodiscard]] bool HoverHitTest(ViewNode& node, Point position) const override {
    return HitTest(node, position);
  }

  void OnHover(ViewNode& node, const HoverEvent& event) override {
    static_cast<void>(node);
    const bool hovered = event.type != HoverEventType::Leave;
    if (hovered_ == hovered) {
      return;
    }
    hovered_ = hovered;
    UpdateThumbSize(node.IsEnabled());
  }

  void OnFocusChanged(ViewNode& node, bool focused, bool) override {
    static_cast<void>(node);
    if (focused_ == focused) {
      return;
    }
    focused_ = focused;
    UpdateThumbSize(node.IsEnabled());
  }

  bool OnKey(ViewNode&, const KeyEvent& event) override {
    if (event.type != KeyEventType::Down || event.modifiers.alt || event.modifiers.control || event.modifiers.meta) {
      return false;
    }
    const float increment = step_.value_or((maximum_ - minimum_) / 100.0F);
    switch (event.key) {
    case Key::ArrowLeft:
    case Key::ArrowDown:
      EmitValue(last_emitted_value_ - increment);
      return true;
    case Key::ArrowRight:
    case Key::ArrowUp:
      EmitValue(last_emitted_value_ + increment);
      return true;
    case Key::Home:
      EmitValue(minimum_);
      return true;
    case Key::End:
      EmitValue(maximum_);
      return true;
    default:
      return false;
    }
  }

  PointerResult OnPointer(ViewNode& node, const PointerEvent& event) override {
    if (!node.IsEnabled()) {
      pointer_id_.reset();
      pressed_ = false;
      UpdateThumbSize(false);
      return PointerResult::Ignored;
    }
    if (event.type == PointerEventType::Down) {
      pointer_id_ = event.pointer_id;
      pressed_ = true;
      UpdateThumbSize(true);
      EmitPointerValue(node, event.position.x);
      return PointerResult::Capture;
    }
    if (!pointer_id_.has_value() || *pointer_id_ != event.pointer_id) {
      return PointerResult::Ignored;
    }
    if (event.type == PointerEventType::Move) {
      EmitPointerValue(node, event.position.x);
      return PointerResult::Handled;
    }
    if (event.type == PointerEventType::Up) {
      EmitPointerValue(node, event.position.x);
    }
    if (event.type == PointerEventType::Up || event.type == PointerEventType::Cancel) {
      pointer_id_.reset();
      pressed_ = false;
      UpdateThumbSize(true);
      return PointerResult::Handled;
    }
    return PointerResult::Ignored;
  }

  void PaintAboveContent(const ViewNode& node, PaintContext& context) const override {
    const Rect frame = node.Bounds();
    if (frame.width <= 0.0F || frame.height <= 0.0F) {
      return;
    }
    const Rect track = ResolveTrackBounds(node);
    const float progress = (value_ - minimum_) / (maximum_ - minimum_);
    const float thumb_x = track.x + track.width * progress;
    const float thumb_width = std::clamp(thumb_width_.Value(), 0.0F, frame.width);
    const float thumb_height = std::clamp(thumb_height_.Value(), 0.0F, frame.height);
    const float thumb_half_width = thumb_width * 0.5F;
    const float gap = style_.thumb_track_gap > 0.0F ? thumb_half_width + style_.thumb_track_gap : 0.0F;
    const float active_end = std::clamp(thumb_x - gap, track.x, track.x + track.width);
    const float inactive_start = std::clamp(thumb_x + gap, track.x, track.x + track.width);
    const bool disabled = AppliesDisabledAppearance(node);
    const Color active_track = disabled ? style_.disabled_active_track : style_.active_track;
    const Color inactive_track = disabled ? style_.disabled_inactive_track : style_.inactive_track;
    const Color active_tick = disabled ? style_.disabled_active_tick : style_.active_tick;
    const Color inactive_tick = disabled ? style_.disabled_inactive_tick : style_.inactive_tick;
    const Color stop_indicator = disabled ? style_.disabled_stop_indicator : style_.stop_indicator;
    const Color thumb = disabled ? style_.disabled_thumb : style_.thumb;

    DrawTrackSegment(context, {track.x, track.y, active_end - track.x, track.height}, active_track, true, false);
    DrawTrackSegment(
        context,
        {inactive_start, track.y, track.x + track.width - inactive_start, track.height},
        inactive_track,
        false,
        true
    );
    DrawTicks(context, track, thumb_x, progress, gap, active_tick, inactive_tick);
    DrawStopIndicator(context, track, thumb_x, gap, stop_indicator);

    if (thumb_width > 0.0F && thumb_height > 0.0F && thumb.alpha > 0.0F) {
      context.DrawRect(
          {
              thumb_x - thumb_half_width,
              frame.y + (frame.height - thumb_height) * 0.5F,
              thumb_width,
              thumb_height,
          },
          thumb,
          std::min(thumb_width, thumb_height) * 0.5F
      );
    }
  }

private:
  void DrawTrackSegment(PaintContext& context, Rect segment, Color color, bool rounded_start, bool rounded_end) const {
    if (segment.width <= 0.0F || segment.height <= 0.0F || color.alpha <= 0.0F) {
      return;
    }
    const float outer_radius = segment.height * 0.5F;
    const float inside_radius = std::clamp(style_.track_inside_corner_radius, 0.0F, outer_radius);
    float start_radius = rounded_start ? outer_radius : inside_radius;
    float end_radius = rounded_end ? outer_radius : inside_radius;
    const float combined_radius = start_radius + end_radius;
    if (combined_radius > segment.width) {
      const float scale = segment.width / combined_radius;
      start_radius *= scale;
      end_radius *= scale;
    }
    const float right = segment.x + segment.width;
    const float bottom = segment.y + segment.height;
    Path path;
    path.MoveTo({segment.x + start_radius, segment.y})
        .LineTo({right - end_radius, segment.y})
        .QuadraticTo({right, segment.y}, {right, segment.y + end_radius})
        .LineTo({right, bottom - end_radius})
        .QuadraticTo({right, bottom}, {right - end_radius, bottom})
        .LineTo({segment.x + start_radius, bottom})
        .QuadraticTo({segment.x, bottom}, {segment.x, bottom - start_radius})
        .LineTo({segment.x, segment.y + start_radius})
        .QuadraticTo({segment.x, segment.y}, {segment.x + start_radius, segment.y})
        .Close();
    context.FillPath(std::move(path), color);
  }

  void DrawTicks(
      PaintContext& context,
      const Rect& track,
      float thumb_x,
      float progress,
      float gap,
      Color active_color,
      Color inactive_color
  ) const {
    const float tick_size = std::max(0.0F, style_.tick_size);
    if (!step_.has_value() || tick_size <= 0.0F || track.width <= 0.0F || track.height <= 0.0F) {
      return;
    }
    const double interval_count = std::ceil(static_cast<double>(maximum_ - minimum_) / *step_);
    if (!std::isfinite(interval_count) || interval_count <= 1.0 || interval_count > 512.0 ||
        track.width / static_cast<float>(interval_count) < tick_size * 1.5F) {
      return;
    }
    const float radius = tick_size * 0.5F;
    const float center_y = track.y + track.height * 0.5F;
    for (int interval = 1; interval < static_cast<int>(interval_count); ++interval) {
      const float tick_value = std::min(maximum_, minimum_ + static_cast<float>(interval) * *step_);
      const float tick_progress = (tick_value - minimum_) / (maximum_ - minimum_);
      const float tick_x = track.x + track.width * tick_progress;
      if (std::abs(tick_x - thumb_x) <= gap + radius) {
        continue;
      }
      const Color color = tick_progress < progress ? active_color : inactive_color;
      if (color.alpha > 0.0F) {
        context.DrawCircle({tick_x, center_y}, radius, color);
      }
    }
  }

  void DrawStopIndicator(PaintContext& context, const Rect& track, float thumb_x, float gap, Color color) const {
    const float size = std::max(0.0F, style_.stop_indicator_size);
    if (size <= 0.0F || track.width <= 0.0F || track.height <= 0.0F || color.alpha <= 0.0F) {
      return;
    }
    const float radius = size * 0.5F;
    const float stop_x = track.x + track.width - track.height * 0.5F;
    if (std::abs(stop_x - thumb_x) <= gap + radius) {
      return;
    }
    context.DrawCircle({stop_x, track.y + track.height * 0.5F}, radius, color);
  }

  [[nodiscard]] float Snap(float value) const {
    const float clamped = std::clamp(value, minimum_, maximum_);
    if (!step_.has_value() || clamped == minimum_ || clamped == maximum_) {
      return clamped;
    }
    const float steps = std::round((clamped - minimum_) / *step_);
    return std::clamp(minimum_ + steps * *step_, minimum_, maximum_);
  }

  void EmitPointerValue(ViewNode& node, float pointer_x) {
    const Rect track = ResolveTrackBounds(node);
    const float progress = track.width > 0.0F ? std::clamp((pointer_x - track.x) / track.width, 0.0F, 1.0F) : 0.0F;
    EmitValue(minimum_ + (maximum_ - minimum_) * progress);
  }

  [[nodiscard]] Rect ResolveTrackBounds(const ViewNode& node) const {
    const Rect frame = node.Bounds();
    const float maximum_thumb_width =
        std::max({style_.thumb_width, style_.hovered_thumb_width, style_.pressed_thumb_width, 0.0F});
    const float inset = std::min(frame.width * 0.5F, maximum_thumb_width * 0.5F);
    const float height = std::clamp(style_.track_height, 0.0F, frame.height);
    return {
        frame.x + inset,
        frame.y + (frame.height - height) * 0.5F,
        std::max(0.0F, frame.width - inset * 2.0F),
        height,
    };
  }

  void EmitValue(float value) {
    const float snapped = Snap(value);
    if (snapped == last_emitted_value_) {
      return;
    }
    last_emitted_value_ = snapped;
    EmitEvent<SliderEvents::Changed>(snapped);
  }

  void UpdateThumbSize(bool enabled) {
    float target_width = style_.thumb_width;
    float target_height = style_.thumb_height;
    if (enabled && (pressed_ || focused_)) {
      target_width = style_.pressed_thumb_width;
      target_height = style_.pressed_thumb_height;
    } else if (enabled && hovered_) {
      target_width = style_.hovered_thumb_width;
      target_height = style_.hovered_thumb_height;
    }
    target_width = std::max(0.0F, target_width);
    target_height = std::max(0.0F, target_height);
    if (!thumb_size_initialized_) {
      thumb_width_.Set(target_width);
      thumb_height_.Set(target_height);
      thumb_size_initialized_ = true;
      return;
    }
    const TweenSpec animation{style_.animation_duration};
    thumb_width_.AnimateTo(target_width, animation);
    thumb_height_.AnimateTo(target_height, animation);
  }

  SliderStyle style_;
  MotionController thumb_width_;
  MotionController thumb_height_;
  std::optional<std::int64_t> pointer_id_;
  std::optional<float> step_;
  float value_ = 0.0F;
  float minimum_ = 0.0F;
  float maximum_ = 1.0F;
  float last_emitted_value_ = 0.0F;
  bool hovered_ = false;
  bool pressed_ = false;
  bool focused_ = false;
  bool thumb_size_initialized_ = false;
};

const detail::ModifierDescriptor& SliderVisual::Descriptor() {
  return detail::ModifierDescriptorFor<SliderVisual, SliderVisualExtension>();
}

void ApplySliderDefaults(detail::ViewSpec& spec, const std::shared_ptr<const Environment>& environment) {
  const ThemeSpec& theme = detail::ResolveThemeSpec(environment);
  const SliderStyle style = ResolveStyleOverride<SliderStyle>(environment).value_or(detail::DefaultSliderStyle(theme));
  spec.layout_values.insert_or_assign(typeid(SliderStyleBinding), detail::MakeErasedLayoutValue(style));
  spec.properties.frame.width = std::max(0.0F, style.width);
  spec.properties.frame.height = std::max(0.0F, style.height);
  spec.properties.corner_radii = std::max(0.0F, style.height * 0.5F);
  if (style.focus_ring.has_value()) {
    ValidateFocusRing(*style.focus_ring);
    spec.properties.focus_ring = *style.focus_ring;
  }
  spec.properties.disabled_opacity = 1.0F;
}

std::shared_ptr<detail::ViewSpec> MakeSliderSpec(float value) {
  auto spec = std::make_shared<detail::ViewSpec>(detail::NodeKind::Slider);
  spec->defaults = ApplySliderDefaults;
  spec->focusable = true;
  spec->component_semantics.role = SemanticRole::Slider;
  spec->modifiers.push_back(detail::MakeModifierSpec(SliderVisual{value, 0.0F, 1.0F, std::nullopt}));
  return spec;
}

} // namespace

Slider::Slider(float value) : detail::TypedView<Slider>(MakeSliderSpec(value)), value_(value) {
  if (!std::isfinite(value)) {
    throw std::invalid_argument("HuxerUI Slider value must be finite");
  }
}

Slider Slider::Range(float minimum, float maximum) && {
  if (!std::isfinite(minimum) || !std::isfinite(maximum) || minimum >= maximum) {
    throw std::invalid_argument("HuxerUI Slider range must be finite and increasing");
  }
  minimum_ = minimum;
  maximum_ = maximum;
  UpdateModifier();
  return std::move(*this);
}

Slider Slider::Step(float step) && {
  if (!std::isfinite(step) || step <= 0.0F) {
    throw std::invalid_argument("HuxerUI Slider step must be finite and greater than zero");
  }
  step_ = step;
  UpdateModifier();
  return std::move(*this);
}

void Slider::UpdateModifier() {
  SetModifier(detail::MakeModifierSpec(SliderVisual{value_, minimum_, maximum_, step_}));
}

} // namespace huxerui
