#include <huxerui/presentation.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <huxerui/animation.h>
#include <huxerui/root.h>
#include <huxerui/theme.h>

#include "internal.h"

namespace huxerui {

namespace detail {

class ToastService : public std::enable_shared_from_this<ToastService> {
public:
  explicit ToastService(LayerController& layers) : layers_(layers) {}

  bool Dismiss(LayerId id);

private:
  LayerId Show(StringVariant message, ToastOptions options, std::shared_ptr<const Environment> environment);

  LayerController layers_;

  friend class huxerui::ToastHandle;
};

class DialogService {
public:
  explicit DialogService(LayerController& layers) : layers_(layers) {}

  bool Update(LayerId id, ViewFactory content, std::shared_ptr<const Environment> environment);
  bool Update(LayerId id, DialogFactory content, std::shared_ptr<const Environment> environment);
  bool Dismiss(LayerId id);

private:
  LayerId Show(
      StringVariant title,
      StringVariant message,
      StringVariant positive,
      std::optional<StringVariant> negative,
      std::function<void()> on_positive_click,
      std::function<void()> on_negative_click,
      DialogOptions options,
      std::shared_ptr<const Environment> environment
  );
  LayerId Show(ViewFactory content, DialogOptions options, std::shared_ptr<const Environment> environment);
  LayerId Show(DialogFactory content, DialogOptions options, std::shared_ptr<const Environment> environment);
  bool Update(
      LayerId id,
      ViewFactory content,
      const DialogOptions& options,
      std::shared_ptr<const Environment> environment
  );
  std::shared_ptr<LayerTransitionState> ReconcileTransition(
      LayerId id, const std::optional<PresentationMotion>& motion, bool reduced_motion
  );
  static View StandardContent(
      const StringVariant& title,
      const StringVariant& message,
      const StringVariant& positive,
      const std::optional<StringVariant>& negative,
      const std::function<void()>& on_positive_click,
      const std::function<void()>& on_negative_click,
      const DialogStyle& style,
      const LayerController& layers,
      LayerId id
  );

  LayerController layers_;

  friend class huxerui::DialogHandle;
  friend class DialogExtension;
};

void DebugMetricsState::RecordCommit(double commit_time_seconds, const DamageRegion& damage, Size viewport) noexcept {
  viewport_ = viewport;
  const bool damaged = damage.full || !damage.rects.empty();
  if (!damaged) {
    return;
  }
  ++painted_frame_count_;
  if (std::isfinite(commit_time_seconds)) {
    const double non_negative_commit_time = std::max(0.0, commit_time_seconds);
    total_commit_time_seconds_ += non_negative_commit_time;
    maximum_commit_time_seconds_ = std::max(maximum_commit_time_seconds_, non_negative_commit_time);
  }

  const double viewport_area = static_cast<double>(viewport.width) * static_cast<double>(viewport.height);
  if (damage.full) {
    total_damage_ratio_ += 1.0;
  } else if (viewport_area > 0.0) {
    double damaged_area = 0.0;
    for (const Rect& rect : damage.rects) {
      damaged_area +=
          static_cast<double>(std::max(0.0F, rect.width)) * static_cast<double>(std::max(0.0F, rect.height));
    }
    total_damage_ratio_ += std::clamp(damaged_area / viewport_area, 0.0, 1.0);
  }
}

void DebugMetricsState::ResetSampling() noexcept {
  window_initialized_ = false;
  window_started_at_ = 0.0;
  painted_frame_count_ = 0;
  total_commit_time_seconds_ = 0.0;
  maximum_commit_time_seconds_ = 0.0;
  total_damage_ratio_ = 0.0;
  previous_process_metrics_.reset();
  previous_process_timestamp_ = 0.0;
}

DebugMetricsSnapshot DebugMetricsState::Sample(double timestamp) noexcept {
  DebugMetricsSnapshot snapshot;
  snapshot.viewport = viewport_;

  if (!window_initialized_) {
    window_initialized_ = true;
    window_started_at_ = timestamp;
  } else {
    snapshot.painted_frame_count = painted_frame_count_;
    const double elapsed = std::max(0.0, timestamp - window_started_at_);
    if (elapsed > 0.0) {
      snapshot.fps = static_cast<float>(static_cast<double>(painted_frame_count_) / elapsed);
    }
    if (painted_frame_count_ > 0) {
      snapshot.average_commit_time_ms =
          static_cast<float>(total_commit_time_seconds_ * 1000.0 / static_cast<double>(painted_frame_count_));
      snapshot.maximum_commit_time_ms = static_cast<float>(maximum_commit_time_seconds_ * 1000.0);
      snapshot.average_damage_ratio =
          static_cast<float>(total_damage_ratio_ / static_cast<double>(painted_frame_count_));
    }
  }

  if (platform_ != nullptr) {
    const std::optional<ProcessMetrics> process = platform_->QueryProcessMetrics();
    if (process.has_value()) {
      snapshot.memory_usage_bytes = process->memory_usage_bytes;
      if (previous_process_metrics_.has_value()) {
        const double elapsed = timestamp - previous_process_timestamp_;
        const double cpu_delta = process->cpu_time_seconds - previous_process_metrics_->cpu_time_seconds;
        if (elapsed > 0.0 && std::isfinite(cpu_delta) && cpu_delta >= 0.0) {
          const double processor_count = static_cast<double>(std::max<std::uint32_t>(1, process->processor_count));
          snapshot.cpu_percent =
              static_cast<float>(std::clamp(cpu_delta / elapsed / processor_count * 100.0, 0.0, 100.0));
        }
      }
      previous_process_metrics_ = process;
      previous_process_timestamp_ = timestamp;
    }
  }

  window_started_at_ = timestamp;
  painted_frame_count_ = 0;
  total_commit_time_seconds_ = 0.0;
  maximum_commit_time_seconds_ = 0.0;
  total_damage_ratio_ = 0.0;
  return snapshot;
}

} // namespace detail

namespace {

struct ToastLifetime {
  std::weak_ptr<detail::ToastService> service;
  LayerId id = 0;
  double duration = 0.0;

  static const detail::ModifierDescriptor& Descriptor();
};

class ToastLifetimeExtension final : public NodeExtension {
public:
  ToastLifetimeExtension(MountedNode& node, const ToastLifetime& modifier) {
    Update(node, modifier);
  }

  void Update(MountedNode& node, const ToastLifetime& modifier) {
    static_cast<void>(node);
    service_ = modifier.service;
    id_ = modifier.id;
    duration_ = std::max(0.0, modifier.duration);
  }

  NodeExtension::FrameResult OnFrame(MountedNode& node, const FrameInfo& frame) override {
    static_cast<void>(node);
    if (dismissed_) {
      return {};
    }
    if (!started_at_.has_value()) {
      started_at_ = frame.timestamp;
    }
    const double remaining = duration_ - (frame.timestamp - *started_at_);
    if (remaining > 0.0) {
      return {
          false,
          remaining,
      };
    }
    dismissed_ = true;
    if (auto service = service_.lock()) {
      service->Dismiss(id_);
    }
    return {};
  }

private:
  std::weak_ptr<detail::ToastService> service_;
  LayerId id_ = 0;
  double duration_ = 0.0;
  std::optional<double> started_at_;
  bool dismissed_ = false;
};

const detail::ModifierDescriptor& ToastLifetime::Descriptor() {
  return detail::ModifierDescriptorFor<ToastLifetime, ToastLifetimeExtension>();
}

template <class Style>
Style ResolvePresentationStyle(const std::shared_ptr<const Environment>& environment, Style fallback) {
  if (const std::any* value = detail::FindThemeStyleValue(environment, typeid(Style))) {
    if (const auto* style = std::any_cast<Style>(value)) {
      return *style;
    }
    throw std::logic_error("HuxerUI presentation style environment value has an invalid type");
  }
  return fallback;
}

ToastStyle ResolveToastStyle(const std::shared_ptr<const Environment>& environment) {
  return ResolvePresentationStyle<ToastStyle>(environment, ToastStyle::Default());
}

DialogStyle ResolveDialogStyle(const std::shared_ptr<const Environment>& environment) {
  return ResolvePresentationStyle<DialogStyle>(environment, DialogStyle::Default());
}

BottomSheetStyle ResolveBottomSheetStyle(const std::shared_ptr<const Environment>& environment) {
  return ResolvePresentationStyle<BottomSheetStyle>(environment, BottomSheetStyle::Default());
}

MenuStyle ResolveMenuStyle(const std::shared_ptr<const Environment>& environment) {
  return ResolvePresentationStyle<MenuStyle>(environment, MenuStyle::Default());
}

struct PresentationContentMotion {
  std::shared_ptr<detail::LayerTransitionState> state;
  PresentationMotion motion;
  Point slide_direction;
  TransformOrigin origin;
  bool slide_by_content_extent = false;

  static const detail::ModifierDescriptor& Descriptor();
};

class PresentationContentMotionExtension final : public NodeExtension {
public:
  PresentationContentMotionExtension(MountedNode& node, const PresentationContentMotion& modifier) {
    Update(node, modifier);
  }

  void Update(MountedNode& node, const PresentationContentMotion& modifier) {
    static_cast<void>(node);
    if (state_ == modifier.state && motion_ == modifier.motion && slide_direction_ == modifier.slide_direction &&
        origin_ == modifier.origin && slide_by_content_extent_ == modifier.slide_by_content_extent) {
      return;
    }
    const bool state_changed = state_ != modifier.state;
    state_ = modifier.state;
    motion_ = modifier.motion;
    slide_direction_ = modifier.slide_direction;
    origin_ = modifier.origin;
    slide_by_content_extent_ = modifier.slide_by_content_extent;
    if (state_changed) {
      initialized_ = false;
    }
  }

  FrameResult OnFrame(MountedNode& node, const FrameInfo& frame) override {
    if (!state_) {
      return {};
    }
    if (!initialized_) {
      if (state_->target_visible && state_->enter_on_mount) {
        progress_.Set(0.0F);
        target_visible_ = false;
      } else {
        progress_.Set(1.0F);
        target_visible_ = true;
      }
      initialized_ = true;
    }
    if (target_visible_ != state_->target_visible) {
      target_visible_ = state_->target_visible;
      progress_.Update(target_visible_ ? 1.0F : 0.0F, target_visible_ ? state_->enter : state_->exit);
    }

    auto& mounted = static_cast<detail::MountedNode&>(node);
    const bool running = progress_.Advance(frame.timestamp, frame.delta_time, state_->reduced_motion);
    const float progress = progress_.Value();
    if (motion_.initial_scale != 1.0F) {
      const float scale_value = motion_.initial_scale + (1.0F - motion_.initial_scale) * progress;
      const Rect bounds = node.Bounds();
      const Point origin{bounds.x + bounds.width * origin_.x, bounds.y + bounds.height * origin_.y};
      const Transform2D scale{scale_value, 0.0F, 0.0F, scale_value};
      mounted.presentation.local_transform =
          detail::ComposeTransform(detail::AroundOriginTransform(scale, origin), mounted.presentation.local_transform);
    }
    if (slide_by_content_extent_ || motion_.slide_distance > 0.0F) {
      const Rect bounds = node.Bounds();
      float distance = motion_.slide_distance;
      if (slide_by_content_extent_) {
        distance = slide_direction_.x != 0.0F ? bounds.width : bounds.height;
      }
      const Point offset{
          slide_direction_.x * distance * (1.0F - progress),
          slide_direction_.y * distance * (1.0F - progress),
      };
      mounted.presentation.local_transform =
          detail::ComposeTransform(detail::TranslationTransform(offset), mounted.presentation.local_transform);
    }
    return {running, std::nullopt};
  }

private:
  std::shared_ptr<detail::LayerTransitionState> state_;
  PresentationMotion motion_;
  Point slide_direction_;
  TransformOrigin origin_;
  bool slide_by_content_extent_ = false;
  detail::AnimatedValue<float> progress_;
  bool initialized_ = false;
  bool target_visible_ = false;
};

const detail::ModifierDescriptor& PresentationContentMotion::Descriptor() {
  return detail::ModifierDescriptorFor<PresentationContentMotion, PresentationContentMotionExtension>();
}

std::shared_ptr<detail::LayerTransitionState> PresentationTransition(
    const std::optional<PresentationMotion>& motion, bool reduced_motion, bool enter_on_mount = true
) {
  if (!motion.has_value()) {
    return {};
  }
  return std::make_shared<detail::LayerTransitionState>(detail::LayerTransitionState{
      .target_visible = true,
      .enter_on_mount = enter_on_mount,
      .reduced_motion = reduced_motion,
      .hidden_opacity = 0.0F,
      .enter = motion->enter,
      .exit = motion->exit,
      .on_exit_complete = {},
  });
}

void UpdatePresentationTransition(
    const std::shared_ptr<detail::LayerTransitionState>& transition,
    const PresentationMotion& motion,
    bool reduced_motion
) {
  transition->hidden_opacity = 0.0F;
  transition->enter = motion.enter;
  transition->exit = motion.exit;
  transition->reduced_motion = reduced_motion;
}

CrossAxisAlignment ResolveCrossAlignment(HorizontalAlignment alignment) noexcept {
  switch (alignment) {
  case HorizontalAlignment::Start:
    return CrossAxisAlignment::Start;
  case HorizontalAlignment::Center:
    return CrossAxisAlignment::Center;
  case HorizontalAlignment::End:
    return CrossAxisAlignment::End;
  case HorizontalAlignment::Stretch:
    return CrossAxisAlignment::Stretch;
  }
  return CrossAxisAlignment::Start;
}

bool ValidShadow(const Shadow& shadow) {
  return std::isfinite(shadow.offset.x) && std::isfinite(shadow.offset.y) && std::isfinite(shadow.blur_radius) &&
         shadow.blur_radius >= 0.0F && std::isfinite(shadow.spread);
}

bool ValidInsets(const EdgeInsets& insets) noexcept {
  return std::isfinite(insets.top) && insets.top >= 0.0F && std::isfinite(insets.right) && insets.right >= 0.0F &&
         std::isfinite(insets.bottom) && insets.bottom >= 0.0F && std::isfinite(insets.left) && insets.left >= 0.0F;
}

bool ValidMotion(const PresentationMotion& motion) noexcept {
  return std::isfinite(motion.initial_scale) && motion.initial_scale > 0.0F && std::isfinite(motion.slide_distance) &&
         motion.slide_distance >= 0.0F;
}

void ValidateToastStyle(const ToastStyle& style) {
  if (!ValidInsets(style.padding) || !ValidInsets(style.viewport_padding) || !ValidShadow(style.shadow) ||
      !std::isfinite(style.corner_radius) || style.corner_radius < 0.0F || !std::isfinite(style.maximum_width) ||
      style.maximum_width <= 0.0F || (style.motion.has_value() && !ValidMotion(*style.motion))) {
    throw std::invalid_argument(
        "HuxerUI toast geometry, shadow, and motion must be finite with positive maximum width and non-negative extents"
    );
  }
}

void ValidateDialogStyle(const DialogStyle& style) {
  if (!ValidInsets(style.content_padding) || !ValidInsets(style.action_padding) || !ValidShadow(style.shadow) ||
      !std::isfinite(style.content_spacing) || style.content_spacing < 0.0F || !std::isfinite(style.action_spacing) ||
      style.action_spacing < 0.0F || !std::isfinite(style.action_separator_thickness) ||
      style.action_separator_thickness < 0.0F || !std::isfinite(style.action_corner_radius) ||
      style.action_corner_radius < 0.0F || !std::isfinite(style.minimum_action_height) ||
      style.minimum_action_height < 0.0F || !std::isfinite(style.corner_radius) || style.corner_radius < 0.0F ||
      !std::isfinite(style.maximum_width) || style.maximum_width <= 0.0F || !std::isfinite(style.viewport_margin) ||
      style.viewport_margin < 0.0F || (style.motion.has_value() && !ValidMotion(*style.motion))) {
    throw std::invalid_argument(
        "HuxerUI dialog geometry, shadow, and motion must be finite with positive maximum width and non-negative "
        "extents"
    );
  }
}

std::shared_ptr<detail::LayerTransitionState>
BottomSheetTransition(const BottomSheetStyle& style, bool reduced_motion) {
  return PresentationTransition(
      PresentationMotion{
          .enter = style.enter,
          .exit = style.exit,
      },
      reduced_motion
  );
}

void ValidateBottomSheetStyle(const BottomSheetStyle& style) {
  if (!std::isfinite(style.corner_radius) || style.corner_radius < 0.0F || !std::isfinite(style.maximum_width) ||
      style.maximum_width <= 0.0F || !ValidShadow(style.shadow)) {
    throw std::invalid_argument(
        "HuxerUI bottom sheet geometry and shadow must be finite with positive maximum width and non-negative extents"
    );
  }
}

detail::LayerPlacementKind VerticalLayerPlacementKind(VerticalPlacement placement) noexcept {
  switch (placement) {
  case VerticalPlacement::Top:
    return detail::LayerPlacementKind::TopCenter;
  case VerticalPlacement::Center:
    return detail::LayerPlacementKind::Center;
  case VerticalPlacement::Bottom:
    return detail::LayerPlacementKind::BottomCenter;
  }
  return detail::LayerPlacementKind::Center;
}

detail::LayerPlacement DialogLayerPlacement(const DialogStyle& style) {
  detail::LayerPlacement placement;
  placement.kind = VerticalLayerPlacementKind(style.placement);
  placement.viewport_margin = style.viewport_margin;
  return placement;
}

Point VerticalSlideDirection(VerticalPlacement placement) noexcept {
  switch (placement) {
  case VerticalPlacement::Top:
    return {0.0F, -1.0F};
  case VerticalPlacement::Center:
    return {};
  case VerticalPlacement::Bottom:
    return {0.0F, 1.0F};
  }
  return {};
}

TransformOrigin VerticalMotionOrigin(VerticalPlacement placement) noexcept {
  switch (placement) {
  case VerticalPlacement::Top:
    return {0.5F, 0.0F};
  case VerticalPlacement::Center:
    return {0.5F, 0.5F};
  case VerticalPlacement::Bottom:
    return {0.5F, 1.0F};
  }
  return {0.5F, 0.5F};
}

ViewFactory AnimatedDialogContent(
    ViewFactory content, const DialogStyle& style, std::shared_ptr<detail::LayerTransitionState> transition
) {
  return [content = std::move(content), style, transition = std::move(transition)]() -> View {
    View result = content();
    if (!transition || !style.motion.has_value()) {
      return result;
    }
    return Stack {std::move(result)}.With(
        PresentationContentMotion{
            .state = transition,
            .motion = *style.motion,
            .slide_direction = VerticalSlideDirection(style.placement),
            .origin = VerticalMotionOrigin(style.placement),
        }
    );
  };
}

ViewFactory BottomSheetContent(
    ViewFactory content, const BottomSheetStyle& style, std::shared_ptr<detail::LayerTransitionState> transition
) {
  return [content = std::move(content), style, transition = std::move(transition)] {
    return Stack {content()}.With(
        Background{style.background},
        CornerRadius{style.corner_radius},
        style.shadow,
        PresentationContentMotion{
            .state = transition,
            .motion =
                PresentationMotion{
                    .enter = style.enter,
                    .exit = style.exit,
                },
            .slide_direction = {0.0F, 1.0F},
            .origin = TransformOrigin{0.5F, 1.0F},
            .slide_by_content_extent = true,
        }
    );
  };
}

std::shared_ptr<detail::DialogService> DialogServiceFor(const detail::MountedNode& node) {
  const std::any* value = detail::FindEnvironmentValue(node.environment, typeid(detail::DialogService));
  if (!value) {
    throw std::logic_error("HuxerUI dialog service is not available");
  }
  const auto* service = std::any_cast<std::shared_ptr<detail::DialogService>>(value);
  if (!service || !*service) {
    throw std::logic_error("HuxerUI dialog service environment value is invalid");
  }
  return *service;
}

LayerOptions DialogLayerOptions(DialogOptions options, Color scrim) {
  return {
      .level = LayerLevel::Presentation,
      .pointer_policy = LayerPointerPolicy::Barrier,
      .trap_focus = true,
      .dismiss_on_outside_press = options.dismiss_on_outside_press,
      .cancel_policy = options.dismiss_on_cancel ? LayerCancelPolicy::Dismiss : LayerCancelPolicy::Consume,
      .on_dismiss_request = std::move(options.on_dismiss_request),
      .barrier_color = scrim,
  };
}

void ValidateAnchoredOptions(float gap, float viewport_margin, Point offset, const std::optional<Point>& point) {
  if (!std::isfinite(gap) || gap < 0.0F) {
    throw std::invalid_argument("HuxerUI anchored presentation gap must be finite and non-negative");
  }
  if (!std::isfinite(viewport_margin) || viewport_margin < 0.0F) {
    throw std::invalid_argument("HuxerUI anchored presentation viewport margin must be finite and non-negative");
  }
  if (!std::isfinite(offset.x) || !std::isfinite(offset.y)) {
    throw std::invalid_argument("HuxerUI anchored presentation offset must be finite");
  }
  if (point.has_value() && (!std::isfinite(point->x) || !std::isfinite(point->y))) {
    throw std::invalid_argument("HuxerUI anchored presentation point must be finite");
  }
}

detail::LayerAnchorSide ResolveAnchorSide(AnchorSide side) noexcept {
  switch (side) {
  case AnchorSide::Below:
    return detail::LayerAnchorSide::Below;
  case AnchorSide::Above:
    return detail::LayerAnchorSide::Above;
  case AnchorSide::Right:
    return detail::LayerAnchorSide::Right;
  case AnchorSide::Left:
    return detail::LayerAnchorSide::Left;
  }
  return detail::LayerAnchorSide::Below;
}

detail::LayerAnchorAlignment ResolveAnchorAlignment(AnchorAlignment alignment) noexcept {
  switch (alignment) {
  case AnchorAlignment::Start:
    return detail::LayerAnchorAlignment::Start;
  case AnchorAlignment::Center:
    return detail::LayerAnchorAlignment::Center;
  case AnchorAlignment::End:
    return detail::LayerAnchorAlignment::End;
  }
  return detail::LayerAnchorAlignment::Start;
}

float AnchorAlignmentOrigin(AnchorAlignment alignment) noexcept {
  switch (alignment) {
  case AnchorAlignment::Start:
    return 0.0F;
  case AnchorAlignment::Center:
    return 0.5F;
  case AnchorAlignment::End:
    return 1.0F;
  }
  return 0.0F;
}

Point AnchorMotionDirection(AnchorSide side) noexcept {
  switch (side) {
  case AnchorSide::Below:
    return {0.0F, -1.0F};
  case AnchorSide::Above:
    return {0.0F, 1.0F};
  case AnchorSide::Right:
    return {-1.0F, 0.0F};
  case AnchorSide::Left:
    return {1.0F, 0.0F};
  }
  return {};
}

TransformOrigin AnchorMotionOrigin(AnchorPlacement placement) noexcept {
  const float alignment = AnchorAlignmentOrigin(placement.alignment);
  switch (placement.side) {
  case AnchorSide::Below:
    return {alignment, 0.0F};
  case AnchorSide::Above:
    return {alignment, 1.0F};
  case AnchorSide::Right:
    return {0.0F, alignment};
  case AnchorSide::Left:
    return {1.0F, alignment};
  }
  return {};
}

detail::LayerPlacement
AnchoredPlacement(Rect anchor, AnchorPlacement placement, float gap, float viewport_margin, Point offset) {
  return detail::LayerPlacement{
      .kind = detail::LayerPlacementKind::Anchored,
      .anchor = anchor,
      .preferred_side = ResolveAnchorSide(placement.side),
      .alignment = ResolveAnchorAlignment(placement.alignment),
      .gap = gap,
      .viewport_margin = viewport_margin,
      .offset = offset,
  };
}

LayerOptions BottomSheetLayerOptions(BottomSheetOptions options, Color scrim) {
  return {
      .level = LayerLevel::Presentation,
      .pointer_policy = LayerPointerPolicy::Barrier,
      .trap_focus = true,
      .dismiss_on_outside_press = options.dismiss_on_outside_press,
      .cancel_policy = options.dismiss_on_cancel ? LayerCancelPolicy::Dismiss : LayerCancelPolicy::Consume,
      .on_dismiss_request = std::move(options.on_dismiss_request),
      .barrier_color = scrim,
  };
}

LayerOptions PopupLayerOptions(PopupOptions options) {
  return {
      .level = LayerLevel::Presentation,
      .pointer_policy = options.dismiss_on_outside_press ? LayerPointerPolicy::Barrier : LayerPointerPolicy::Content,
      .trap_focus = options.trap_focus,
      .dismiss_on_outside_press = options.dismiss_on_outside_press,
      .cancel_policy = options.dismiss_on_cancel ? LayerCancelPolicy::Dismiss : LayerCancelPolicy::Consume,
      .on_dismiss_request = std::move(options.on_dismiss_request),
      .barrier_color = std::nullopt,
  };
}

LayerOptions MenuLayerOptions(MenuOptions options, bool submenu) {
  // The root barrier owns outside dismissal; content-only descendants leave their visible ancestors interactive.
  return {
      .level = LayerLevel::Presentation,
      .pointer_policy = submenu ? LayerPointerPolicy::Content : LayerPointerPolicy::Barrier,
      .trap_focus = true,
      .dismiss_on_outside_press = !submenu && options.dismiss_on_outside_press,
      .cancel_policy = options.dismiss_on_cancel ? LayerCancelPolicy::Dismiss : LayerCancelPolicy::Consume,
      .on_dismiss_request = std::move(options.on_dismiss_request),
      .barrier_color = std::nullopt,
  };
}

void ValidateMenuStyle(const MenuStyle& style) {
  if (!std::isfinite(style.corner_radius) || style.corner_radius < 0.0F || !std::isfinite(style.minimum_width) ||
      style.minimum_width < 0.0F || !std::isfinite(style.minimum_item_height) || style.minimum_item_height < 0.0F ||
      !std::isfinite(style.separator_thickness) || style.separator_thickness < 0.0F ||
      !std::isfinite(style.item_content_spacing) || style.item_content_spacing < 0.0F ||
      !std::isfinite(style.icon_size) || style.icon_size < 0.0F || !ValidInsets(style.separator_padding) ||
      !ValidInsets(style.content_padding) || !ValidInsets(style.item_padding) || !ValidShadow(style.shadow) ||
      (style.motion.has_value() && !ValidMotion(*style.motion))) {
    throw std::invalid_argument("HuxerUI menu geometry, shadow, and motion must be finite and non-negative");
  }
}

class DebugOverlayLayout final : public Layout<DebugOverlayLayout> {
public:
  using Layout::Layout;

  static LayoutResult Measure(LayoutContext& context, MountedNode& node, Constraints constraints) {
    const Constraints loose = constraints.Loose();
    for (MountedNode& child : node.Children()) {
      static_cast<void>(context.Measure(child, loose));
    }

    LayoutResult result;
    if (node.ChildCount() > 0) {
      MountedNode& panel = node.ChildAt(0);
      result.Place(
          panel,
          {
              std::min(16.0F, std::max(0.0F, constraints.max_width - panel.LayoutSize().width)),
              std::min(16.0F, std::max(0.0F, constraints.max_height - panel.LayoutSize().height)),
          }
      );
    }
    if (node.ChildCount() > 1) {
      constexpr float corner_inset = 28.0F;
      MountedNode& ribbon = node.ChildAt(1);
      result.Place(
          ribbon,
          {
              constraints.max_width - corner_inset - ribbon.LayoutSize().width * 0.5F,
              corner_inset - ribbon.LayoutSize().height * 0.5F,
          }
      );
    }
    result.SetSize(constraints.Constrain({constraints.max_width, constraints.max_height}));
    return result;
  }
};

class MenuItemLayout final : public Layout<MenuItemLayout> {
public:
  using Layout::Layout;

  static LayoutResult Measure(LayoutContext& context, MountedNode& node, Constraints constraints) {
    if (node.ChildCount() != 2) {
      throw std::logic_error("HuxerUI menu item layout requires content and a trailing item");
    }
    const Constraints loose = constraints.Loose();
    MountedNode& content = node.ChildAt(0);
    MountedNode& trailing = node.ChildAt(1);
    const Size trailing_size = context.Measure(trailing, loose);
    const float spacing = node.Spacing();
    Constraints content_constraints = loose;
    if (content_constraints.HasBoundedWidth()) {
      content_constraints.max_width = std::max(0.0F, content_constraints.max_width - trailing_size.width - spacing);
    }
    const Size content_size = context.Measure(content, content_constraints);
    const Size size = constraints.Constrain({
        content_size.width + spacing + trailing_size.width,
        std::max(content_size.height, trailing_size.height),
    });

    LayoutResult result;
    result.Place(content, {0.0F, (size.height - content_size.height) * 0.5F});
    result.Place(trailing, {size.width - trailing_size.width, (size.height - trailing_size.height) * 0.5F});
    result.SetSize(size);
    return result;
  }
};

constexpr Color debug_ribbon_background = Color::Rgb(183, 28, 28);
constexpr Color debug_ribbon_foreground = Color::White();
constexpr Color debug_ribbon_shadow = Color::Rgb(0, 0, 0, 0.32F);
constexpr Color debug_panel_background = Color::Rgb(17, 22, 31, 0.97F);
constexpr Color debug_panel_foreground = Color::White();
constexpr Color debug_panel_secondary = Color::Rgb(164, 174, 190);
constexpr Color debug_metric_background = Color::Rgb(255, 255, 255, 0.065F);
constexpr Color debug_shadow_color = Color::Rgb(0, 0, 0, 0.42F);
constexpr Color debug_live_color = Color::Rgb(67, 209, 125);

struct DebugSampler {
  std::shared_ptr<detail::DebugMetricsState> metrics;
  State<detail::DebugMetricsSnapshot> snapshot;

  static const detail::ModifierDescriptor& Descriptor();
};

class DebugSamplerExtension final : public NodeExtension {
public:
  DebugSamplerExtension(MountedNode& node, const DebugSampler& modifier) {
    Update(node, modifier);
    if (metrics_) {
      metrics_->ResetSampling();
    }
  }

  void Update(MountedNode& node, const DebugSampler& modifier) {
    static_cast<void>(node);
    metrics_ = modifier.metrics;
    snapshot_ = modifier.snapshot;
  }

  NodeExtension::FrameResult OnFrame(MountedNode& node, const FrameInfo& frame) override {
    static_cast<void>(node);
    constexpr double sample_interval = 1.0;
    if (!next_sample_at_.has_value()) {
      if (metrics_) {
        const detail::DebugMetricsSnapshot sampled = metrics_->Sample(frame.timestamp);
        const bool changed = snapshot_.Get() != sampled;
        if (changed) {
          snapshot_ = sampled;
        }
      }
      next_sample_at_ = frame.timestamp + sample_interval;
      return {
          .needs_frame = false,
          .wake_after = sample_interval,
      };
    }
    const double remaining = *next_sample_at_ - frame.timestamp;
    if (remaining > 0.0) {
      return {
          .needs_frame = false,
          .wake_after = remaining,
      };
    }
    if (metrics_) {
      const detail::DebugMetricsSnapshot sampled = metrics_->Sample(frame.timestamp);
      if (snapshot_.Get() != sampled) {
        snapshot_ = sampled;
      }
    }
    next_sample_at_ = frame.timestamp + sample_interval;
    return {
        .needs_frame = false,
        .wake_after = sample_interval,
    };
  }

private:
  std::shared_ptr<detail::DebugMetricsState> metrics_;
  State<detail::DebugMetricsSnapshot> snapshot_;
  std::optional<double> next_sample_at_;
};

const detail::ModifierDescriptor& DebugSampler::Descriptor() {
  return detail::ModifierDescriptorFor<DebugSampler, DebugSamplerExtension>();
}

std::string FormatOneDecimal(float value) {
  const int tenths = std::max(0, static_cast<int>(std::lround(value * 10.0F)));
  return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10);
}

std::string FormatMemory(std::uint64_t bytes) {
  constexpr double bytes_per_megabyte = 1024.0 * 1024.0;
  return FormatOneDecimal(static_cast<float>(static_cast<double>(bytes) / bytes_per_megabyte)) + " MiB";
}

View DebugMetricCard(std::string label, std::string value, std::string detail, Color accent) {
  Frame frame;
  frame.height = 64.0F;
  return Column {
    Text(std::move(label)).Style(TextStyle{Font::System(10.0F).WithWeight(FontWeight::SemiBold), accent}),
    Text(std::move(value))
        .Style(TextStyle{Font::System(18.0F).WithWeight(FontWeight::SemiBold), debug_panel_foreground}),
    Text(std::move(detail)).Style(TextStyle{Font::System(10.0F), debug_panel_secondary}),
  }.With(frame, Grow{}, Spacing{1.0F}, Padding{8.0F}, Background{debug_metric_background}, CornerRadius{8.0F});
}

View DebugPanel(
    const detail::DebugMetricsSnapshot& snapshot,
    const std::shared_ptr<detail::DebugMetricsState>& metrics,
    State<detail::DebugMetricsSnapshot> snapshot_state
) {
  const std::string fps =
      snapshot.painted_frame_count == 0 ? "Idle" : std::to_string(static_cast<int>(std::lround(snapshot.fps)));
  const std::string commit_time =
      snapshot.painted_frame_count == 0 ? "--" : FormatOneDecimal(snapshot.average_commit_time_ms) + " ms";
  const std::string maximum_commit_time = snapshot.painted_frame_count == 0
                                              ? "No painted frames"
                                              : "Max " + FormatOneDecimal(snapshot.maximum_commit_time_ms) + " ms";
  const std::string cpu = snapshot.cpu_percent.has_value() ? FormatOneDecimal(*snapshot.cpu_percent) + "%" : "--";
  const std::string memory =
      snapshot.memory_usage_bytes.has_value() ? FormatMemory(*snapshot.memory_usage_bytes) : "--";
  const std::string footer = "Damage " + FormatOneDecimal(snapshot.average_damage_ratio * 100.0F) + "%  /  " +
                             std::to_string(static_cast<int>(std::lround(snapshot.viewport.width))) + " x " +
                             std::to_string(static_cast<int>(std::lround(snapshot.viewport.height)));

  Frame panel_frame;
  panel_frame.width = 288.0F;
  Frame live_indicator_frame;
  live_indicator_frame.width = 8.0F;
  live_indicator_frame.height = 8.0F;
  return Column {
    Row {
      Column {}.With(live_indicator_frame, Background{debug_live_color}, CornerRadius{4.0F}),
      Text("HuxerUI Performance")
          .Style(TextStyle{Font::System(14.0F).WithWeight(FontWeight::SemiBold), debug_panel_foreground}),
      Spacer().With(Grow{}),
      Text("LIVE").Style(TextStyle{Font::System(9.0F).WithWeight(FontWeight::SemiBold), debug_live_color}),
    }.With(Spacing{7.0F}, CrossAlign{CrossAxisAlignment::Center}),
    Row {
      DebugMetricCard("FPS", fps, "Painted frames/s", debug_live_color),
      DebugMetricCard("COMMIT", commit_time, maximum_commit_time, Color::Rgb(92, 158, 255)),
    }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch}),
    Row {
      DebugMetricCard("CPU", cpu, "Process / all cores", Color::Rgb(255, 183, 77)),
      DebugMetricCard("MEMORY", memory, "Process footprint", Color::Rgb(186, 132, 255)),
    }.With(Spacing{8.0F}, CrossAlign{CrossAxisAlignment::Stretch}),
    Text(footer).Style(TextStyle{Font::System(10.0F), debug_panel_secondary}),
  }.With(
      panel_frame,
      Spacing{8.0F},
      Padding{12.0F},
      Background{debug_panel_background},
      Shadow{
          .color = debug_shadow_color,
          .offset = {},
          .blur_radius = 20.0F,
          .spread = -2.0F,
      },
      CornerRadius{12.0F},
      DebugSampler{metrics, snapshot_state}
  );
}

View DebugRibbon(State<bool> expanded, State<detail::DebugMetricsSnapshot> snapshot) {
  Frame ribbon_frame;
  ribbon_frame.width = 96.0F;
  ribbon_frame.height = 18.0F;
  return Row {
    Text("DEBUG").Style(TextStyle{Font::System(12.0F).WithWeight(FontWeight::Bold), debug_ribbon_foreground}),
  }.With(
      ribbon_frame,
      MainAlign{MainAxisAlignment::Center},
      CrossAlign{CrossAxisAlignment::Center},
      Background{debug_ribbon_background},
      Shadow{
          .color = debug_ribbon_shadow,
          .offset = {},
          .blur_radius = 8.0F,
      },
      Rotation{45.0F}
  ).OnClick([expanded, snapshot] {
    const bool next_expanded = !expanded.Get();
    if (next_expanded) {
      snapshot = {};
    }
    expanded = next_expanded;
  });
}

} // namespace

namespace detail {

struct LayerAnchorState : std::enable_shared_from_this<LayerAnchorState> {
  explicit LayerAnchorState(LayerController controller) : layers(std::move(controller)) {}

  void Mount() {
    if (mounted) {
      throw std::logic_error("HuxerUI presentation anchor must be mounted on only one View");
    }
    mounted = true;
  }

  void Unmount() {
    mounted = false;
    bounds.reset();
    const std::optional<LayerId> anchored_layer = follows_anchor ? active_layer : std::nullopt;
    follows_anchor = false;
    if (anchored_layer.has_value()) {
      Dismiss(*anchored_layer);
    }
  }

  void UpdateBounds(Rect next_bounds) {
    if (bounds == next_bounds) {
      return;
    }
    bounds = next_bounds;
    if (active_layer.has_value() && follows_anchor) {
      active_placement.anchor = next_bounds;
      layers.UpdatePlacement(*active_layer, active_placement);
    }
  }

  [[nodiscard]] Rect RequireBounds() const {
    if (!mounted || !bounds.has_value()) {
      throw std::logic_error("HuxerUI anchored presentation requires a mounted anchor View");
    }
    return *bounds;
  }

  void Bind(LayerId id, LayerPlacement placement, bool should_follow_anchor) {
    if (active_layer.has_value() && *active_layer != id) {
      Dismiss(*active_layer);
    }
    active_layer = id;
    active_placement = std::move(placement);
    follows_anchor = should_follow_anchor;
  }

  bool Dismiss(LayerId id) {
    if (active_layer == id && dismiss_handler) {
      auto handler = std::move(dismiss_handler);
      return handler(id);
    }
    return DismissDirect(id);
  }

  bool DismissDirect(LayerId id) {
    if (active_layer == id) {
      active_layer.reset();
      follows_anchor = false;
      dismiss_handler = {};
    }
    return layers.Dismiss(id);
  }

  void SetDismissHandler(LayerId id, std::function<bool(LayerId)> handler) {
    if (active_layer != id) {
      throw std::logic_error("HuxerUI presentation dismissal handler requires the active layer");
    }
    dismiss_handler = std::move(handler);
  }

  LayerId AttachLayer(
      std::optional<Point> point,
      ViewFactory content,
      AnchorPlacement preferred_placement,
      float gap,
      float viewport_margin,
      Point offset,
      LayerOptions options,
      std::shared_ptr<const Environment> environment,
      std::shared_ptr<LayerTransitionState> transition = {}
  ) {
    ValidateAnchoredOptions(gap, viewport_margin, offset, point);
    const Rect anchor_bounds = point.has_value() ? Rect{point->x, point->y, 0.0F, 0.0F} : RequireBounds();
    LayerPlacement placement = AnchoredPlacement(anchor_bounds, preferred_placement, gap, viewport_margin, offset);
    auto id = std::make_shared<LayerId>(0);
    if (!options.on_dismiss_request) {
      options.on_dismiss_request = [anchor = shared_from_this(), id] { anchor->Dismiss(*id); };
    }
    const LayerId attached = layers.AttachCaptured(
        std::move(options),
        std::move(content),
        std::move(environment),
        placement,
        std::move(transition)
    );
    *id = attached;
    Bind(attached, std::move(placement), !point.has_value());
    return attached;
  }

  LayerController layers;
  std::optional<Rect> bounds;
  std::optional<LayerId> active_layer;
  LayerPlacement active_placement;
  std::function<bool(LayerId)> dismiss_handler;
  bool mounted = false;
  bool follows_anchor = false;
};

struct MenuChainState : std::enable_shared_from_this<MenuChainState> {
  struct Level {
    std::weak_ptr<LayerAnchorState> anchor;
    LayerId id = 0;
  };

  bool DismissFrom(std::size_t depth) {
    // Remove descendants first so focus and anchor ownership unwind in visual stacking order.
    bool dismissed = false;
    while (levels.size() > depth) {
      Level level = std::move(levels.back());
      levels.pop_back();
      if (const auto anchor = level.anchor.lock()) {
        dismissed = anchor->DismissDirect(level.id) || dismissed;
      }
    }
    return dismissed;
  }

  void Register(std::size_t depth, const std::shared_ptr<LayerAnchorState>& anchor, LayerId id) {
    if (depth != levels.size()) {
      throw std::logic_error("HuxerUI menu chain levels must be registered in order");
    }
    levels.push_back(Level{anchor, id});
    anchor->SetDismissHandler(id, [chain = weak_from_this(), depth](LayerId) {
      const auto locked = chain.lock();
      return locked && locked->DismissFrom(depth);
    });
  }

  std::vector<Level> levels;
};

class BottomSheetService {
public:
  explicit BottomSheetService(LayerController& layers) : layers_(layers) {}

  LayerId Show(ViewFactory content, BottomSheetOptions options, std::shared_ptr<const Environment> environment);
  LayerId Show(BottomSheetFactory content, BottomSheetOptions options, std::shared_ptr<const Environment> environment);
  bool Dismiss(LayerId id);

private:
  LayerController layers_;
};

class PopupService {
public:
  explicit PopupService(LayerController& layers) : layers_(layers) {}

  [[nodiscard]] std::shared_ptr<LayerAnchorState> CreateAnchor() const {
    return std::make_shared<LayerAnchorState>(layers_);
  }

  LayerId Show(
      const std::shared_ptr<LayerAnchorState>& anchor,
      std::optional<Point> point,
      ViewFactory content,
      PopupOptions options,
      std::shared_ptr<const Environment> environment
  );
  LayerId Show(
      const std::shared_ptr<LayerAnchorState>& anchor,
      std::optional<Point> point,
      PopupFactory content,
      PopupOptions options,
      std::shared_ptr<const Environment> environment
  );

private:
  LayerController layers_;
};

class MenuService {
public:
  explicit MenuService(LayerController& layers) : layers_(layers) {}

  [[nodiscard]] std::shared_ptr<LayerAnchorState> CreateAnchor() const {
    return std::make_shared<LayerAnchorState>(layers_);
  }

  LayerId Show(
      const std::shared_ptr<LayerAnchorState>& anchor,
      std::optional<Point> point,
      std::vector<MenuEntry> entries,
      MenuOptions options,
      std::shared_ptr<const Environment> environment
  );

private:
  LayerId ShowLevel(
      const std::shared_ptr<LayerAnchorState>& anchor,
      std::optional<Point> point,
      std::vector<MenuEntry> entries,
      MenuOptions options,
      std::shared_ptr<const Environment> environment,
      const std::shared_ptr<MenuChainState>& chain,
      std::size_t depth,
      bool submenu
  );
  static void ValidateEntries(const std::vector<MenuEntry>& entries);
  static View ItemView(
      MenuItem item, const MenuStyle& style, const std::shared_ptr<MenuChainState>& chain, std::size_t depth
  );
  static View SeparatorView(const MenuStyle& style);
  static View Surface(
      std::vector<MenuEntry> entries,
      MenuStyle style,
      std::optional<float> width,
      const std::shared_ptr<MenuChainState>& chain,
      std::size_t depth
  );

  LayerController layers_;
};

void MenuService::ValidateEntries(const std::vector<MenuEntry>& entries) {
  if (entries.empty()) {
    throw std::invalid_argument("HuxerUI menu must contain at least one item");
  }

  bool previous_was_section = true;
  for (const MenuEntry& entry : entries) {
    if (std::holds_alternative<MenuSection>(entry.value_)) {
      if (previous_was_section) {
        throw std::invalid_argument("HuxerUI menu section must separate two items");
      }
      previous_was_section = true;
      continue;
    }

    const MenuItem& item = std::get<MenuItem>(entry.value_);
    if (detail::IsEmptyStringVariantLiteral(item.label_)) {
      throw std::invalid_argument("HuxerUI menu item label must not be empty");
    }
    if (const auto* icon = std::get_if<ImageAsset>(&item.icon_); icon && !icon->HasValue()) {
      throw std::invalid_argument("HuxerUI menu item image asset must not be empty");
    }
    if (const auto* action = std::get_if<std::function<void()>>(&item.destination_)) {
      if (!*action) {
        throw std::invalid_argument("HuxerUI menu action item must provide an action");
      }
    } else {
      ValidateEntries(std::get<std::vector<MenuEntry>>(item.destination_));
    }
    previous_was_section = false;
  }

  if (previous_was_section) {
    throw std::invalid_argument("HuxerUI menu section must separate two items");
  }
}

View MenuService::ItemView(
    MenuItem item, const MenuStyle& style, const std::shared_ptr<MenuChainState>& chain, std::size_t depth
) {
  Frame item_frame;
  item_frame.min_height = style.minimum_item_height;
  Frame icon_frame;
  icon_frame.width = style.icon_size;
  icon_frame.height = style.icon_size;

  std::vector<View> content;
  if (item.checked_) {
    content.push_back(Text("\xE2\x9C\x93").With(icon_frame, Foreground{style.foreground}));
  }
  if (const auto* resource = std::get_if<ImageResource>(&item.icon_)) {
    content.push_back(Image(*resource).Fit(ImageFit::Contain).With(icon_frame));
  } else if (const auto* asset = std::get_if<ImageAsset>(&item.icon_)) {
    content.push_back(Image(*asset).Fit(ImageFit::Contain).With(icon_frame));
  }

  std::string label = detail::ResolveStringVariant(std::move(item.label_));
  if (label.empty()) {
    throw std::invalid_argument("HuxerUI menu item label must not be empty");
  }
  content.push_back(Text(std::move(label)).With(Foreground{style.foreground}));

  if (std::holds_alternative<std::vector<MenuEntry>>(item.destination_)) {
    auto submenu = UseMenu();
    std::vector<MenuEntry> entries = std::get<std::vector<MenuEntry>>(std::move(item.destination_));
    Frame arrow_frame;
    arrow_frame.width = style.icon_size;
    return MenuItemLayout{
        Row{std::move(content)}.With(Spacing{style.item_content_spacing}, CrossAlign{CrossAxisAlignment::Center}),
        Text("\xE2\x80\xBA").With(arrow_frame, Foreground{style.foreground}),
    }
        .With(
            submenu.Anchor(),
            item_frame,
            Padding{style.item_padding},
            Spacing{style.item_content_spacing},
            Enabled{item.enabled_},
            Indication{style.item_indication},
            Focusable{}
        )
        .OnClick([submenu, entries = std::move(entries), chain, depth] {
          MenuOptions options;
          options.placement = {
              .side = AnchorSide::Right,
              .alignment = AnchorAlignment::Start,
          };
          options.gap = 2.0F;
          submenu.service_->ShowLevel(
              submenu.anchor_,
              std::nullopt,
              entries,
              std::move(options),
              submenu.environment_,
              chain,
              depth + 1,
              true
          );
        });
  }

  std::function<void()> action = std::get<std::function<void()>>(std::move(item.destination_));
  return Row {std::move(content)}
      .With(
          item_frame,
          Padding{style.item_padding},
          Spacing{style.item_content_spacing},
          CrossAlign{CrossAxisAlignment::Center},
          Enabled{item.enabled_},
          Indication{style.item_indication},
          Focusable{}
      )
      .OnClick([chain, action = std::move(action)] {
        chain->DismissFrom(0);
        action();
      });
}

View MenuService::SeparatorView(const MenuStyle& style) {
  Frame line_frame;
  line_frame.height = style.separator_thickness;
  return Column {
    Row {}.With(line_frame, Background{style.separator_color}),
  }.With(Padding{style.separator_padding}, CrossAlign{CrossAxisAlignment::Stretch});
}

View MenuService::Surface(
    std::vector<MenuEntry> entries,
    MenuStyle style,
    std::optional<float> width,
    const std::shared_ptr<MenuChainState>& chain,
    std::size_t depth
) {
  std::vector<View> children;
  bool has_item = false;
  bool pending_section = false;
  for (MenuEntry& entry : entries) {
    if (std::holds_alternative<MenuSection>(entry.value_)) {
      pending_section = true;
      continue;
    }
    if (has_item && (style.separator_mode == MenuSeparatorMode::BetweenItems ||
                     (style.separator_mode == MenuSeparatorMode::BetweenSections && pending_section))) {
      children.push_back(SeparatorView(style));
    }
    has_item = true;
    pending_section = false;
    children.push_back(ItemView(std::get<MenuItem>(std::move(entry.value_)), style, chain, depth));
  }

  Frame surface_frame;
  if (width.has_value()) {
    surface_frame.width = *width;
  } else {
    surface_frame.min_width = style.minimum_width;
  }
  return Column {std::move(children)}.With(
      surface_frame,
      Padding{style.content_padding},
      CrossAlign{CrossAxisAlignment::Stretch},
      Background{style.background},
      CornerRadius{style.corner_radius},
      style.shadow
  );
}

View DialogService::StandardContent(
    const StringVariant& title,
    const StringVariant& message,
    const StringVariant& positive,
    const std::optional<StringVariant>& negative,
    const std::function<void()>& on_positive_click,
    const std::function<void()>& on_negative_click,
    const DialogStyle& style,
    const LayerController& layers,
    LayerId id
) {
  std::string resolved_title = detail::ResolveStringVariant(title);
  std::string resolved_message = detail::ResolveStringVariant(message);
  std::string resolved_positive = detail::ResolveStringVariant(positive);
  if (resolved_positive.empty()) {
    resolved_positive = "OK";
  }
  std::optional<std::string> resolved_negative;
  if (negative.has_value()) {
    resolved_negative = detail::ResolveStringVariant(*negative);
    if (resolved_negative->empty()) {
      *resolved_negative = "Cancel";
    }
  }
  if (resolved_title.empty()) {
    throw std::invalid_argument("HuxerUI dialog title must not be empty");
  }
  if (resolved_message.empty()) {
    throw std::invalid_argument("HuxerUI dialog message must not be empty");
  }
  std::vector<View> action_views;
  action_views.reserve(resolved_negative.has_value() ? 3 : 1);
  const auto append_action = [&](std::string label, bool is_positive, std::function<void()> on_click) {
    if (!action_views.empty() && style.action_separator_thickness > 0.0F) {
      Frame separator_frame;
      if (style.action_layout == Axis::Horizontal) {
        separator_frame.width = style.action_separator_thickness;
        separator_frame.height = style.minimum_action_height;
      } else {
        separator_frame.height = style.action_separator_thickness;
      }
      action_views.push_back(Row {}.With(separator_frame, Background{style.action_separator_color}));
    }

    const TextStyle& text_style = is_positive ? style.positive_action_style : style.negative_action_style;
    const Color background = is_positive ? style.positive_action_background : style.negative_action_background;
    const IndicationSpec& indication =
        is_positive ? style.positive_action_indication : style.negative_action_indication;

    Frame action_frame;
    action_frame.min_height = style.minimum_action_height;
    View action_view = Text(std::move(label))
                           .Style(text_style)
                           .With(
                               action_frame,
                               Padding{style.action_padding},
                               Background{background},
                               CornerRadius{style.action_corner_radius},
                               Indication{indication},
                               Focusable{}
                           )
                           .OnClick([layers, id, on_click = std::move(on_click)] {
                             layers.Dismiss(id);
                             if (on_click) {
                               on_click();
                             }
                           });
    if (style.action_layout == Axis::Horizontal && style.action_alignment == HorizontalAlignment::Stretch) {
      action_view = std::move(action_view).With(Grow{});
    }
    action_views.push_back(std::move(action_view));
  };
  if (resolved_negative.has_value()) {
    append_action(std::move(*resolved_negative), false, std::move(on_negative_click));
  }
  append_action(std::move(resolved_positive), true, std::move(on_positive_click));

  View actions;
  if (style.action_layout == Axis::Horizontal) {
    View action_row =
        Row {std::move(action_views)}.With(Spacing{style.action_spacing}, CrossAlign{CrossAxisAlignment::Center});
    actions = Stack {
      std::move(action_row),
    }.With(Align{style.action_alignment, VerticalAlignment::Center});
  } else {
    actions = Column {std::move(action_views)}.With(
        Spacing{style.action_spacing},
        CrossAlign{ResolveCrossAlignment(style.action_alignment)}
    );
  }

  View body = Column {
    Text(std::move(resolved_title)).Style(style.title_style),
    Text(std::move(resolved_message)).Style(style.message_style),
  }.With(Spacing{style.content_spacing}, CrossAlign{ResolveCrossAlignment(style.content_alignment)});

  Frame surface_frame;
  surface_frame.max_width = style.maximum_width;
  return Column {
    std::move(body),
    std::move(actions),
  }.With(
      surface_frame,
      Padding{style.content_padding},
      Spacing{style.content_spacing},
      CrossAlign{CrossAxisAlignment::Stretch},
      Background{style.background},
      CornerRadius{style.corner_radius},
      style.shadow
  );
}

class LayerAnchorExtension final : public NodeExtension {
public:
  LayerAnchorExtension(huxerui::MountedNode& node, const LayerAnchor& modifier) {
    Update(node, modifier);
  }

  ~LayerAnchorExtension() override {
    if (state_) {
      try {
        state_->Unmount();
      } catch (...) {
      }
    }
  }

  void Update(huxerui::MountedNode& node, const LayerAnchor& modifier) {
    static_cast<void>(node);
    if (state_ == modifier.state_) {
      return;
    }
    if (state_) {
      state_->Unmount();
    }
    state_ = modifier.state_;
    if (state_) {
      state_->Mount();
    }
  }

  [[nodiscard]] bool PrepareGeometry(huxerui::MountedNode& node) override {
    if (state_) {
      state_->UpdateBounds(node.PresentationBounds());
    }
    return false;
  }

private:
  std::shared_ptr<LayerAnchorState> state_;
};

class DebugOverlayInstaller {
public:
  static void Install(RootContext& root, std::shared_ptr<DebugMetricsState> metrics) {
    LayerPlacement placement;
    placement.kind = LayerPlacementKind::Fill;
    root.Layers().AttachCaptured(
        LayerOptions{
            .level = LayerLevel::System,
            .pointer_policy = LayerPointerPolicy::Content,
            .trap_focus = false,
            .dismiss_on_outside_press = false,
            .cancel_policy = LayerCancelPolicy::PassThrough,
            .on_dismiss_request = {},
            .barrier_color = std::nullopt,
        },
        [metrics = std::move(metrics)] {
          auto expanded = UseState(false);
          auto snapshot = UseState(DebugMetricsSnapshot{});
          std::vector<View> children;
          if (expanded.Get()) {
            children.push_back(DebugPanel(snapshot.Get(), metrics, snapshot));
          } else {
            children.push_back(Spacer());
          }
          children.push_back(DebugRibbon(expanded, snapshot));
          return DebugOverlayLayout{std::move(children)};
        },
        {},
        std::move(placement)
    );
  }
};

class DialogExtension final : public NodeExtension {
public:
  DialogExtension(huxerui::MountedNode& node, const Dialog& modifier) {
    Update(node, modifier);
  }

  ~DialogExtension() override {
    if (service_ && layer_.has_value()) {
      try {
        service_->Dismiss(*layer_);
      } catch (...) {
      }
    }
  }

  void Update(huxerui::MountedNode& node, const Dialog& modifier) {
    auto& mounted = static_cast<detail::MountedNode&>(node);
    if (!service_) {
      service_ = DialogServiceFor(mounted);
    }

    if (!modifier.visible) {
      if (layer_.has_value()) {
        service_->Dismiss(*layer_);
      }
      return;
    }
    if (!modifier.content) {
      throw std::invalid_argument("HuxerUI visible Dialog modifier content must not be empty");
    }
    if ((modifier.dismiss_on_outside_press || modifier.dismiss_on_cancel) && !modifier.on_dismiss_request) {
      throw std::invalid_argument(
          "HuxerUI dismissible Dialog modifier requires "
          "on_dismiss_request"
      );
    }
    DialogOptions options{
        .dismiss_on_outside_press = modifier.dismiss_on_outside_press,
        .dismiss_on_cancel = modifier.dismiss_on_cancel,
        .on_dismiss_request = modifier.on_dismiss_request,
    };
    if (layer_.has_value()) {
      if (service_->Update(*layer_, modifier.content, options, mounted.environment)) {
        return;
      }
      layer_.reset();
    }
    layer_ = service_->Show(modifier.content, std::move(options), mounted.environment);
  }

private:
  std::shared_ptr<DialogService> service_;
  std::optional<LayerId> layer_;
};

void InstallBuiltinPresentation(RootContext& root) {
  root.Provide(std::make_shared<ToastService>(root.Layers()));
  root.Provide(std::make_shared<DialogService>(root.Layers()));
  root.Provide(std::make_shared<BottomSheetService>(root.Layers()));
  root.Provide(std::make_shared<PopupService>(root.Layers()));
  root.Provide(std::make_shared<MenuService>(root.Layers()));
}

void InstallDebugOverlay(RootContext& root, std::shared_ptr<DebugMetricsState> metrics) {
  DebugOverlayInstaller::Install(root, std::move(metrics));
}

} // namespace detail

MenuItem::MenuItem(StringVariant label, std::function<void()> on_item_click)
    : MenuItem(std::move(label), Icon{std::monostate{}}, std::move(on_item_click)) {}

MenuItem::MenuItem(ImageResource icon, StringVariant label, std::function<void()> on_item_click)
    : MenuItem(std::move(label), Icon{std::move(icon)}, std::move(on_item_click)) {}

MenuItem::MenuItem(ImageAsset icon, StringVariant label, std::function<void()> on_item_click)
    : MenuItem(std::move(label), Icon{std::move(icon)}, std::move(on_item_click)) {}

MenuItem::MenuItem(StringVariant label, std::vector<MenuEntry> children)
    : MenuItem(std::move(label), Icon{std::monostate{}}, std::move(children)) {}

MenuItem::MenuItem(ImageResource icon, StringVariant label, std::vector<MenuEntry> children)
    : MenuItem(std::move(label), Icon{std::move(icon)}, std::move(children)) {}

MenuItem::MenuItem(ImageAsset icon, StringVariant label, std::vector<MenuEntry> children)
    : MenuItem(std::move(label), Icon{std::move(icon)}, std::move(children)) {}

MenuItem::MenuItem(StringVariant label, Icon icon, std::function<void()> on_item_click)
    : label_(std::move(label)), icon_(std::move(icon)), destination_(std::move(on_item_click)) {}

MenuItem::MenuItem(StringVariant label, Icon icon, std::vector<MenuEntry> children)
    : label_(std::move(label)), icon_(std::move(icon)), destination_(std::move(children)) {}

MenuItem::MenuItem(const MenuItem& other) = default;

MenuItem::MenuItem(MenuItem&& other) noexcept = default;

MenuItem& MenuItem::operator=(const MenuItem& other) = default;

MenuItem& MenuItem::operator=(MenuItem&& other) noexcept = default;

MenuItem::~MenuItem() = default;

MenuItem MenuItem::Enabled(bool enabled) && {
  enabled_ = enabled;
  return std::move(*this);
}

MenuItem MenuItem::Checked(bool checked) && {
  checked_ = checked;
  return std::move(*this);
}

LayerId ToastHandle::Show(StringVariant message, ToastOptions options) const {
  return service_->Show(std::move(message), options, environment_);
}

bool ToastHandle::Dismiss(LayerId id) const {
  return service_->Dismiss(id);
}

LayerId detail::ToastService::Show(
    StringVariant message, ToastOptions options, std::shared_ptr<const Environment> environment
) {
  if (!std::isfinite(options.duration) || options.duration < 0.0) {
    throw std::invalid_argument("HuxerUI toast duration must be finite and non-negative");
  }
  if (detail::IsEmptyStringVariantLiteral(message)) {
    throw std::invalid_argument("HuxerUI toast message must not be empty");
  }
  const ThemeSpec theme = detail::ResolveThemeSpec(environment);
  const ToastStyle style = ResolveToastStyle(environment);
  ValidateToastStyle(style);
  const std::shared_ptr<detail::LayerTransitionState> transition =
      PresentationTransition(style.motion, theme.motion.reduced_motion);
  auto id = std::make_shared<LayerId>(0);
  std::weak_ptr<ToastService> service = weak_from_this();
  detail::LayerPlacement placement;
  placement.kind = VerticalLayerPlacementKind(style.placement);
  const LayerId attached = layers_.AttachCaptured(
      LayerOptions{
          .level = LayerLevel::Notification,
          .pointer_policy = LayerPointerPolicy::PassThrough,
          .trap_focus = false,
          .dismiss_on_outside_press = false,
          .cancel_policy = LayerCancelPolicy::PassThrough,
          .on_dismiss_request = {},
          .barrier_color = std::nullopt,
      },
      [service, id, message = std::move(message), options, style, transition]() -> View {
        std::string resolved_message = detail::ResolveStringVariant(message);
        if (resolved_message.empty()) {
          throw std::invalid_argument("HuxerUI toast message must not be empty");
        }
        Frame surface_frame;
        surface_frame.max_width = style.maximum_width;
        View result = Stack {
          Text(std::move(resolved_message))
              .Style(style.text_style)
              .With(
                  surface_frame,
                  Padding{style.padding},
                  Background{style.background},
                  CornerRadius{style.corner_radius},
                  style.shadow,
                  ToastLifetime{
                      service,
                      *id,
                      options.duration,
                  }
              ),
        }.With(Padding{style.viewport_padding});
        if (!transition || !style.motion.has_value()) {
          return result;
        }
        return std::move(result).With(
            PresentationContentMotion{
                .state = transition,
                .motion = *style.motion,
                .slide_direction = VerticalSlideDirection(style.placement),
                .origin = TransformOrigin{0.5F, 0.5F},
            }
        );
      },
      std::move(environment),
      std::move(placement),
      transition
  );
  *id = attached;
  return attached;
}

bool detail::ToastService::Dismiss(LayerId id) {
  return layers_.Dismiss(id);
}

ToastHandle UseToast() {
  return ToastHandle{
      UseService<detail::ToastService>(),
      detail::CurrentEnvironment(),
  };
}

LayerId DialogHandle::Show(ViewFactory content, DialogOptions options) const {
  return service_->Show(std::move(content), std::move(options), environment_);
}

LayerId DialogHandle::Show(DialogFactory content, DialogOptions options) const {
  return service_->Show(std::move(content), std::move(options), environment_);
}

LayerId DialogHandle::Show(
    StringVariant title,
    StringVariant message,
    StringVariant positive,
    std::function<void()> on_positive_click,
    DialogOptions options
) const {
  return service_->Show(
      std::move(title),
      std::move(message),
      std::move(positive),
      std::nullopt,
      std::move(on_positive_click),
      {},
      std::move(options),
      environment_
  );
}

LayerId DialogHandle::Show(
    StringVariant title,
    StringVariant message,
    StringVariant positive,
    StringVariant negative,
    std::function<void()> on_positive_click,
    std::function<void()> on_negative_click,
    DialogOptions options
) const {
  return service_->Show(
      std::move(title),
      std::move(message),
      std::move(positive),
      std::move(negative),
      std::move(on_positive_click),
      std::move(on_negative_click),
      std::move(options),
      environment_
  );
}

bool DialogHandle::Update(LayerId id, ViewFactory content) const {
  return service_->Update(id, std::move(content), environment_);
}

bool DialogHandle::Update(LayerId id, DialogFactory content) const {
  return service_->Update(id, std::move(content), environment_);
}

bool DialogHandle::Dismiss(LayerId id) const {
  return service_->Dismiss(id);
}

std::shared_ptr<detail::LayerTransitionState> detail::DialogService::ReconcileTransition(
    LayerId id, const std::optional<PresentationMotion>& motion, bool reduced_motion
) {
  if (!motion.has_value()) {
    static_cast<void>(layers_.UpdateTransition(id, {}));
    return {};
  }
  std::shared_ptr<detail::LayerTransitionState> transition = layers_.Transition(id);
  if (transition) {
    UpdatePresentationTransition(transition, *motion, reduced_motion);
    return transition;
  }
  transition = PresentationTransition(motion, reduced_motion, false);
  static_cast<void>(layers_.UpdateTransition(id, transition));
  return transition;
}

LayerId detail::DialogService::Show(
    StringVariant title,
    StringVariant message,
    StringVariant positive,
    std::optional<StringVariant> negative,
    std::function<void()> on_positive_click,
    std::function<void()> on_negative_click,
    DialogOptions options,
    std::shared_ptr<const Environment> environment
) {
  if (detail::IsEmptyStringVariantLiteral(title)) {
    throw std::invalid_argument("HuxerUI dialog title must not be empty");
  }
  if (detail::IsEmptyStringVariantLiteral(message)) {
    throw std::invalid_argument("HuxerUI dialog message must not be empty");
  }
  const ThemeSpec theme = detail::ResolveThemeSpec(environment);
  const DialogStyle style = ResolveDialogStyle(environment);
  ValidateDialogStyle(style);
  const std::shared_ptr<detail::LayerTransitionState> transition =
      PresentationTransition(style.motion, theme.motion.reduced_motion);
  auto id = std::make_shared<LayerId>(0);
  LayerOptions layer_options = DialogLayerOptions(std::move(options), style.scrim);
  const LayerId attached = layers_.AttachCaptured(
      std::move(layer_options),
      AnimatedDialogContent(
          [title = std::move(title),
           message = std::move(message),
           positive = std::move(positive),
           negative = std::move(negative),
           on_positive_click = std::move(on_positive_click),
           on_negative_click = std::move(on_negative_click),
           style,
           layers = layers_,
           id] {
            return StandardContent(
                title,
                message,
                positive,
                negative,
                on_positive_click,
                on_negative_click,
                style,
                layers,
                *id
            );
          },
          style,
          transition
      ),
      std::move(environment),
      DialogLayerPlacement(style),
      transition
  );
  *id = attached;
  return attached;
}

LayerId detail::DialogService::Show(
    ViewFactory content, DialogOptions options, std::shared_ptr<const Environment> environment
) {
  if (!content) {
    throw std::invalid_argument("HuxerUI dialog content factory must not be empty");
  }
  const ThemeSpec theme = detail::ResolveThemeSpec(environment);
  const DialogStyle style = ResolveDialogStyle(environment);
  ValidateDialogStyle(style);
  const std::shared_ptr<detail::LayerTransitionState> transition =
      PresentationTransition(style.motion, theme.motion.reduced_motion);
  LayerOptions layer_options = DialogLayerOptions(std::move(options), style.scrim);
  return layers_.AttachCaptured(
      std::move(layer_options),
      AnimatedDialogContent(std::move(content), style, transition),
      std::move(environment),
      DialogLayerPlacement(style),
      transition
  );
}

LayerId detail::DialogService::Show(
    DialogFactory content, DialogOptions options, std::shared_ptr<const Environment> environment
) {
  if (!content) {
    throw std::invalid_argument("HuxerUI dialog content factory must not be empty");
  }
  auto id = std::make_shared<LayerId>(0);
  const LayerId attached = Show(
      [layers = layers_, id, content = std::move(content)] { return content(DialogContext{layers, *id}); },
      std::move(options),
      std::move(environment)
  );
  *id = attached;
  return attached;
}

bool detail::DialogService::Update(LayerId id, ViewFactory content, std::shared_ptr<const Environment> environment) {
  if (!content) {
    throw std::invalid_argument("HuxerUI dialog content factory must not be empty");
  }
  const ThemeSpec theme = detail::ResolveThemeSpec(environment);
  const DialogStyle style = ResolveDialogStyle(environment);
  ValidateDialogStyle(style);
  std::optional<LayerOptions> layer_options = layers_.EntryOptions(id);
  if (!layer_options.has_value()) {
    return false;
  }
  layer_options->barrier_color = style.scrim;
  if (!layers_.UpdatePlacement(id, DialogLayerPlacement(style))) {
    return false;
  }
  const std::shared_ptr<detail::LayerTransitionState> transition =
      ReconcileTransition(id, style.motion, theme.motion.reduced_motion);
  return layers_.UpdateCaptured(
      id,
      std::move(*layer_options),
      AnimatedDialogContent(std::move(content), style, transition),
      std::move(environment)
  );
}

bool detail::DialogService::Update(
    LayerId id, ViewFactory content, const DialogOptions& options, std::shared_ptr<const Environment> environment
) {
  const ThemeSpec theme = detail::ResolveThemeSpec(environment);
  const DialogStyle style = ResolveDialogStyle(environment);
  ValidateDialogStyle(style);
  LayerOptions layer_options = DialogLayerOptions(options, style.scrim);
  if (!layers_.UpdatePlacement(id, DialogLayerPlacement(style))) {
    return false;
  }
  const std::shared_ptr<detail::LayerTransitionState> transition =
      ReconcileTransition(id, style.motion, theme.motion.reduced_motion);
  return layers_.UpdateCaptured(
      id,
      std::move(layer_options),
      AnimatedDialogContent(std::move(content), style, transition),
      std::move(environment)
  );
}

bool detail::DialogService::Update(LayerId id, DialogFactory content, std::shared_ptr<const Environment> environment) {
  if (!content) {
    throw std::invalid_argument("HuxerUI dialog content factory must not be empty");
  }
  return Update(
      id,
      [layers = layers_, id, content = std::move(content)] { return content(DialogContext{layers, id}); },
      std::move(environment)
  );
}

bool detail::DialogService::Dismiss(LayerId id) {
  return layers_.Dismiss(id);
}

DialogHandle UseDialog() {
  return DialogHandle{
      UseService<detail::DialogService>(),
      detail::CurrentEnvironment(),
  };
}

LayerId BottomSheetHandle::Show(ViewFactory content, BottomSheetOptions options) const {
  return service_->Show(std::move(content), std::move(options), environment_);
}

LayerId BottomSheetHandle::Show(BottomSheetFactory content, BottomSheetOptions options) const {
  return service_->Show(std::move(content), std::move(options), environment_);
}

bool BottomSheetHandle::Dismiss(LayerId id) const {
  return service_->Dismiss(id);
}

LayerId detail::BottomSheetService::Show(
    ViewFactory content, BottomSheetOptions options, std::shared_ptr<const Environment> environment
) {
  if (!content) {
    throw std::invalid_argument("HuxerUI bottom sheet content factory must not be empty");
  }
  const ThemeSpec theme = detail::ResolveThemeSpec(environment);
  const BottomSheetStyle style = ResolveBottomSheetStyle(environment);
  ValidateBottomSheetStyle(style);
  const std::shared_ptr<detail::LayerTransitionState> transition =
      BottomSheetTransition(style, theme.motion.reduced_motion);
  LayerOptions layer_options = BottomSheetLayerOptions(std::move(options), style.scrim);
  detail::LayerPlacement placement;
  placement.kind = detail::LayerPlacementKind::BottomCenter;
  placement.fill_cross_axis = true;
  placement.maximum_cross_axis_extent = style.maximum_width;
  return layers_.AttachCaptured(
      std::move(layer_options),
      BottomSheetContent(std::move(content), style, transition),
      std::move(environment),
      std::move(placement),
      transition
  );
}

LayerId detail::BottomSheetService::Show(
    BottomSheetFactory content, BottomSheetOptions options, std::shared_ptr<const Environment> environment
) {
  if (!content) {
    throw std::invalid_argument("HuxerUI bottom sheet content factory must not be empty");
  }
  auto id = std::make_shared<LayerId>(0);
  const LayerId attached = Show(
      [layers = layers_, id, content = std::move(content)] { return content(BottomSheetContext{layers, *id}); },
      std::move(options),
      std::move(environment)
  );
  *id = attached;
  return attached;
}

bool detail::BottomSheetService::Dismiss(LayerId id) {
  return layers_.Dismiss(id);
}

BottomSheetHandle UseBottomSheet() {
  return BottomSheetHandle{
      UseService<detail::BottomSheetService>(),
      detail::CurrentEnvironment(),
  };
}

LayerAnchor PopupHandle::Anchor() const {
  return LayerAnchor{anchor_};
}

LayerId PopupHandle::Show(ViewFactory content, PopupOptions options) const {
  return service_->Show(anchor_, std::nullopt, std::move(content), std::move(options), environment_);
}

LayerId PopupHandle::Show(PopupFactory content, PopupOptions options) const {
  return service_->Show(anchor_, std::nullopt, std::move(content), std::move(options), environment_);
}

LayerId PopupHandle::ShowAt(Point point, ViewFactory content, PopupOptions options) const {
  return service_->Show(anchor_, point, std::move(content), std::move(options), environment_);
}

LayerId PopupHandle::ShowAt(Point point, PopupFactory content, PopupOptions options) const {
  return service_->Show(anchor_, point, std::move(content), std::move(options), environment_);
}

bool PopupHandle::Dismiss(LayerId id) const {
  return anchor_ && anchor_->Dismiss(id);
}

LayerId detail::PopupService::Show(
    const std::shared_ptr<detail::LayerAnchorState>& anchor,
    std::optional<Point> point,
    ViewFactory content,
    PopupOptions options,
    std::shared_ptr<const Environment> environment
) {
  if (!content) {
    throw std::invalid_argument("HuxerUI popup content factory must not be empty");
  }
  const AnchorPlacement preferred_placement = options.placement;
  const float gap = options.gap;
  const float viewport_margin = options.viewport_margin;
  const Point offset = options.offset;
  return anchor->AttachLayer(
      point,
      std::move(content),
      preferred_placement,
      gap,
      viewport_margin,
      offset,
      PopupLayerOptions(std::move(options)),
      std::move(environment)
  );
}

LayerId detail::PopupService::Show(
    const std::shared_ptr<detail::LayerAnchorState>& anchor,
    std::optional<Point> point,
    PopupFactory content,
    PopupOptions options,
    std::shared_ptr<const Environment> environment
) {
  if (!content) {
    throw std::invalid_argument("HuxerUI popup content factory must not be empty");
  }
  auto id = std::make_shared<LayerId>(0);
  const LayerId attached = Show(
      anchor,
      point,
      [anchor, id, content = std::move(content)] { return content(PopupContext{anchor, *id}); },
      std::move(options),
      std::move(environment)
  );
  *id = attached;
  return attached;
}

PopupHandle UsePopup() {
  const std::shared_ptr<detail::PopupService> service = UseService<detail::PopupService>();
  auto anchor = UseState(service->CreateAnchor());
  return PopupHandle{
      service,
      detail::CurrentEnvironment(),
      anchor.Get(),
  };
}

bool PopupContext::Dismiss() const {
  return anchor_ && anchor_->Dismiss(id_);
}

LayerAnchor MenuHandle::Anchor() const {
  return LayerAnchor{anchor_};
}

LayerId MenuHandle::Show(std::vector<MenuEntry> entries, MenuOptions options) const {
  return service_->Show(anchor_, std::nullopt, std::move(entries), std::move(options), environment_);
}

LayerId MenuHandle::ShowAt(Point point, std::vector<MenuEntry> entries, MenuOptions options) const {
  return service_->Show(anchor_, point, std::move(entries), std::move(options), environment_);
}

bool MenuHandle::Dismiss(LayerId id) const {
  return anchor_ && anchor_->Dismiss(id);
}

LayerId detail::MenuService::Show(
    const std::shared_ptr<detail::LayerAnchorState>& anchor,
    std::optional<Point> point,
    std::vector<MenuEntry> entries,
    MenuOptions options,
    std::shared_ptr<const Environment> environment
) {
  return ShowLevel(
      anchor,
      point,
      std::move(entries),
      std::move(options),
      std::move(environment),
      std::make_shared<MenuChainState>(),
      0,
      false
  );
}

LayerId detail::MenuService::ShowLevel(
    const std::shared_ptr<detail::LayerAnchorState>& anchor,
    std::optional<Point> point,
    std::vector<MenuEntry> entries,
    MenuOptions options,
    std::shared_ptr<const Environment> environment,
    const std::shared_ptr<MenuChainState>& chain,
    std::size_t depth,
    bool submenu
) {
  ValidateEntries(entries);
  const ThemeSpec theme = detail::ResolveThemeSpec(environment);
  const MenuStyle style = ResolveMenuStyle(environment);
  ValidateMenuStyle(style);
  if (options.width.has_value() && (!std::isfinite(*options.width) || *options.width <= 0.0F)) {
    throw std::invalid_argument("HuxerUI menu width must be finite and positive");
  }
  const AnchorPlacement preferred_placement = options.placement;
  const float gap = options.gap;
  const float viewport_margin = options.viewport_margin;
  const Point offset = options.offset;
  const std::optional<float> width = options.width;
  const std::shared_ptr<detail::LayerTransitionState> transition =
      PresentationTransition(style.motion, theme.motion.reduced_motion);
  if (submenu) {
    chain->DismissFrom(depth);
  }
  const LayerId attached = anchor->AttachLayer(
      point,
      [entries = std::move(entries), style, width, chain, depth, preferred_placement, transition]() -> View {
        View result = Stack {
          Surface(entries, style, width, chain, depth),
        };
        if (!transition || !style.motion.has_value()) {
          return result;
        }
        return std::move(result).With(
            PresentationContentMotion{
                .state = transition,
                .motion = *style.motion,
                .slide_direction = AnchorMotionDirection(preferred_placement.side),
                .origin = AnchorMotionOrigin(preferred_placement),
            }
        );
      },
      preferred_placement,
      gap,
      viewport_margin,
      offset,
      MenuLayerOptions(std::move(options), submenu),
      std::move(environment),
      transition
  );
  chain->Register(depth, anchor, attached);
  return attached;
}

MenuHandle UseMenu() {
  const std::shared_ptr<detail::MenuService> service = UseService<detail::MenuService>();
  auto anchor = UseState(service->CreateAnchor());
  return MenuHandle{
      service,
      detail::CurrentEnvironment(),
      anchor.Get(),
  };
}

const detail::ModifierDescriptor& Dialog::Descriptor() {
  return detail::ModifierDescriptorFor<Dialog, detail::DialogExtension>();
}

const detail::ModifierDescriptor& LayerAnchor::Descriptor() {
  return detail::ModifierDescriptorFor<LayerAnchor, detail::LayerAnchorExtension>();
}

} // namespace huxerui
