#include <huxerui/indication.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include <huxerui/theme.h>

#include "indication_internal.h"
#include "internal.h"
#include "resource_internal.h"

namespace huxerui::detail {

namespace {

bool IsEmpty(const Indication& indication) noexcept {
  return !indication.focus.has_value() && !indication.hover.has_value() && !indication.press.has_value() &&
         !indication.ripple.has_value();
}

bool NeedsResourceResolution(const Indication& indication) {
  const auto layer_needs_resolution = [](const std::optional<IndicationLayer>& layer) {
    return layer.has_value() && layer->fill.has_value() && detail::NeedsResourceResolution(*layer->fill);
  };
  return layer_needs_resolution(indication.focus) || layer_needs_resolution(indication.hover) ||
         layer_needs_resolution(indication.press);
}

Indication ResolveIndication(Indication indication, AppResources& resources, const Locale& locale) {
  const auto resolve_layer = [&resources, &locale](std::optional<IndicationLayer>& layer) {
    if (layer.has_value() && layer->fill.has_value()) {
      layer->fill = ResolveVisualFill(*layer->fill, resources, locale);
    }
  };
  resolve_layer(indication.focus);
  resolve_layer(indication.hover);
  resolve_layer(indication.press);
  if (indication.geometry.layer_size.has_value() &&
      (!std::isfinite(indication.geometry.layer_size->width) ||
       !std::isfinite(indication.geometry.layer_size->height) || indication.geometry.layer_size->width < 0.0F ||
       indication.geometry.layer_size->height < 0.0F)) {
    throw std::invalid_argument("HuxerUI indication layer size must be finite and non-negative");
  }
  if (indication.geometry.clip_corner_radii.has_value()) {
    ValidateCornerRadii(
        *indication.geometry.clip_corner_radii,
        "HuxerUI indication clip corner radii must be finite and non-negative"
    );
  }
  const auto validate_layer = [](const std::optional<IndicationLayer>& layer) {
    if (!layer.has_value()) {
      return;
    }
    if (layer->border.has_value()) {
      ValidateBorder(*layer->border);
    }
    if (layer->corner_radii.has_value()) {
      ValidateCornerRadii(*layer->corner_radii, "HuxerUI indication corner radii must be finite and non-negative");
    }
    MotionController enter;
    enter.AnimateTo(1.0F, layer->enter);
    MotionController exit;
    exit.AnimateTo(1.0F, layer->exit);
  };
  validate_layer(indication.focus);
  validate_layer(indication.hover);
  validate_layer(indication.press);
  if (indication.ripple.has_value()) {
    ValidateColor(indication.ripple->color, "HuxerUI ripple color must be finite");
    MotionController expansion;
    expansion.AnimateTo(1.0F, indication.ripple->expansion);
    MotionController fade_out;
    fade_out.AnimateTo(1.0F, indication.ripple->fade_out);
  }
  return indication;
}

void CompileIndicationModifier(
    ViewSpec&, ModifierSpec& modifier, const std::shared_ptr<const Environment>& environment, AppResources& resources
) {
  Indication indication = *static_cast<const Indication*>(modifier.value.get());
  const Locale locale =
      NeedsResourceResolution(indication) ? ResolveResourceLocale(environment, resources) : Locale::Default();
  modifier.value = std::make_shared<Indication>(ResolveIndication(std::move(indication), resources, locale));
}

void CompileDefaultIndicationModifier(
    ViewSpec& spec,
    ModifierSpec& modifier,
    const std::shared_ptr<const Environment>& environment,
    AppResources& resources
) {
  DefaultIndication value = *static_cast<const DefaultIndication*>(modifier.value.get());
  Indication indication;
  if (value.value.has_value()) {
    indication = *value.value;
  } else if (spec.default_indication.has_value()) {
    indication = *spec.default_indication;
  } else {
    indication = ResolveThemeSpec(environment).interactions.indication;
  }
  const Locale locale =
      NeedsResourceResolution(indication) ? ResolveResourceLocale(environment, resources) : Locale::Default();
  value.value = ResolveIndication(std::move(indication), resources, locale);
  modifier.value = std::make_shared<DefaultIndication>(std::move(value));
}

NodeExtension::PaintInvalidation MergeInvalidation(NodeExtension::PaintInvalidation left,
                                                   NodeExtension::PaintInvalidation right) noexcept {
  if (left == NodeExtension::PaintInvalidation::Both || right == NodeExtension::PaintInvalidation::Both) {
    return NodeExtension::PaintInvalidation::Both;
  }
  if (left == NodeExtension::PaintInvalidation::None) {
    return right;
  }
  if (right == NodeExtension::PaintInvalidation::None || left == right) {
    return left;
  }
  return NodeExtension::PaintInvalidation::Both;
}

NodeExtension::PaintInvalidation PlacementInvalidation(IndicationPlacement placement) noexcept {
  return placement == IndicationPlacement::BehindContent ? NodeExtension::PaintInvalidation::Content
                                                         : NodeExtension::PaintInvalidation::Foreground;
}

void PaintLayer(PaintContext& context, Rect frame, CornerRadii corner_radii, const IndicationLayer& layer,
                float opacity) {
  if (opacity <= 0.0F) {
    return;
  }
  if (layer.fill.has_value()) {
    PaintVisualFill(context, frame, *layer.fill, corner_radii, opacity);
  }
}

} // namespace

void IndicationState::Update(Indication spec) {
  spec_ = std::move(spec);
  if (IsEmpty(spec_)) {
    Reset();
    return;
  }
  SetInteraction(interaction_, std::nullopt, {});
}

void IndicationState::Reset() {
  interaction_ = {};
  focus_opacity_.Set(0.0F);
  hover_opacity_.Set(0.0F);
  press_opacity_.Set(0.0F);
  ripples_.clear();
}

void IndicationState::RetargetLayer(MotionController& controller, bool visible,
                                    const std::optional<IndicationLayer>& layer) {
  if (!layer.has_value()) {
    controller.Set(0.0F);
    return;
  }
  const float target = visible ? 1.0F : 0.0F;
  if (controller.Target() == target && (controller.IsRunning() || controller.Value() == target)) {
    return;
  }
  controller.AnimateTo(target, visible ? layer->enter : layer->exit);
}

void IndicationState::SetInteraction(const InteractionState& state, const std::optional<InteractionEvent>& event,
                                     Rect frame) {
  const bool released_before_press_frame = interaction_.pressed && !state.pressed && event.has_value() &&
                                           event->type == InteractionEvent::Type::Release && spec_.press.has_value() &&
                                           press_opacity_.Target() == 1.0F && press_opacity_.Value() == 0.0F &&
                                           press_opacity_.IsRunning();
  interaction_ = state;
  if (!state.enabled || IsEmpty(spec_)) {
    Reset();
    interaction_.enabled = state.enabled;
    return;
  }

  const bool press_visible = state.pressed && spec_.press.has_value();
  const bool hover_visible = !press_visible && state.hovered && spec_.hover.has_value();
  const bool focus_visible = !press_visible && !hover_visible && state.focus_visible && spec_.focus.has_value();
  if (released_before_press_frame) {
    // Preserve feedback when Press and Release arrive before the first animation frame.
    press_opacity_.Set(1.0F);
  }
  RetargetLayer(press_opacity_, press_visible, spec_.press);
  RetargetLayer(hover_opacity_, hover_visible, spec_.hover);
  RetargetLayer(focus_opacity_, focus_visible, spec_.focus);

  if (!event.has_value() || !spec_.ripple.has_value()) {
    return;
  }
  if (event->type == InteractionEvent::Type::Press) {
    const Point origin = event->position.has_value() && frame.Contains(*event->position)
                             ? Point{event->position->x - frame.x, event->position->y - frame.y}
                             : Point{frame.width * 0.5F, frame.height * 0.5F};
    ripples_.push_back(IndicationRippleState{
        .press_id = event->press_id,
        .local_origin = origin,
        .effect = *spec_.ripple,
    });
    return;
  }
  const auto found = std::ranges::find(ripples_, event->press_id, &IndicationRippleState::press_id);
  if (found != ripples_.end() && !found->released) {
    found->release_pending = true;
    found->released = true;
  }
}

MotionAdvanceResult IndicationState::Advance(const FrameInfo& frame) {
  MotionAdvanceResult result;
  const auto advance = [&](MotionController& controller) {
    const MotionAdvanceResult current = controller.Advance(frame);
    result.changed = result.changed || current.changed;
    result.needs_frame = result.needs_frame || current.needs_frame;
    result.wake_after = EarliestWakeAfter(result.wake_after, current.wake_after);
  };
  advance(focus_opacity_);
  advance(hover_opacity_);
  advance(press_opacity_);
  for (IndicationRippleState& ripple : ripples_) {
    if (ripple.expansion_pending) {
      ripple.expansion.AnimateTo(1.0F, ripple.effect.expansion);
      ripple.expansion_pending = false;
    }
    if (ripple.release_pending) {
      ripple.opacity.AnimateTo(0.0F, ripple.effect.fade_out);
      ripple.release_pending = false;
    }
    advance(ripple.expansion);
    advance(ripple.opacity);
  }
  std::erase_if(ripples_, [](const IndicationRippleState& ripple) {
    return ripple.released && !ripple.opacity.IsRunning() && ripple.opacity.Value() <= 0.0F;
  });
  return result;
}

void IndicationState::Paint(PaintContext& context, Rect frame, CornerRadii corner_radii,
                            IndicationPlacement placement, float opacity) const {
  const auto paint_layer = [&](const std::optional<IndicationLayer>& layer, float layer_opacity) {
    if (layer.has_value() && layer->placement == placement) {
      PaintLayer(context, frame, corner_radii, *layer, layer_opacity * opacity);
    }
  };
  paint_layer(spec_.focus, focus_opacity_.Value());
  paint_layer(spec_.hover, hover_opacity_.Value());
  paint_layer(spec_.press, press_opacity_.Value());

  const bool has_ripple = std::ranges::any_of(ripples_, [placement](const IndicationRippleState& ripple) {
    return ripple.effect.placement == placement && ripple.opacity.Value() > 0.0F;
  });
  if (!has_ripple) {
    return;
  }
  context.PushClip(frame, corner_radii);
  for (const IndicationRippleState& ripple : ripples_) {
    if (ripple.effect.placement != placement || ripple.opacity.Value() <= 0.0F) {
      continue;
    }
    Color color = ripple.effect.color;
    color.alpha *= ripple.opacity.Value() * opacity;
    const float radius = std::hypot(frame.width, frame.height) * ripple.expansion.Value();
    context.DrawCircle({frame.x + ripple.local_origin.x, frame.y + ripple.local_origin.y}, radius, color);
  }
  context.PopClip();
}

NodeExtension::PaintInvalidation IndicationState::ActivePaintPhases() const noexcept {
  NodeExtension::PaintInvalidation result = NodeExtension::PaintInvalidation::None;
  const auto merge_layer = [&](const std::optional<IndicationLayer>& layer, const MotionController& opacity) {
    if (layer.has_value() && (opacity.Value() > 0.0F || opacity.IsRunning())) {
      result = MergeInvalidation(result, PlacementInvalidation(layer->placement));
    }
  };
  merge_layer(spec_.focus, focus_opacity_);
  merge_layer(spec_.hover, hover_opacity_);
  merge_layer(spec_.press, press_opacity_);
  for (const IndicationRippleState& ripple : ripples_) {
    result = MergeInvalidation(result, PlacementInvalidation(ripple.effect.placement));
  }
  return result;
}

bool IndicationState::HasVisuals() const noexcept {
  return focus_opacity_.Value() > 0.0F || hover_opacity_.Value() > 0.0F || press_opacity_.Value() > 0.0F ||
         focus_opacity_.IsRunning() || hover_opacity_.IsRunning() || press_opacity_.IsRunning() || !ripples_.empty();
}

} // namespace huxerui::detail

namespace huxerui {

namespace {

CornerRadii InterpolateCornerRadii(CornerRadii from, CornerRadii to, float progress) noexcept {
  return {
      from.top_left + (to.top_left - from.top_left) * progress,
      from.top_right + (to.top_right - from.top_right) * progress,
      from.bottom_right + (to.bottom_right - from.bottom_right) * progress,
      from.bottom_left + (to.bottom_left - from.bottom_left) * progress,
  };
}

std::optional<Border>
InterpolateBorder(const std::optional<Border>& from, const std::optional<Border>& to, float progress) noexcept {
  if (progress <= 0.0F) {
    return from;
  }
  if (progress >= 1.0F) {
    return to;
  }
  if (!from.has_value() && !to.has_value()) {
    return std::nullopt;
  }
  Border start = from.value_or(Border{to->color, 0.0F});
  Border end = to.value_or(Border{from->color, 0.0F});
  if (!from.has_value()) {
    start.color.alpha = 0.0F;
  }
  if (!to.has_value()) {
    end.color.alpha = 0.0F;
  }
  return Border{
      detail::InterpolateColor(start.color, end.color, progress),
      start.width + (end.width - start.width) * progress,
  };
}

Rect ResolveIndicationFrame(const MountedNode& node, const Indication& indication) {
  const auto& mounted = static_cast<const detail::MountedNode&>(node);
  if (mounted.indication_bounds_override.has_value()) {
    return *mounted.indication_bounds_override;
  }
  Rect frame = node.Bounds();
  if (!indication.geometry.layer_size.has_value()) {
    return frame;
  }
  const Size size = *indication.geometry.layer_size;
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
    std::optional<AnimationSpec> departing_exit;
    if (const IndicationLayer* departing = LayerFor(surface_state_)) {
      departing_exit = departing->exit;
    }
    spec_ = modifier;
    indication_.Update(spec_);
    indication_.SetInteraction(node.Interaction(), std::nullopt, ResolveIndicationFrame(node, spec_));
    RetargetSurface(node, std::move(departing_exit));
    InvalidatePaint(PaintInvalidation::Both);
  }

  void Update(MountedNode& node, const detail::DefaultIndication& modifier) {
    if (!modifier.value.has_value()) {
      throw std::logic_error("HuxerUI compiled default indication is missing");
    }
    Update(node, *modifier.value);
  }

  bool HoverHitTest(MountedNode& node, Point position) const override {
    return !detail::IsEmpty(spec_) && node.IsEnabled() && node.Bounds().Contains(position);
  }

  void OnInteraction(MountedNode& node, const InteractionState& state,
                     const std::optional<InteractionEvent>& event) override {
    const PaintInvalidation previous_phases = indication_.ActivePaintPhases();
    indication_.SetInteraction(state, event, ResolveIndicationFrame(node, spec_));
    const PaintInvalidation surface_invalidation = RetargetSurface(node) ? PaintInvalidation::Both
                                                                         : PaintInvalidation::None;
    InvalidatePaint(detail::MergeInvalidation(
        detail::MergeInvalidation(previous_phases, indication_.ActivePaintPhases()), surface_invalidation));
  }

  NodeExtension::FrameResult OnFrame(MountedNode& node, const FrameInfo& frame) override {
    static_cast<void>(node);
    const PaintInvalidation previous_phases = indication_.ActivePaintPhases();
    const MotionAdvanceResult indication_result = indication_.Advance(frame);
    const MotionAdvanceResult surface_result = surface_progress_.Advance(frame);
    const bool needs_frame = surface_result.needs_frame || indication_result.needs_frame;
    const PaintInvalidation active_phases = indication_.ActivePaintPhases();
    PaintInvalidation invalidation = PaintInvalidation::None;
    if (indication_result.changed || previous_phases != active_phases) {
      invalidation = detail::MergeInvalidation(previous_phases, active_phases);
    }
    if (surface_result.changed || surface_progress_.IsRunning()) {
      invalidation = detail::MergeInvalidation(invalidation, PaintInvalidation::Both);
    }
    if (invalidation != PaintInvalidation::None) {
      InvalidatePaint(invalidation);
    }
    return {
        .needs_frame = needs_frame,
        .wake_after = detail::EarliestWakeAfter(indication_result.wake_after, surface_result.wake_after),
    };
  }

  [[nodiscard]] PaintInvalidation PrepareGeometry(MountedNode& node) override {
    auto& mounted = static_cast<detail::MountedNode&>(node);
    if (!mounted.indication_bounds_override.has_value() && spec_.geometry.layer_size.has_value()) {
      const Size size = *spec_.geometry.layer_size;
      const Rect frame = node.Bounds();
      mounted.indication_bounds_override = Rect{
          frame.x + (frame.width - size.width) * 0.5F,
          frame.y + (frame.height - size.height) * 0.5F,
          size.width,
          size.height,
      };
    }
    const SurfaceStyle presented = PresentedSurface();
    if (mounted.resolved_border == presented.border && mounted.resolved_corner_radii == presented.corner_radii) {
      return PaintInvalidation::None;
    }
    mounted.resolved_border = presented.border;
    mounted.resolved_corner_radii = presented.corner_radii;
    return PaintInvalidation::Both;
  }

  void PaintBehindContent(const MountedNode& node, PaintContext& context) const override {
    Paint(node, context, IndicationPlacement::BehindContent);
  }

  void PaintAboveContent(const MountedNode& node, PaintContext& context) const override {
    Paint(node, context, IndicationPlacement::AboveContent);
  }

private:
  enum class SurfaceState {
    Normal,
    Focus,
    Hover,
    Press,
  };

  struct SurfaceStyle {
    std::optional<Border> border;
    CornerRadii corner_radii;

    bool operator==(const SurfaceStyle&) const = default;
  };

  std::pair<SurfaceState, const IndicationLayer*> ActiveSurface(const InteractionState& interaction) const {
    if (!interaction.enabled) {
      return {SurfaceState::Normal, nullptr};
    }
    if (interaction.pressed && spec_.press.has_value()) {
      return {SurfaceState::Press, &*spec_.press};
    }
    if (interaction.hovered && spec_.hover.has_value()) {
      return {SurfaceState::Hover, &*spec_.hover};
    }
    if (interaction.focus_visible && spec_.focus.has_value()) {
      return {SurfaceState::Focus, &*spec_.focus};
    }
    return {SurfaceState::Normal, nullptr};
  }

  const IndicationLayer* LayerFor(SurfaceState state) const {
    switch (state) {
    case SurfaceState::Focus:
      return spec_.focus ? &*spec_.focus : nullptr;
    case SurfaceState::Hover:
      return spec_.hover ? &*spec_.hover : nullptr;
    case SurfaceState::Press:
      return spec_.press ? &*spec_.press : nullptr;
    case SurfaceState::Normal:
      return nullptr;
    }
    return nullptr;
  }

  SurfaceStyle PresentedSurface() const {
    const float progress = std::clamp(surface_progress_.Value(), 0.0F, 1.0F);
    return {
        InterpolateBorder(surface_source_.border, surface_target_.border, progress),
        InterpolateCornerRadii(surface_source_.corner_radii, surface_target_.corner_radii, progress),
    };
  }

  bool RetargetSurface(MountedNode& node, std::optional<AnimationSpec> departing_exit = std::nullopt) {
    const auto& mounted = static_cast<const detail::MountedNode&>(node);
    const std::optional<Border> base_border =
        mounted.applies_disabled_appearance && mounted.properties.disabled_border.has_value()
            ? mounted.properties.disabled_border
            : mounted.properties.border;
    const auto [next_state, active] = ActiveSurface(node.Interaction());
    const SurfaceStyle next{
        active != nullptr && active->border.has_value() ? active->border : base_border,
        active != nullptr && active->corner_radii.has_value() ? *active->corner_radii
                                                              : mounted.properties.corner_radii,
    };
    if (!surface_initialized_) {
      surface_source_ = next;
      surface_target_ = next;
      surface_progress_.Set(1.0F);
      surface_state_ = next_state;
      surface_initialized_ = true;
      return true;
    }
    if (surface_target_ == next) {
      surface_state_ = next_state;
      return false;
    }
    const SurfaceStyle current = PresentedSurface();
    AnimationSpec animation = SnapSpec{};
    if (node.Interaction().enabled) {
      if (active != nullptr) {
        animation = active->enter;
      } else if (departing_exit.has_value()) {
        animation = std::move(*departing_exit);
      } else if (const IndicationLayer* departing = LayerFor(surface_state_)) {
        animation = departing->exit;
      }
    }
    surface_source_ = current;
    surface_target_ = next;
    surface_progress_.Set(0.0F);
    surface_progress_.AnimateTo(1.0F, std::move(animation));
    surface_state_ = next_state;
    return true;
  }

  CornerRadii ResolveCornerRadii(const detail::MountedNode& mounted, float outset = 0.0F) const {
    CornerRadii radii = spec_.geometry.clip_corner_radii.value_or(mounted.resolved_corner_radii);
    radii.top_left += outset;
    radii.top_right += outset;
    radii.bottom_right += outset;
    radii.bottom_left += outset;
    return radii;
  }

  void Paint(const MountedNode& node, PaintContext& context, IndicationPlacement placement) const {
    const auto& mounted = static_cast<const detail::MountedNode&>(node);
    indication_.Paint(context, ResolveIndicationFrame(node, spec_), ResolveCornerRadii(mounted), placement);
  }

  Indication spec_;
  detail::IndicationState indication_;
  SurfaceStyle surface_source_;
  SurfaceStyle surface_target_;
  MotionController surface_progress_{1.0F};
  SurfaceState surface_state_ = SurfaceState::Normal;
  bool surface_initialized_ = false;
};

} // namespace

const detail::ModifierDescriptor& Indication::Descriptor() {
  static const detail::ModifierDescriptor descriptor = [] {
    detail::ModifierDescriptor result = detail::ModifierDescriptorFor<Indication, IndicationExtension>();
    result.compile = detail::CompileIndicationModifier;
    return result;
  }();
  return descriptor;
}

namespace detail {

const ModifierDescriptor& DefaultIndication::Descriptor() {
  static const ModifierDescriptor descriptor = [] {
    ModifierDescriptor result = ModifierDescriptorFor<DefaultIndication, IndicationExtension>();
    result.compile = CompileDefaultIndicationModifier;
    return result;
  }();
  return descriptor;
}

bool IsDefaultIndicationDescriptor(const ModifierDescriptor* descriptor) noexcept {
  return descriptor == &DefaultIndication::Descriptor();
}

bool IsExplicitIndicationDescriptor(const ModifierDescriptor* descriptor) noexcept {
  return descriptor == &Indication::Descriptor();
}

} // namespace detail

} // namespace huxerui
