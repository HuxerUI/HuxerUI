#include <huxerui/presentation.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <huxerui/animation.h>
#include <huxerui/app.h>
#include <huxerui/root.h>
#include <huxerui/theme.h>
#include <huxerui/window.h>

#include "huxerui_builtin_resources.h"
#include "internal_access.h"
#include "runtime/runtime_internal.h"
#include "resources/resource_internal.h"
#include "tooltip_internal.h"

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

struct SnackBarActionRequest {
  StringVariant label;
  std::function<void()> callback;
};

class SnackBarService : public std::enable_shared_from_this<SnackBarService> {
public:
  explicit SnackBarService(LayerController& layers) : layers_(layers) {}

  bool Dismiss(LayerId id);
  void Activate(LayerId id, const std::function<void()>& callback);

private:
  LayerId Show(StringVariant message, std::optional<SnackBarActionRequest> action, SnackBarOptions options,
               std::shared_ptr<const Environment> environment);

  LayerController layers_;
  std::optional<LayerId> replaceable_layer_;

  friend class huxerui::SnackBarHandle;
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
      LayerId id, const std::optional<PresentationMotion>& motion
  );
  ViewFactory PresentedContent(
      ViewFactory content,
      const DialogStyle& style,
      std::shared_ptr<LayerTransitionState> transition,
      std::shared_ptr<LayerId> id,
      bool dismissible
  ) const;
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

struct DismissAction {
  std::function<bool()> request;

  static const detail::ModifierDescriptor& Descriptor();
};

class DismissActionExtension final : public NodeExtension {
public:
  DismissActionExtension(MountedNode& node, const DismissAction& modifier) {
    Update(node, modifier);
  }

  void Update(MountedNode& node, const DismissAction& modifier) {
    static_cast<void>(node);
    request_ = modifier.request;
  }

  void BuildSemantics(SemanticBuilder& builder) const override {
    builder.SetOwner({});
    builder.AddAction(0, SemanticActionKind::Dismiss);
  }

  bool OnSemanticAction(std::uint64_t local_id, const SemanticAction& action) override {
    return local_id == 0 && action.kind == SemanticActionKind::Dismiss && request_ && request_();
  }

private:
  std::function<bool()> request_;
};

const detail::ModifierDescriptor& DismissAction::Descriptor() {
  return detail::ModifierDescriptorFor<DismissAction, DismissActionExtension>();
}

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

struct SnackBarPauseState {
  void Set(bool& target, bool value) noexcept {
    if (target == value) {
      return;
    }
    target = value;
    ++revision;
  }

  [[nodiscard]] bool Paused() const noexcept {
    return !application_active || surface_hovered || action_hovered || action_focused || action_pressed;
  }

  std::uint64_t revision = 0;
  bool application_active = true;
  bool surface_hovered = false;
  bool action_hovered = false;
  bool action_focused = false;
  bool action_pressed = false;
};

struct SnackBarLifetime {
  std::weak_ptr<detail::SnackBarService> service;
  std::shared_ptr<SnackBarPauseState> pause;
  LayerId id = 0;
  std::optional<double> duration;
  bool application_active = true;

  static const detail::ModifierDescriptor& Descriptor();
};

class SnackBarLifetimeExtension final : public NodeExtension {
public:
  SnackBarLifetimeExtension(MountedNode& node, const SnackBarLifetime& modifier) {
    Update(node, modifier);
  }

  void Update(MountedNode& node, const SnackBarLifetime& modifier) {
    static_cast<void>(node);
    const bool request_changed = id_ != modifier.id;
    service_ = modifier.service;
    pause_ = modifier.pause;
    id_ = modifier.id;
    pause_->Set(pause_->application_active, modifier.application_active);
    if (request_changed) {
      remaining_ = modifier.duration;
      last_timestamp_.reset();
      pause_revision_ = pause_->revision;
      dismissed_ = false;
    }
  }

  [[nodiscard]] bool HoverHitTest(MountedNode& node, Point position) const override {
    return node.Bounds().Contains(position);
  }

  void OnHover(MountedNode& node, const HoverEvent& event) override {
    static_cast<void>(node);
    pause_->Set(pause_->surface_hovered, event.type != HoverEventType::Leave);
  }

  NodeExtension::FrameResult OnFrame(MountedNode& node, const FrameInfo& frame) override {
    static_cast<void>(node);
    if (dismissed_ || !remaining_.has_value()) {
      return {};
    }
    if (!last_timestamp_.has_value() || pause_revision_ != pause_->revision) {
      last_timestamp_ = frame.timestamp;
      pause_revision_ = pause_->revision;
    } else if (!pause_->Paused()) {
      *remaining_ -= std::max(0.0, frame.timestamp - *last_timestamp_);
      last_timestamp_ = frame.timestamp;
    }
    if (pause_->Paused()) {
      return {};
    }
    if (*remaining_ > 0.0) {
      return {.wake_after = *remaining_};
    }
    dismissed_ = true;
    if (const auto service = service_.lock()) {
      service->Dismiss(id_);
    }
    return {};
  }

private:
  std::weak_ptr<detail::SnackBarService> service_;
  std::shared_ptr<SnackBarPauseState> pause_;
  LayerId id_ = 0;
  std::optional<double> remaining_;
  std::optional<double> last_timestamp_;
  std::uint64_t pause_revision_ = 0;
  bool dismissed_ = false;
};

const detail::ModifierDescriptor& SnackBarLifetime::Descriptor() {
  return detail::ModifierDescriptorFor<SnackBarLifetime, SnackBarLifetimeExtension>();
}

struct SnackBarActionPause {
  std::shared_ptr<SnackBarPauseState> pause;

  static const detail::ModifierDescriptor& Descriptor();
};

class SnackBarActionPauseExtension final : public NodeExtension {
public:
  SnackBarActionPauseExtension(MountedNode& node, const SnackBarActionPause& modifier) {
    Update(node, modifier);
  }

  void Update(MountedNode& node, const SnackBarActionPause& modifier) {
    static_cast<void>(node);
    pause_ = modifier.pause;
  }

  [[nodiscard]] bool HoverHitTest(MountedNode& node, Point position) const override {
    return node.Bounds().Contains(position);
  }

  void OnHover(MountedNode& node, const HoverEvent& event) override {
    static_cast<void>(node);
    pause_->Set(pause_->action_hovered, event.type != HoverEventType::Leave);
  }

  void OnFocusChanged(MountedNode& node, bool focused, bool) override {
    static_cast<void>(node);
    pause_->Set(pause_->action_focused, focused);
  }

  void OnInteraction(MountedNode& node, const InteractionState& state,
                     const std::optional<InteractionEvent>& event) override {
    static_cast<void>(node);
    static_cast<void>(event);
    pause_->Set(pause_->action_pressed, state.pressed);
  }

private:
  std::shared_ptr<SnackBarPauseState> pause_;
};

const detail::ModifierDescriptor& SnackBarActionPause::Descriptor() {
  return detail::ModifierDescriptorFor<SnackBarActionPause, SnackBarActionPauseExtension>();
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

SnackBarStyle ResolveSnackBarStyle(const std::shared_ptr<const Environment>& environment) {
  return ResolvePresentationStyle<SnackBarStyle>(environment, SnackBarStyle::Default());
}

DialogStyle ResolveDialogStyle(const std::shared_ptr<const Environment>& environment) {
  return ResolvePresentationStyle<DialogStyle>(environment, DialogStyle::Default());
}

BottomSheetStyle ResolveBottomSheetStyle(const std::shared_ptr<const Environment>& environment) {
  return ResolvePresentationStyle<BottomSheetStyle>(environment, BottomSheetStyle::Default());
}

bool ShowsBottomSheetDragHandle(const BottomSheetStyle& style) {
  return style.drag_handle_size.width > 0.0F && style.drag_handle_size.height > 0.0F && style.drag_handle.alpha > 0.0F;
}

MenuStyle ResolveMenuStyle(const std::shared_ptr<const Environment>& environment) {
  return ResolvePresentationStyle<MenuStyle>(environment, MenuStyle::Default());
}

struct BottomSheetDragState {
  float offset = 0.0F;
  float visual_offset = 0.0F;
  float dismiss_threshold = 0.0F;
  AnimationSpec settle = SpringSpec{};
  std::function<bool()> dismiss;
  std::uint64_t revision = 0;
  bool dragging = false;
  bool dismiss_requested = false;
  bool dismissed = false;
};

struct BottomSheetDragHandle {
  std::shared_ptr<BottomSheetDragState> state;

  static const detail::ModifierDescriptor& Descriptor();
};

class BottomSheetDragHandleExtension final : public NodeExtension {
public:
  BottomSheetDragHandleExtension(MountedNode& node, const BottomSheetDragHandle& modifier) {
    Update(node, modifier);
  }

  void Update(MountedNode& node, const BottomSheetDragHandle& modifier) {
    static_cast<void>(node);
    if (state_ == modifier.state) {
      return;
    }
    state_ = modifier.state;
    pointer_id_.reset();
  }

  bool HitTest(MountedNode& node, Point position) const override {
    return state_ && !state_->dismissed && node.IsEnabled() && node.Bounds().Contains(position);
  }

  PointerResult OnPointer(MountedNode& node, const PointerEvent& event) override {
    if (!state_ || state_->dismissed || !node.IsEnabled()) {
      pointer_id_.reset();
      return PointerResult::Ignored;
    }
    const Point host_position = node.LocalToWindow(event.position);
    if (event.type == PointerEventType::Down) {
      pointer_id_ = event.pointer_id;
      pointer_origin_ = host_position.y;
      offset_origin_ = state_->visual_offset;
      state_->offset = offset_origin_;
      state_->dragging = true;
      state_->dismiss_requested = false;
      ++state_->revision;
      return PointerResult::Capture;
    }
    if (!pointer_id_.has_value() || *pointer_id_ != event.pointer_id) {
      return PointerResult::Ignored;
    }
    if (event.type == PointerEventType::Move) {
      state_->offset = std::max(0.0F, offset_origin_ + host_position.y - pointer_origin_);
      ++state_->revision;
      return PointerResult::Handled;
    }
    if (event.type == PointerEventType::Up) {
      state_->offset = std::max(0.0F, offset_origin_ + host_position.y - pointer_origin_);
      state_->dragging = false;
      state_->dismiss_requested = state_->offset >= state_->dismiss_threshold;
      pointer_id_.reset();
      ++state_->revision;
      return PointerResult::Handled;
    }
    if (event.type == PointerEventType::Cancel) {
      state_->dragging = false;
      state_->dismiss_requested = false;
      pointer_id_.reset();
      ++state_->revision;
      return PointerResult::Handled;
    }
    return PointerResult::Handled;
  }

private:
  std::shared_ptr<BottomSheetDragState> state_;
  std::optional<std::int64_t> pointer_id_;
  float pointer_origin_ = 0.0F;
  float offset_origin_ = 0.0F;
};

const detail::ModifierDescriptor& BottomSheetDragHandle::Descriptor() {
  return detail::ModifierDescriptorFor<BottomSheetDragHandle, BottomSheetDragHandleExtension>();
}

struct PresentationContentMotion {
  std::shared_ptr<detail::LayerTransitionState> state;
  std::shared_ptr<BottomSheetDragState> bottom_sheet_drag{};
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
    if (state_ == modifier.state && bottom_sheet_drag_ == modifier.bottom_sheet_drag && motion_ == modifier.motion &&
        slide_direction_ == modifier.slide_direction && origin_ == modifier.origin &&
        slide_by_content_extent_ == modifier.slide_by_content_extent) {
      return;
    }
    const bool state_changed = state_ != modifier.state;
    const bool drag_state_changed = bottom_sheet_drag_ != modifier.bottom_sheet_drag;
    state_ = modifier.state;
    bottom_sheet_drag_ = modifier.bottom_sheet_drag;
    motion_ = modifier.motion;
    slide_direction_ = modifier.slide_direction;
    origin_ = modifier.origin;
    slide_by_content_extent_ = modifier.slide_by_content_extent;
    if (state_changed) {
      initialized_ = false;
    }
    if (drag_state_changed) {
      drag_revision_ = 0;
      drag_offset_.Set(0.0F);
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
    if (bottom_sheet_drag_) {
      const float sheet_height = node.Bounds().height;
      bottom_sheet_drag_->dismiss_threshold = std::clamp(sheet_height / 3.0F, 56.0F, 160.0F);
      if (drag_revision_ != bottom_sheet_drag_->revision) {
        drag_revision_ = bottom_sheet_drag_->revision;
        if (bottom_sheet_drag_->dragging) {
          drag_offset_.Set(bottom_sheet_drag_->offset);
        } else if (bottom_sheet_drag_->dismiss_requested) {
          drag_offset_.Set(bottom_sheet_drag_->offset);
          bottom_sheet_drag_->dismiss_requested = false;
          bottom_sheet_drag_->dismissed = bottom_sheet_drag_->dismiss && bottom_sheet_drag_->dismiss();
          if (!bottom_sheet_drag_->dismissed) {
            drag_offset_.AnimateTo(0.0F, bottom_sheet_drag_->settle);
          }
        } else {
          drag_offset_.AnimateTo(0.0F, bottom_sheet_drag_->settle);
        }
      }
    }
    if (target_visible_ != state_->target_visible) {
      target_visible_ = state_->target_visible;
      progress_.AnimateTo(target_visible_ ? 1.0F : 0.0F, target_visible_ ? state_->enter : state_->exit);
    }

    auto& mounted = static_cast<detail::MountedNode&>(node);
    const MotionAdvanceResult progress_result = progress_.Advance(frame);
    const MotionAdvanceResult drag_result = bottom_sheet_drag_ ? drag_offset_.Advance(frame)
                                                               : MotionAdvanceResult{};
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
    if (bottom_sheet_drag_) {
      bottom_sheet_drag_->visual_offset = drag_offset_.Value();
      mounted.presentation.local_transform = detail::ComposeTransform(
          detail::TranslationTransform({0.0F, bottom_sheet_drag_->visual_offset}),
          mounted.presentation.local_transform
      );
    }
    return {
        progress_result.needs_frame || drag_result.needs_frame,
        detail::EarliestWakeAfter(progress_result.wake_after, drag_result.wake_after),
    };
  }

private:
  std::shared_ptr<detail::LayerTransitionState> state_;
  std::shared_ptr<BottomSheetDragState> bottom_sheet_drag_;
  PresentationMotion motion_;
  Point slide_direction_;
  TransformOrigin origin_;
  bool slide_by_content_extent_ = false;
  MotionController progress_;
  MotionController drag_offset_;
  std::uint64_t drag_revision_ = 0;
  bool initialized_ = false;
  bool target_visible_ = false;
};

const detail::ModifierDescriptor& PresentationContentMotion::Descriptor() {
  return detail::ModifierDescriptorFor<PresentationContentMotion, PresentationContentMotionExtension>();
}

std::shared_ptr<detail::LayerTransitionState> PresentationTransition(
    const std::optional<PresentationMotion>& motion, bool enter_on_mount = true
) {
  if (!motion.has_value()) {
    return {};
  }
  return std::make_shared<detail::LayerTransitionState>(detail::LayerTransitionState{
      .target_visible = true,
      .enter_on_mount = enter_on_mount,
      .hidden_opacity = 0.0F,
      .enter = motion->enter,
      .exit = motion->exit,
      .on_exit_complete = {},
  });
}

void UpdatePresentationTransition(
    const std::shared_ptr<detail::LayerTransitionState>& transition, const PresentationMotion& motion
) {
  transition->hidden_opacity = 0.0F;
  transition->enter = motion.enter;
  transition->exit = motion.exit;
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
      !std::isfinite(style.corner_radius) || style.corner_radius < 0.0F || !std::isfinite(style.minimum_height) ||
      style.minimum_height < 0.0F || !std::isfinite(style.maximum_width) ||
      style.maximum_width <= 0.0F || (style.motion.has_value() && !ValidMotion(*style.motion))) {
    throw std::invalid_argument(
        "HuxerUI toast geometry, shadow, and motion must be finite with positive maximum width and non-negative extents"
    );
  }
}

void ValidateSnackBarStyle(const SnackBarStyle& style) {
  if (!ValidInsets(style.padding) || !ValidInsets(style.viewport_padding) || !ValidInsets(style.action_padding) ||
      !ValidShadow(style.shadow) || !std::isfinite(style.content_spacing) || style.content_spacing < 0.0F ||
      !std::isfinite(style.corner_radius) || style.corner_radius < 0.0F || !std::isfinite(style.minimum_height) ||
      style.minimum_height < 0.0F || !std::isfinite(style.maximum_width) || style.maximum_width <= 0.0F ||
      !std::isfinite(style.action_minimum_height) || style.action_minimum_height < 0.0F ||
      !std::isfinite(style.action_corner_radius) || style.action_corner_radius < 0.0F ||
      (style.motion.has_value() && !ValidMotion(*style.motion))) {
    throw std::invalid_argument(
        "HuxerUI SnackBar geometry, shadow, and motion must be finite with positive maximum width and non-negative "
        "extents"
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
      !std::isfinite(style.minimum_width) || style.minimum_width < 0.0F || !std::isfinite(style.maximum_width) ||
      style.maximum_width <= 0.0F || style.minimum_width > style.maximum_width ||
      !std::isfinite(style.viewport_margin) || style.viewport_margin < 0.0F ||
      (style.motion.has_value() && !ValidMotion(*style.motion))) {
    throw std::invalid_argument(
        "HuxerUI dialog geometry, shadow, and motion must be finite with positive maximum width and non-negative "
        "extents"
    );
  }
}

std::shared_ptr<detail::LayerTransitionState> BottomSheetTransition(const BottomSheetStyle& style) {
  return PresentationTransition(
      PresentationMotion{
          .enter = style.enter,
          .exit = style.exit,
      }
  );
}

void ValidateBottomSheetStyle(const BottomSheetStyle& style) {
  const CornerRadii& radii = style.corner_radii;
  if (!std::isfinite(radii.top_left) || radii.top_left < 0.0F || !std::isfinite(radii.top_right) ||
      radii.top_right < 0.0F || !std::isfinite(radii.bottom_right) || radii.bottom_right < 0.0F ||
      !std::isfinite(radii.bottom_left) || radii.bottom_left < 0.0F || !std::isfinite(style.maximum_width) ||
      style.maximum_width <= 0.0F || !std::isfinite(style.drag_handle_size.width) ||
      style.drag_handle_size.width < 0.0F || !std::isfinite(style.drag_handle_size.height) ||
      style.drag_handle_size.height < 0.0F || !ValidInsets(style.drag_handle_padding) || !ValidShadow(style.shadow)) {
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

bool IsBlankResolvedString(const std::string& value) {
  return value.find_first_not_of(" \t\n\r\f\v") == std::string::npos;
}

ViewFactory SnackBarContent(
    std::weak_ptr<detail::SnackBarService> service,
    std::shared_ptr<LayerId> id,
    StringVariant message,
    std::optional<detail::SnackBarActionRequest> action,
    std::optional<double> duration,
    SnackBarStyle style,
    std::shared_ptr<detail::LayerTransitionState> transition,
    std::shared_ptr<SnackBarPauseState> pause
) {
  return [service = std::move(service),
          id = std::move(id),
          message = std::move(message),
          action = std::move(action),
          duration,
          style = std::move(style),
          transition = std::move(transition),
          pause = std::move(pause)] {
    std::string resolved_message = UseString(message);
    if (IsBlankResolvedString(resolved_message)) {
      throw std::invalid_argument("HuxerUI SnackBar message must not be empty");
    }

    const bool application_active = UseApplication().LifecycleState() == ApplicationLifecycleState::Active;
    Semantics message_semantics;
    message_semantics.live_region = SemanticLiveRegion::Polite;
    std::vector<View> children;
    children.push_back(
        Text(std::move(resolved_message))
            .Style(style.message_style)
            .With(Grow{1.0F}, detail::BuiltInSemantics{std::move(message_semantics)})
    );

    if (action.has_value()) {
      std::string resolved_action = UseString(action->label);
      if (IsBlankResolvedString(resolved_action)) {
        throw std::invalid_argument("HuxerUI SnackBar action label must not be empty");
      }
      View action_button = Button(std::move(resolved_action))
                               .With(SnackBarActionPause{pause})
                               .OnClick([service, id, callback = action->callback] {
                                 if (const auto active = service.lock()) {
                                   active->Activate(*id, callback);
                                 }
                               });
      ThemeDefinition action_theme;
      action_theme.Set(ButtonStyle{
          .background = style.action_background,
          .label_style = style.action_text_style,
          .disabled_background = style.action_background,
          .disabled_label = style.action_text_style.foreground,
          .padding = style.action_padding,
          .minimum_height = style.action_minimum_height,
          .corner_radius = style.action_corner_radius,
          .indication = style.action_indication,
      });
      children.push_back(Theme {std::move(action_theme), std::move(action_button)});
    }

    Frame surface_frame;
    surface_frame.min_height = style.minimum_height;
    surface_frame.max_width = style.maximum_width;
    View result = Stack {
      Flow {std::move(children)}.With(
          surface_frame,
          Spacing{style.content_spacing},
          CrossAlign{CrossAxisAlignment::Center},
          Padding{style.padding},
          Background{style.background},
          CornerRadius{style.corner_radius},
          ClipChildren{},
          style.shadow,
          SnackBarLifetime{service, pause, *id, duration, application_active}
      ),
    }.With(Padding{style.viewport_padding});
    if (!transition || !style.motion.has_value()) {
      return result;
    }
    return std::move(result).With(
        PresentationContentMotion{
            .state = transition,
            .motion = *style.motion,
            .slide_direction = {0.0F, 1.0F},
            .origin = TransformOrigin{0.5F, 1.0F},
        }
    );
  };
}

Semantics DialogOwnerSemantics() {
  Semantics semantics;
  semantics.role = SemanticRole::Dialog;
  return semantics;
}

ViewFactory AnimatedDialogContent(
    ViewFactory content,
    const DialogStyle& style,
    std::shared_ptr<detail::LayerTransitionState> transition,
    std::function<bool()> request_dismiss
) {
  return [content = std::move(content),
          style,
          transition = std::move(transition),
          request_dismiss = std::move(request_dismiss)]() -> View {
    View result = content();
    if (transition && style.motion.has_value()) {
      result = Stack {std::move(result)}.With(
          PresentationContentMotion{
            .state = transition,
            .motion = *style.motion,
            .slide_direction = VerticalSlideDirection(style.placement),
            .origin = VerticalMotionOrigin(style.placement),
          }
      );
    }
    result = std::move(result).With(detail::BuiltInSemantics{DialogOwnerSemantics()});
    if (request_dismiss) {
      result = std::move(result).With(DismissAction{request_dismiss});
    }
    return result;
  };
}

ViewFactory BottomSheetContent(
    ViewFactory content,
    const BottomSheetStyle& style,
    std::shared_ptr<detail::LayerTransitionState> transition,
    std::shared_ptr<BottomSheetDragState> drag,
    std::function<bool()> request_dismiss
) {
  return [content = std::move(content),
          style,
          transition = std::move(transition),
          drag = std::move(drag),
          request_dismiss = std::move(request_dismiss)] {
    std::vector<View> children;
    if (ShowsBottomSheetDragHandle(style)) {
      Frame handle_frame;
      handle_frame.width = style.drag_handle_size.width;
      handle_frame.height = style.drag_handle_size.height;
      children.push_back(
          Row {
            Spacer().With(
                handle_frame,
                Grow{0.0F},
                Background{style.drag_handle},
                CornerRadius{style.drag_handle_size.height * 0.5F}
            ),
          }.With(
              Padding{style.drag_handle_padding},
              MainAlign{MainAxisAlignment::Center},
              CrossAlign{CrossAxisAlignment::Center},
              BottomSheetDragHandle{drag}
          )
      );
    }
    children.push_back(content());
    View result = Column {std::move(children)}.With(
        CrossAlign{CrossAxisAlignment::Stretch},
        Background{style.background},
        SafeAreaPadding{.top = false, .right = false, .left = false},
        CornerRadius{style.corner_radii},
        ClipChildren{},
        style.shadow,
        PresentationContentMotion{
            .state = transition,
            .bottom_sheet_drag = drag,
            .motion =
                PresentationMotion{
                    .enter = style.enter,
                    .exit = style.exit,
                },
            .slide_direction = {0.0F, 1.0F},
            .origin = TransformOrigin{0.5F, 1.0F},
            .slide_by_content_extent = true,
        },
        detail::BuiltInSemantics{DialogOwnerSemantics()}
    );
    if (request_dismiss) {
      result = std::move(result).With(DismissAction{request_dismiss});
    }
    return result;
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

void ValidateAnchoredOptions(float gap, float viewport_margin, Point offset) {
  if (!std::isfinite(gap) || gap < 0.0F) {
    throw std::invalid_argument("HuxerUI anchored presentation gap must be finite and non-negative");
  }
  if (!std::isfinite(viewport_margin) || viewport_margin < 0.0F) {
    throw std::invalid_argument("HuxerUI anchored presentation viewport margin must be finite and non-negative");
  }
  if (!std::isfinite(offset.x) || !std::isfinite(offset.y)) {
    throw std::invalid_argument("HuxerUI anchored presentation offset must be finite");
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
      constexpr float corner_inset = 30.0F;
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

constexpr Color debug_ribbon_background = Color::Rgb(155, 38, 52);
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
  ribbon_frame.width = 104.0F;
  ribbon_frame.height = 16.0F;
  return Row {
    Text("DEBUG").Style(TextStyle{Font::System(11.0F).WithWeight(FontWeight::Medium), debug_ribbon_foreground}),
  }.With(
      ribbon_frame,
      MainAlign{MainAxisAlignment::Center},
      CrossAlign{CrossAxisAlignment::Center},
      Background{debug_ribbon_background},
      Shadow{
          .color = debug_ribbon_shadow,
          .offset = {},
          .blur_radius = 12.0F,
      },
      Rotation{45.0F},
      Semantics{
          .role = SemanticRole::Button,
          .label = "DEBUG",
          .expanded = expanded.Get(),
          .descendants = SemanticDescendantPolicy::Exclude,
      }
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

enum class LayerAnchorMode {
  NodeBounds,
  LocalRect,
  FixedWindowPoint,
};

struct LayerAnchorTarget {
  LayerAnchorMode mode = LayerAnchorMode::NodeBounds;
  Rect bounds;
};

void ValidateLayerAnchorTarget(const LayerAnchorTarget& target) {
  if (target.mode == LayerAnchorMode::FixedWindowPoint) {
    if (!std::isfinite(target.bounds.x) || !std::isfinite(target.bounds.y)) {
      throw std::invalid_argument("HuxerUI anchored presentation point must be finite");
    }
    return;
  }
  if (target.mode == LayerAnchorMode::LocalRect &&
      (!std::isfinite(target.bounds.x) || !std::isfinite(target.bounds.y) ||
       !std::isfinite(target.bounds.width) || !std::isfinite(target.bounds.height) || target.bounds.width < 0.0F ||
       target.bounds.height < 0.0F)) {
    throw std::invalid_argument(
        "HuxerUI local presentation anchor must have finite coordinates and finite non-negative dimensions"
    );
  }
}

struct LayerAnchorState : std::enable_shared_from_this<LayerAnchorState> {
  explicit LayerAnchorState(LayerController controller) : layers(std::move(controller)) {}

  void Mount(std::uint64_t identity) {
    if (mounted) {
      throw std::logic_error("HuxerUI presentation anchor must be mounted on only one View");
    }
    mounted_identity = identity;
    mounted = true;
  }

  void Unmount() {
    mounted = false;
    mounted_identity.reset();
    node_bounds.reset();
    node_to_window.reset();
    const std::optional<LayerId> anchored_layer =
        active_target.mode == LayerAnchorMode::FixedWindowPoint ? std::nullopt : active_layer;
    if (anchored_layer.has_value()) {
      Dismiss(*anchored_layer);
    }
  }

  void UpdateGeometry(Rect next_bounds, Transform2D next_node_to_window) {
    node_bounds = next_bounds;
    node_to_window = next_node_to_window;
    if (!active_layer.has_value() || active_target.mode == LayerAnchorMode::FixedWindowPoint) {
      return;
    }
    const Rect next_anchor = ResolveTarget(active_target);
    if (active_placement.anchor != next_anchor) {
      active_placement.anchor = next_anchor;
      InternalAccess::UpdatePlacement(layers, *active_layer, active_placement);
    }
  }

  [[nodiscard]] Rect ResolveTarget(const LayerAnchorTarget& target) const {
    if (target.mode == LayerAnchorMode::FixedWindowPoint) {
      return target.bounds;
    }
    if (!mounted || !node_bounds.has_value() || !node_to_window.has_value()) {
      throw std::logic_error("HuxerUI anchored presentation requires a mounted anchor View");
    }
    if (target.mode == LayerAnchorMode::NodeBounds) {
      return *node_bounds;
    }
    return TransformBounds(*node_to_window, target.bounds);
  }

  void Bind(LayerId id, LayerPlacement placement, LayerAnchorTarget target) {
    if (active_layer.has_value() && *active_layer != id) {
      Dismiss(*active_layer);
    }
    active_layer = id;
    active_placement = std::move(placement);
    active_target = target;
  }

  bool UpdateLocalAnchor(LayerId id, Rect local_anchor) {
    const LayerAnchorTarget next_target{LayerAnchorMode::LocalRect, local_anchor};
    ValidateLayerAnchorTarget(next_target);
    if (active_layer != id || active_target.mode != LayerAnchorMode::LocalRect) {
      return false;
    }
    active_target = next_target;
    active_placement.anchor = ResolveTarget(active_target);
    return InternalAccess::UpdatePlacement(layers, id, active_placement);
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
      dismiss_handler = {};
    }
    return layers.Dismiss(id);
  }

  bool RequestDismissActive() {
    return active_layer.has_value() && InternalAccess::RequestDismiss(layers, *active_layer).handled;
  }

  void SetDismissHandler(LayerId id, std::function<bool(LayerId)> handler) {
    if (active_layer != id) {
      throw std::logic_error("HuxerUI presentation dismissal handler requires the active layer");
    }
    dismiss_handler = std::move(handler);
  }

  LayerId AttachLayer(
      LayerAnchorTarget target,
      ViewFactory content,
      AnchorPlacement preferred_placement,
      float gap,
      float viewport_margin,
      Point offset,
      bool retain_anchor_focus,
      LayerOptions options,
      std::shared_ptr<const Environment> environment,
      std::shared_ptr<LayerTransitionState> transition = {},
      std::shared_ptr<const SemanticModalGroupToken> semantic_modal_group = {}
  ) {
    ValidateAnchoredOptions(gap, viewport_margin, offset);
    ValidateLayerAnchorTarget(target);
    const Rect anchor_bounds = ResolveTarget(target);
    LayerPlacement placement = AnchoredPlacement(anchor_bounds, preferred_placement, gap, viewport_margin, offset);
    auto id = std::make_shared<LayerId>(0);
    if (!options.on_dismiss_request) {
      options.on_dismiss_request = [anchor = shared_from_this(), id] { anchor->Dismiss(*id); };
    }
    const LayerId attached = InternalAccess::AttachCaptured(
        layers,
        std::move(options),
        std::move(content),
        std::move(environment),
        placement,
        std::move(transition),
        std::move(semantic_modal_group),
        retain_anchor_focus ? mounted_identity : std::nullopt
    );
    *id = attached;
    Bind(attached, std::move(placement), target);
    return attached;
  }

  LayerController layers;
  std::optional<Rect> node_bounds;
  std::optional<Transform2D> node_to_window;
  std::optional<std::uint64_t> mounted_identity;
  // This identifies the layer currently owned by the anchor. It clears when dismissal begins even though the
  // LayerController may retain the same entry until its exit motion completes.
  std::optional<LayerId> active_layer;
  LayerPlacement active_placement;
  LayerAnchorTarget active_target;
  // Menu installs a chain-aware command here; ordinary Popup dismissal continues directly to LayerController.
  std::function<bool(LayerId)> dismiss_handler;
  bool mounted = false;
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

  [[nodiscard]] bool IsExpanded(std::size_t depth, const std::shared_ptr<LayerAnchorState>& anchor) const {
    if (levels.size() <= depth + 1) {
      return false;
    }
    return levels[depth + 1].anchor.lock() == anchor;
  }

  // Layer semantics compare this token without depending on MenuChainState or its operational menu state.
  std::shared_ptr<const SemanticModalGroupToken> semantic_modal_group = std::make_shared<SemanticModalGroupToken>();
  // Levels contains operationally open menus only. DismissFrom removes them before their retained Layer entries finish
  // exit motion, which lets submenu ownership and expansion state settle immediately.
  std::vector<Level> levels;
};

struct MenuExpansionAction {
  std::weak_ptr<MenuChainState> chain;
  std::weak_ptr<LayerAnchorState> submenu_anchor;
  std::size_t depth = 0;
  std::function<void()> expand;

  static const ModifierDescriptor& Descriptor();
};

class MenuExpansionActionExtension final : public NodeExtension {
public:
  MenuExpansionActionExtension(huxerui::MountedNode& node, const MenuExpansionAction& modifier) {
    Update(node, modifier);
  }

  void Update(huxerui::MountedNode& node, const MenuExpansionAction& modifier) {
    static_cast<void>(node);
    chain_ = modifier.chain;
    submenu_anchor_ = modifier.submenu_anchor;
    depth_ = modifier.depth;
    expand_ = modifier.expand;
  }

  void BuildSemantics(SemanticBuilder& builder) const override {
    const std::shared_ptr<MenuChainState> chain = chain_.lock();
    const std::shared_ptr<LayerAnchorState> anchor = submenu_anchor_.lock();
    const bool expanded = chain && anchor && chain->IsExpanded(depth_, anchor);
    Semantics semantics;
    semantics.expanded = expanded;
    builder.SetOwner(std::move(semantics));
    builder.AddAction(0, expanded ? SemanticActionKind::Collapse : SemanticActionKind::Expand);
  }

  bool OnSemanticAction(std::uint64_t local_id, const SemanticAction& action) override {
    if (local_id != 0) {
      return false;
    }
    const std::shared_ptr<MenuChainState> chain = chain_.lock();
    const std::shared_ptr<LayerAnchorState> anchor = submenu_anchor_.lock();
    if (!chain || !anchor) {
      return false;
    }
    const bool expanded = chain->IsExpanded(depth_, anchor);
    if (action.kind == SemanticActionKind::Expand && !expanded && expand_) {
      expand_();
      return true;
    }
    if (action.kind == SemanticActionKind::Collapse && expanded) {
      return chain->DismissFrom(depth_ + 1);
    }
    return false;
  }

private:
  std::weak_ptr<MenuChainState> chain_;
  std::weak_ptr<LayerAnchorState> submenu_anchor_;
  std::size_t depth_ = 0;
  std::function<void()> expand_;
};

const ModifierDescriptor& MenuExpansionAction::Descriptor() {
  return ModifierDescriptorFor<MenuExpansionAction, MenuExpansionActionExtension>();
}

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
      LayerAnchorTarget target,
      ViewFactory content,
      PopupOptions options,
      std::shared_ptr<const Environment> environment
  );
  LayerId Show(
      const std::shared_ptr<LayerAnchorState>& anchor,
      LayerAnchorTarget target,
      PopupFactory content,
      PopupOptions options,
      std::shared_ptr<const Environment> environment
  );
  bool Update(const std::shared_ptr<LayerAnchorState>& anchor, LayerId id, ViewFactory content,
              std::shared_ptr<const Environment> environment);
  bool Update(const std::shared_ptr<LayerAnchorState>& anchor, LayerId id, PopupFactory content,
              std::shared_ptr<const Environment> environment);

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
      MenuItem item,
      const MenuStyle& style,
      const std::shared_ptr<MenuChainState>& chain,
      std::size_t depth,
      std::size_t index
  );
  static View SeparatorView(const MenuStyle& style);
  static View Surface(
      std::vector<MenuEntry> entries,
      MenuStyle style,
      std::optional<float> width,
      const std::shared_ptr<MenuChainState>& chain,
      std::size_t depth,
      std::function<bool()> request_dismiss
  );

  LayerController layers_;
};

Semantics MenuContainerSemantics(std::size_t item_count) {
  Semantics semantics;
  semantics.role = SemanticRole::Menu;
  semantics.collection.emplace();
  semantics.collection->item_count = item_count;
  return semantics;
}

Semantics MenuEntrySemantics(const std::string& label, const std::optional<bool>& checked, std::size_t index) {
  Semantics semantics;
  semantics.role = SemanticRole::MenuItem;
  semantics.label = label;
  if (checked.has_value()) {
    semantics.checked = *checked ? SemanticCheckedState::Checked : SemanticCheckedState::Unchecked;
  }
  semantics.collection_item.emplace();
  semantics.collection_item->index = index;
  semantics.descendants = SemanticDescendantPolicy::Exclude;
  return semantics;
}

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
    MenuItem item,
    const MenuStyle& style,
    const std::shared_ptr<MenuChainState>& chain,
    std::size_t depth,
    std::size_t index
) {
  Frame item_frame;
  item_frame.min_height = style.minimum_item_height;
  Frame icon_frame;
  icon_frame.width = style.icon_size;
  icon_frame.height = style.icon_size;

  std::vector<View> content;
  if (item.checked_.value_or(false)) {
    content.push_back(Image(images::check).Fit(ImageFit::Contain).Tint(style.icon_tint).With(icon_frame));
  }

  std::optional<detail::ResolvedImageAsset> resolved_icon;
  if (item.icon_.has_value()) {
    resolved_icon = detail::UseImageVariant(*item.icon_);
  }
  if (resolved_icon.has_value()) {
    if (auto* vector = std::get_if<VectorAsset>(&*resolved_icon)) {
      content.push_back(
          Image(std::move(*vector))
              .Fit(ImageFit::Contain)
              .Tint(item.icon_tint_.value_or(style.icon_tint))
              .With(icon_frame)
      );
    } else {
      if (item.icon_tint_.has_value()) {
        throw std::invalid_argument("HuxerUI raster menu item icons do not support tint");
      }
      content.push_back(Image(std::move(std::get<ImageAsset>(*resolved_icon))).Fit(ImageFit::Contain).With(icon_frame));
    }
  }

  std::string label = UseString(std::move(item.label_));
  if (label.empty()) {
    throw std::invalid_argument("HuxerUI menu item label must not be empty");
  }
  content.push_back(Text(label).With(Foreground{style.foreground}));

  const detail::BuiltInSemantics item_semantics{MenuEntrySemantics(label, item.checked_, index)};

  if (std::holds_alternative<std::vector<MenuEntry>>(item.destination_)) {
    auto submenu = UseMenu();
    std::vector<MenuEntry> entries = std::get<std::vector<MenuEntry>>(std::move(item.destination_));
    Frame arrow_frame;
    arrow_frame.width = style.icon_size;
    std::function<void()> expand = [submenu, entries, chain, depth] {
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
    };
    return MenuItemLayout{
        Row {std::move(content)}.With(Spacing{style.item_content_spacing}, CrossAlign{CrossAxisAlignment::Center}),
        Image(images::chevron_right).Fit(ImageFit::Contain).Tint(style.icon_tint).With(arrow_frame),
    }
        .With(
            submenu.Anchor(),
            item_frame,
            Padding{style.item_padding},
            Spacing{style.item_content_spacing},
            Enabled{item.enabled_},
            style.item_indication,
            Focusable{},
            item_semantics,
            MenuExpansionAction{
                .chain = chain,
                .submenu_anchor = submenu.anchor_,
                .depth = depth,
                .expand = expand,
            }
        )
        .OnClick(std::move(expand));
  }

  std::function<void()> action = std::get<std::function<void()>>(std::move(item.destination_));
  return Row {std::move(content)}
      .With(
          item_frame,
          Padding{style.item_padding},
          Spacing{style.item_content_spacing},
          CrossAlign{CrossAxisAlignment::Center},
          Enabled{item.enabled_},
          style.item_indication,
          Focusable{},
          item_semantics
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
    std::size_t depth,
    std::function<bool()> request_dismiss
) {
  std::vector<View> children;
  const std::size_t item_count = static_cast<std::size_t>(std::ranges::count_if(entries, [](const MenuEntry& entry) {
    return std::holds_alternative<MenuItem>(entry.value_);
  }));
  std::size_t item_index = 0;
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
    children.push_back(ItemView(std::get<MenuItem>(std::move(entry.value_)), style, chain, depth, item_index++));
  }

  Frame surface_frame;
  if (width.has_value()) {
    surface_frame.width = *width;
  } else {
    surface_frame.min_width = style.minimum_width;
  }
  View result = Column {std::move(children)}.With(
      surface_frame,
      Padding{style.content_padding},
      CrossAlign{CrossAxisAlignment::Stretch},
      Background{style.background},
      CornerRadius{style.corner_radius},
      ClipChildren{},
      style.shadow,
      detail::BuiltInSemantics{MenuContainerSemantics(item_count)}
  );
  if (request_dismiss) {
    result = std::move(result).With(DismissAction{std::move(request_dismiss)});
  }
  return result;
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
  std::string resolved_title = UseString(title);
  std::string resolved_message = UseString(message);
  std::string resolved_positive = UseString(positive);
  if (resolved_positive.empty()) {
    resolved_positive = UseString(strings::dialog_ok);
  }
  std::optional<std::string> resolved_negative;
  if (negative.has_value()) {
    resolved_negative = UseString(*negative);
    if (resolved_negative->empty()) {
      *resolved_negative = UseString(strings::dialog_cancel);
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
    const Indication& indication =
        is_positive ? style.positive_action_indication : style.negative_action_indication;

    Frame action_frame;
    action_frame.min_height = style.minimum_action_height;
    Semantics action_semantics;
    action_semantics.role = SemanticRole::Button;
    View action_view = Text(std::move(label))
                           .Style(text_style)
                           .With(
                               action_frame,
                               Padding{style.action_padding},
                               Background{background},
                               CornerRadius{style.action_corner_radius},
                               indication,
                               Focusable{},
                               detail::BuiltInSemantics{std::move(action_semantics)}
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
    View action_flow =
        Flow {std::move(action_views)}.With(Spacing{style.action_spacing}, CrossAlign{CrossAxisAlignment::Center});
    actions = Stack {
      std::move(action_flow),
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
  surface_frame.min_width = style.minimum_width;
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
      state_->Mount(static_cast<const detail::MountedNode&>(node).identity);
    }
  }

  [[nodiscard]] PaintInvalidation PrepareGeometry(huxerui::MountedNode& node, huxerui::TextMeasurer&) override {
    if (state_) {
      const auto& mounted = static_cast<const detail::MountedNode&>(node);
      state_->UpdateGeometry(node.PresentationBounds(), mounted.presentation.resolved_transform);
    }
    return PaintInvalidation::None;
  }

private:
  std::shared_ptr<LayerAnchorState> state_;
};

class DebugOverlayInstaller {
public:
  static void Install(RootContext& root, std::shared_ptr<DebugMetricsState> metrics) {
    LayerPlacement placement;
    placement.kind = LayerPlacementKind::Fill;
    placement.safe_area_policy = LayerSafeAreaPolicy::Ignore;
    InternalAccess::AttachCaptured(
        root.Layers(),
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
          return DebugOverlayLayout {std::move(children)};
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
  root.Provide(std::make_shared<SnackBarService>(root.Layers()));
  root.Provide(std::make_shared<DialogService>(root.Layers()));
  root.Provide(std::make_shared<BottomSheetService>(root.Layers()));
  root.Provide(std::make_shared<PopupService>(root.Layers()));
  root.Provide(std::make_shared<MenuService>(root.Layers()));
  InstallTooltip(root);
}

void InstallDebugOverlay(RootContext& root, std::shared_ptr<DebugMetricsState> metrics) {
  DebugOverlayInstaller::Install(root, std::move(metrics));
}

} // namespace detail

namespace {

void ValidateMenuItemDeclaration(const StringVariant& label, const std::optional<ImageVariant>& icon) {
  if (detail::IsBlankStringVariantLiteral(label)) {
    throw std::invalid_argument("HuxerUI menu item label must not be empty");
  }
  if (icon.has_value()) {
    detail::ValidateImageVariant(*icon);
  }
}

} // namespace

MenuItem::MenuItem(StringVariant label, std::function<void()> on_item_click)
    : label_(std::move(label)), destination_(std::move(on_item_click)) {
  ValidateMenuItemDeclaration(label_, icon_);
}

MenuItem::MenuItem(ImageVariant icon, StringVariant label, std::function<void()> on_item_click)
    : label_(std::move(label)), icon_(std::move(icon)), destination_(std::move(on_item_click)) {
  ValidateMenuItemDeclaration(label_, icon_);
}

MenuItem::MenuItem(StringVariant label, std::vector<MenuEntry> children)
    : label_(std::move(label)), destination_(std::move(children)) {
  ValidateMenuItemDeclaration(label_, icon_);
}

MenuItem::MenuItem(ImageVariant icon, StringVariant label, std::vector<MenuEntry> children)
    : label_(std::move(label)), icon_(std::move(icon)), destination_(std::move(children)) {
  ValidateMenuItemDeclaration(label_, icon_);
}

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

MenuItem MenuItem::IconTint(Color tint) && {
  icon_tint_ = tint;
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
  const ToastStyle style = ResolveToastStyle(environment);
  ValidateToastStyle(style);
  const std::shared_ptr<detail::LayerTransitionState> transition = PresentationTransition(style.motion);
  auto id = std::make_shared<LayerId>(0);
  std::weak_ptr<ToastService> service = weak_from_this();
  detail::LayerPlacement placement;
  placement.kind = VerticalLayerPlacementKind(style.placement);
  const LayerId attached = InternalAccess::AttachCaptured(
      layers_,
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
        std::string resolved_message = UseString(message);
        if (resolved_message.empty()) {
          throw std::invalid_argument("HuxerUI toast message must not be empty");
        }
        Frame surface_frame;
        surface_frame.min_height = style.minimum_height;
        surface_frame.max_width = style.maximum_width;
        Semantics toast_semantics;
        toast_semantics.live_region = SemanticLiveRegion::Polite;
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
                  },
                  detail::BuiltInSemantics{std::move(toast_semantics)}
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

LayerId SnackBarHandle::Show(StringVariant message, SnackBarOptions options) const {
  return service_->Show(std::move(message), std::nullopt, options, environment_);
}

LayerId SnackBarHandle::Show(StringVariant message, StringVariant action, std::function<void()> on_action,
                             SnackBarOptions options) const {
  detail::SnackBarActionRequest request{std::move(action), std::move(on_action)};
  return service_->Show(std::move(message), std::move(request), options, environment_);
}

bool SnackBarHandle::Dismiss(LayerId id) const {
  return service_->Dismiss(id);
}

LayerId detail::SnackBarService::Show(StringVariant message, std::optional<SnackBarActionRequest> action,
                                      SnackBarOptions options, std::shared_ptr<const Environment> environment) {
  if (options.duration.has_value() &&
      (!std::isfinite(*options.duration) || *options.duration <= 0.0)) {
    throw std::invalid_argument("HuxerUI SnackBar duration must be finite and positive when specified");
  }
  if (detail::IsBlankStringVariantLiteral(message)) {
    throw std::invalid_argument("HuxerUI SnackBar message must not be empty");
  }
  if (action.has_value() && detail::IsBlankStringVariantLiteral(action->label)) {
    throw std::invalid_argument("HuxerUI SnackBar action label must not be empty");
  }
  if (action.has_value() && !action->callback) {
    throw std::invalid_argument("HuxerUI SnackBar action callback must not be empty");
  }

  SnackBarStyle style = ResolveSnackBarStyle(environment);
  ValidateSnackBarStyle(style);
  auto transition = PresentationTransition(style.motion);
  auto id = std::make_shared<LayerId>(0);
  auto pause = std::make_shared<SnackBarPauseState>();
  detail::LayerPlacement placement;
  placement.kind = detail::LayerPlacementKind::BottomCenter;
  const LayerId attached = InternalAccess::AttachCapturedReplacing(
      layers_,
      replaceable_layer_,
      LayerOptions{
          .level = LayerLevel::Notification,
          .pointer_policy = LayerPointerPolicy::Content,
          .trap_focus = false,
          .dismiss_on_outside_press = false,
          .cancel_policy = LayerCancelPolicy::PassThrough,
          .on_dismiss_request = {},
          .barrier_color = std::nullopt,
      },
      SnackBarContent(
          weak_from_this(), id, std::move(message), std::move(action), options.duration, style, transition, pause
      ),
      std::move(environment),
      std::move(placement),
      std::move(transition)
  );
  *id = attached;
  replaceable_layer_ = attached;
  return attached;
}

bool detail::SnackBarService::Dismiss(LayerId id) {
  return layers_.Dismiss(id);
}

void detail::SnackBarService::Activate(LayerId id, const std::function<void()>& callback) {
  if (!Dismiss(id)) {
    return;
  }
  callback();
}

SnackBarHandle UseSnackBar() {
  return SnackBarHandle{
      UseService<detail::SnackBarService>(),
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
    LayerId id, const std::optional<PresentationMotion>& motion
) {
  if (!motion.has_value()) {
    return {};
  }
  std::shared_ptr<detail::LayerTransitionState> transition = InternalAccess::Transition(layers_, id);
  if (transition) {
    UpdatePresentationTransition(transition, *motion);
    return transition;
  }
  transition = PresentationTransition(motion, false);
  return transition;
}

ViewFactory detail::DialogService::PresentedContent(
    ViewFactory content,
    const DialogStyle& style,
    std::shared_ptr<LayerTransitionState> transition,
    std::shared_ptr<LayerId> id,
    bool dismissible
) const {
  std::function<bool()> request_dismiss;
  if (dismissible) {
    request_dismiss = [layers = layers_, id = std::move(id)] { return InternalAccess::RequestDismiss(layers, *id).handled; };
  }
  return AnimatedDialogContent(std::move(content), style, std::move(transition), std::move(request_dismiss));
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
  const DialogStyle style = ResolveDialogStyle(environment);
  ValidateDialogStyle(style);
  const std::shared_ptr<detail::LayerTransitionState> transition = PresentationTransition(style.motion);
  auto id = std::make_shared<LayerId>(0);
  LayerOptions layer_options = DialogLayerOptions(std::move(options), style.scrim);
  const bool dismissible = layer_options.cancel_policy == LayerCancelPolicy::Dismiss;
  const LayerId attached = InternalAccess::AttachCaptured(
      layers_,
      std::move(layer_options),
      PresentedContent(
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
          transition,
          id,
          dismissible
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
  const DialogStyle style = ResolveDialogStyle(environment);
  ValidateDialogStyle(style);
  const std::shared_ptr<detail::LayerTransitionState> transition = PresentationTransition(style.motion);
  auto id = std::make_shared<LayerId>(0);
  LayerOptions layer_options = DialogLayerOptions(std::move(options), style.scrim);
  const bool dismissible = layer_options.cancel_policy == LayerCancelPolicy::Dismiss;
  const LayerId attached = InternalAccess::AttachCaptured(
      layers_,
      std::move(layer_options),
      PresentedContent(std::move(content), style, transition, id, dismissible),
      std::move(environment),
      DialogLayerPlacement(style),
      transition
  );
  *id = attached;
  return attached;
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
  const DialogStyle style = ResolveDialogStyle(environment);
  ValidateDialogStyle(style);
  std::optional<LayerOptions> layer_options = InternalAccess::EntryOptions(layers_, id);
  if (!layer_options.has_value()) {
    return false;
  }
  layer_options->barrier_color = style.scrim;
  const std::shared_ptr<detail::LayerTransitionState> transition = ReconcileTransition(id, style.motion);
  const bool dismissible = layer_options->cancel_policy == LayerCancelPolicy::Dismiss;
  return InternalAccess::UpdateCaptured(
      layers_,
      id,
      std::move(*layer_options),
      PresentedContent(std::move(content), style, transition, std::make_shared<LayerId>(id), dismissible),
      std::move(environment),
      DialogLayerPlacement(style),
      transition
  );
}

bool detail::DialogService::Update(
    LayerId id, ViewFactory content, const DialogOptions& options, std::shared_ptr<const Environment> environment
) {
  const DialogStyle style = ResolveDialogStyle(environment);
  ValidateDialogStyle(style);
  LayerOptions layer_options = DialogLayerOptions(options, style.scrim);
  const std::shared_ptr<detail::LayerTransitionState> transition = ReconcileTransition(id, style.motion);
  const bool dismissible = layer_options.cancel_policy == LayerCancelPolicy::Dismiss;
  return InternalAccess::UpdateCaptured(
      layers_,
      id,
      std::move(layer_options),
      PresentedContent(std::move(content), style, transition, std::make_shared<LayerId>(id), dismissible),
      std::move(environment),
      DialogLayerPlacement(style),
      transition
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
  const BottomSheetStyle style = ResolveBottomSheetStyle(environment);
  ValidateBottomSheetStyle(style);
  const std::shared_ptr<detail::LayerTransitionState> transition = BottomSheetTransition(style);
  const bool dismissible = options.dismiss_on_cancel || ShowsBottomSheetDragHandle(style);
  auto id_value = std::make_shared<LayerId>(0);
  std::function<bool()> request_dismiss;
  if (dismissible) {
    request_dismiss = [layers = layers_, id_value] { return InternalAccess::RequestDismiss(layers, *id_value).handled; };
  }
  std::shared_ptr<BottomSheetDragState> drag;
  if (ShowsBottomSheetDragHandle(style)) {
    drag = std::make_shared<BottomSheetDragState>();
    drag->settle = style.enter;
  }
  LayerOptions layer_options = BottomSheetLayerOptions(std::move(options), style.scrim);
  detail::LayerPlacement placement;
  placement.kind = detail::LayerPlacementKind::BottomCenter;
  placement.safe_area_policy = detail::LayerSafeAreaPolicy::ExtendBottom;
  placement.fill_cross_axis = true;
  placement.maximum_cross_axis_extent = style.maximum_width;
  const LayerId id = InternalAccess::AttachCaptured(
      layers_,
      std::move(layer_options),
      BottomSheetContent(std::move(content), style, transition, drag, std::move(request_dismiss)),
      std::move(environment),
      std::move(placement),
      transition
  );
  *id_value = id;
  if (drag) {
    drag->dismiss = [layers = layers_, id] { return InternalAccess::RequestDismiss(layers, id).dismissed; };
  }
  return id;
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
  return service_->Show(anchor_, {}, std::move(content), std::move(options), environment_);
}

LayerId PopupHandle::Show(PopupFactory content, PopupOptions options) const {
  return service_->Show(anchor_, {}, std::move(content), std::move(options), environment_);
}

LayerId PopupHandle::ShowAtAnchor(Rect local_anchor, ViewFactory content, PopupOptions options) const {
  return service_->Show(
      anchor_,
      detail::LayerAnchorTarget{detail::LayerAnchorMode::LocalRect, local_anchor},
      std::move(content),
      std::move(options),
      environment_
  );
}

LayerId PopupHandle::ShowAtAnchor(Rect local_anchor, PopupFactory content, PopupOptions options) const {
  return service_->Show(
      anchor_,
      detail::LayerAnchorTarget{detail::LayerAnchorMode::LocalRect, local_anchor},
      std::move(content),
      std::move(options),
      environment_
  );
}

bool PopupHandle::Update(LayerId id, ViewFactory content) const {
  return service_->Update(anchor_, id, std::move(content), environment_);
}

bool PopupHandle::Update(LayerId id, PopupFactory content) const {
  return service_->Update(anchor_, id, std::move(content), environment_);
}

bool PopupHandle::UpdateAnchor(LayerId id, Rect local_anchor) const {
  return anchor_ && anchor_->UpdateLocalAnchor(id, local_anchor);
}

LayerId PopupHandle::ShowAt(Point point, ViewFactory content, PopupOptions options) const {
  return service_->Show(
      anchor_,
      detail::LayerAnchorTarget{
          detail::LayerAnchorMode::FixedWindowPoint,
          {point.x, point.y, 0.0F, 0.0F},
      },
      std::move(content),
      std::move(options),
      environment_
  );
}

LayerId PopupHandle::ShowAt(Point point, PopupFactory content, PopupOptions options) const {
  return service_->Show(
      anchor_,
      detail::LayerAnchorTarget{
          detail::LayerAnchorMode::FixedWindowPoint,
          {point.x, point.y, 0.0F, 0.0F},
      },
      std::move(content),
      std::move(options),
      environment_
  );
}

bool PopupHandle::Dismiss(LayerId id) const {
  return anchor_ && anchor_->Dismiss(id);
}

LayerId detail::PopupService::Show(
    const std::shared_ptr<detail::LayerAnchorState>& anchor,
    LayerAnchorTarget target,
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
  const bool retain_anchor_focus = options.retain_anchor_focus;
  return anchor->AttachLayer(
      target,
      std::move(content),
      preferred_placement,
      gap,
      viewport_margin,
      offset,
      retain_anchor_focus,
      PopupLayerOptions(std::move(options)),
      std::move(environment)
  );
}

LayerId detail::PopupService::Show(
    const std::shared_ptr<detail::LayerAnchorState>& anchor,
    LayerAnchorTarget target,
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
      target,
      [anchor, id, content = std::move(content)] { return content(PopupContext{anchor, *id}); },
      std::move(options),
      std::move(environment)
  );
  *id = attached;
  return attached;
}

bool detail::PopupService::Update(const std::shared_ptr<detail::LayerAnchorState>& anchor, LayerId id,
                                  ViewFactory content, std::shared_ptr<const Environment> environment) {
  if (!content) {
    throw std::invalid_argument("HuxerUI popup content factory must not be empty");
  }
  if (!anchor || anchor->active_layer != id) {
    return false;
  }
  return InternalAccess::UpdateEntry(layers_, id, std::nullopt, std::move(content), std::move(environment));
}

bool detail::PopupService::Update(const std::shared_ptr<detail::LayerAnchorState>& anchor, LayerId id,
                                  PopupFactory content, std::shared_ptr<const Environment> environment) {
  if (!content) {
    throw std::invalid_argument("HuxerUI popup content factory must not be empty");
  }
  return Update(anchor, id,
                [anchor, id, content = std::move(content)] { return content(PopupContext{anchor, id}); },
                std::move(environment));
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
  const bool dismissible = options.dismiss_on_cancel;
  const std::shared_ptr<detail::LayerTransitionState> transition = PresentationTransition(style.motion);
  if (submenu) {
    chain->DismissFrom(depth);
  }
  LayerAnchorTarget target;
  if (point.has_value()) {
    target = {
        LayerAnchorMode::FixedWindowPoint,
        {point->x, point->y, 0.0F, 0.0F},
    };
  }
  const LayerId attached = anchor->AttachLayer(
      target,
      [entries = std::move(entries),
       style,
       width,
       chain,
       depth,
       preferred_placement,
       transition,
       dismissible,
       anchor = std::weak_ptr<LayerAnchorState>(anchor)]() -> View {
        std::function<bool()> request_dismiss;
        if (dismissible) {
          request_dismiss = [anchor] {
            const std::shared_ptr<LayerAnchorState> locked = anchor.lock();
            return locked && locked->RequestDismissActive();
          };
        }
        View result = Stack {
          Surface(entries, style, width, chain, depth, std::move(request_dismiss)),
        };
        if (transition && style.motion.has_value()) {
          result = std::move(result).With(
              PresentationContentMotion{
                .state = transition,
                .motion = *style.motion,
                .slide_direction = AnchorMotionDirection(preferred_placement.side),
                .origin = AnchorMotionOrigin(preferred_placement),
              }
          );
        }
        return result;
      },
      preferred_placement,
      gap,
      viewport_margin,
      offset,
      false,
      MenuLayerOptions(std::move(options), submenu),
      std::move(environment),
      transition,
      chain->semantic_modal_group
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
