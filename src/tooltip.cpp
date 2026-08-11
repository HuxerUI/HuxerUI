#include <huxerui/presentation.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#include <huxerui/theme.h>

#include "internal.h"
#include "tooltip_internal.h"

namespace huxerui {

namespace detail {

class TooltipService;

struct TooltipTargetState {
  std::optional<Rect> bounds;
  bool anchor_hovered = false;
  bool surface_hovered = false;
  bool focus_visible = false;
  bool blocked = false;
  bool visible = false;
};

struct TooltipSurfaceHover {
  std::weak_ptr<TooltipTargetState> target;

  static const ModifierDescriptor& Descriptor();
};

class TooltipSurfaceHoverExtension final : public NodeExtension {
public:
  TooltipSurfaceHoverExtension(huxerui::MountedNode& node, const TooltipSurfaceHover& modifier) {
    Update(node, modifier);
  }

  void Update(huxerui::MountedNode& node, const TooltipSurfaceHover& modifier) {
    static_cast<void>(node);
    target_ = modifier.target;
  }

  ~TooltipSurfaceHoverExtension() override {
    if (const auto target = target_.lock()) {
      target->surface_hovered = false;
    }
  }

  bool HoverHitTest(huxerui::MountedNode& node, Point position) const override {
    return node.Bounds().Contains(position);
  }

  void OnHoverChanged(huxerui::MountedNode& node, bool hovered) override {
    static_cast<void>(node);
    if (const auto target = target_.lock()) {
      target->surface_hovered = hovered;
      if (!hovered && !target->anchor_hovered && !target->focus_visible) {
        target->blocked = false;
      }
    }
  }

private:
  std::weak_ptr<TooltipTargetState> target_;
};

const ModifierDescriptor& TooltipSurfaceHover::Descriptor() {
  return ModifierDescriptorFor<TooltipSurfaceHover, TooltipSurfaceHoverExtension>();
}

namespace {

void DismissTooltipFromLayer(
    std::weak_ptr<TooltipService> service, std::weak_ptr<TooltipTargetState> target, LayerId id
);

bool IsFiniteNonnegative(float value) noexcept {
  return std::isfinite(value) && value >= 0.0F;
}

bool IsFiniteNonnegative(double value) noexcept {
  return std::isfinite(value) && value >= 0.0;
}

bool IsValidTooltipInsets(const EdgeInsets& insets) noexcept {
  return IsFiniteNonnegative(insets.top) && IsFiniteNonnegative(insets.right) && IsFiniteNonnegative(insets.bottom) &&
         IsFiniteNonnegative(insets.left);
}

bool IsValidTooltipShadow(const Shadow& shadow) noexcept {
  return std::isfinite(shadow.offset.x) && std::isfinite(shadow.offset.y) && IsFiniteNonnegative(shadow.blur_radius) &&
         std::isfinite(shadow.spread);
}

void ValidateTooltipStyle(const TooltipStyle& style) {
  if (!IsValidTooltipInsets(style.padding) || !IsValidTooltipShadow(style.shadow) ||
      !IsFiniteNonnegative(style.corner_radius) || !IsFiniteNonnegative(style.minimum_height) ||
      !std::isfinite(style.maximum_width) || style.maximum_width <= 0.0F || !IsFiniteNonnegative(style.gap) ||
      !IsFiniteNonnegative(style.viewport_margin) || !IsFiniteNonnegative(style.hover_delay) ||
      !IsFiniteNonnegative(style.exit_delay) || !IsFiniteNonnegative(style.long_press_delay) ||
      !IsFiniteNonnegative(style.touch_show_duration)) {
    throw std::invalid_argument(
        "HuxerUI tooltip geometry, shadow, and timing must be finite with positive maximum width and non-negative "
        "extents"
    );
  }
}

TooltipStyle ResolveTooltipStyle(const std::shared_ptr<const Environment>& environment) {
  if (const std::any* value = FindThemeStyleValue(environment, typeid(TooltipStyle))) {
    if (const auto* style = std::any_cast<TooltipStyle>(value)) {
      ValidateTooltipStyle(*style);
      return *style;
    }
    throw std::logic_error("HuxerUI tooltip style environment value has an invalid type");
  }
  TooltipStyle style = TooltipStyle::Default();
  ValidateTooltipStyle(style);
  return style;
}

LayerAnchorSide ResolveTooltipSide(AnchorSide side) noexcept {
  switch (side) {
  case AnchorSide::Below:
    return LayerAnchorSide::Below;
  case AnchorSide::Above:
    return LayerAnchorSide::Above;
  case AnchorSide::Right:
    return LayerAnchorSide::Right;
  case AnchorSide::Left:
    return LayerAnchorSide::Left;
  }
  return LayerAnchorSide::Above;
}

LayerAnchorAlignment ResolveTooltipAlignment(AnchorAlignment alignment) noexcept {
  switch (alignment) {
  case AnchorAlignment::Start:
    return LayerAnchorAlignment::Start;
  case AnchorAlignment::Center:
    return LayerAnchorAlignment::Center;
  case AnchorAlignment::End:
    return LayerAnchorAlignment::End;
  }
  return LayerAnchorAlignment::Center;
}

LayerPlacement TooltipPlacement(Rect anchor, const TooltipStyle& style) {
  return {
      .kind = LayerPlacementKind::Anchored,
      .anchor = anchor,
      .preferred_side = ResolveTooltipSide(style.placement.side),
      .alignment = ResolveTooltipAlignment(style.placement.alignment),
      .gap = style.gap,
      .viewport_margin = style.viewport_margin,
      .offset = {},
      .safe_area_policy = LayerSafeAreaPolicy::Constrain,
  };
}

ViewFactory TooltipContent(std::weak_ptr<TooltipTargetState> target, StringVariant message, TooltipStyle style) {
  return [target = std::move(target), message = std::move(message), style = std::move(style)] {
    std::string resolved_message = UseString(message);
    if (resolved_message.empty()) {
      throw std::invalid_argument("HuxerUI tooltip message must not be empty");
    }
    Semantics semantics;
    semantics.descendants = SemanticDescendantPolicy::Exclude;
    semantics.hidden = true;
    return Text(std::move(resolved_message))
        .Style(style.text_style)
        .With(
            Frame{.max_width = style.maximum_width, .min_height = style.minimum_height},
            Padding{style.padding},
            Background{style.background},
            CornerRadius{style.corner_radius},
            style.shadow,
            TooltipSurfaceHover{target},
            std::move(semantics)
        );
  };
}

LayerOptions TooltipLayerOptions(
    std::weak_ptr<TooltipService> service, std::weak_ptr<TooltipTargetState> target, std::shared_ptr<LayerId> id
) {
  return {
      .level = LayerLevel::Notification,
      .pointer_policy = LayerPointerPolicy::Content,
      .trap_focus = false,
      .dismiss_on_outside_press = false,
      .cancel_policy = LayerCancelPolicy::Dismiss,
      .on_dismiss_request = [service = std::move(service),
                             target = std::move(target),
                             id = std::move(id)] { DismissTooltipFromLayer(service, target, *id); },
      .barrier_color = std::nullopt,
  };
}

} // namespace

class TooltipService final : public std::enable_shared_from_this<TooltipService> {
public:
  explicit TooltipService(LayerController layers) : layers_(std::move(layers)) {}

  void Show(
      const std::shared_ptr<TooltipTargetState>& target,
      StringVariant message,
      const TooltipStyle& style,
      std::shared_ptr<const Environment> environment
  ) {
    if (!target->bounds.has_value()) {
      return;
    }
    if (const auto active = active_target_.lock(); active && active != target) {
      active->visible = false;
      active->blocked = true;
      DismissActive();
    }

    target->visible = true;
    if (active_layer_.has_value() && active_target_.lock() == target) {
      layers_.UpdateCaptured(
          *active_layer_,
          TooltipLayerOptions(weak_from_this(), target, active_id_),
          TooltipContent(target, std::move(message), style),
          std::move(environment),
          TooltipPlacement(*target->bounds, style),
          {}
      );
      return;
    }

    auto id = std::make_shared<LayerId>(0);
    const LayerId attached = layers_.AttachCaptured(
        TooltipLayerOptions(weak_from_this(), target, id),
        TooltipContent(target, std::move(message), style),
        std::move(environment),
        TooltipPlacement(*target->bounds, style)
    );
    *id = attached;
    active_target_ = target;
    active_layer_ = attached;
    active_id_ = std::move(id);
  }

  void UpdatePlacement(const std::shared_ptr<TooltipTargetState>& target, const TooltipStyle& style) {
    if (active_layer_.has_value() && active_target_.lock() == target && target->bounds.has_value()) {
      layers_.UpdatePlacement(*active_layer_, TooltipPlacement(*target->bounds, style));
    }
  }

  void Dismiss(const std::shared_ptr<TooltipTargetState>& target, bool block) {
    if (block) {
      target->blocked = true;
    }
    target->visible = false;
    if (active_target_.lock() == target) {
      DismissActive();
    }
  }

  void DismissFromLayer(const std::weak_ptr<TooltipTargetState>& target, LayerId id) {
    const auto locked = target.lock();
    if (!locked || !active_layer_.has_value() || *active_layer_ != id || active_target_.lock() != locked) {
      return;
    }
    locked->blocked = true;
    locked->visible = false;
    DismissActive();
  }

private:
  void DismissActive() {
    const std::optional<LayerId> layer = active_layer_;
    active_target_.reset();
    active_layer_.reset();
    active_id_.reset();
    if (layer.has_value()) {
      layers_.Dismiss(*layer);
    }
  }

  LayerController layers_;
  std::weak_ptr<TooltipTargetState> active_target_;
  std::optional<LayerId> active_layer_;
  std::shared_ptr<LayerId> active_id_;
};

namespace {

void DismissTooltipFromLayer(
    std::weak_ptr<TooltipService> service, std::weak_ptr<TooltipTargetState> target, LayerId id
) {
  if (const auto locked = service.lock()) {
    locked->DismissFromLayer(target, id);
  }
}

} // namespace

static std::shared_ptr<TooltipService> TooltipServiceFor(const huxerui::MountedNode& node) {
  const auto& mounted = static_cast<const detail::MountedNode&>(node);
  const std::any* value = FindEnvironmentValue(mounted.environment, typeid(TooltipService));
  if (!value) {
    throw std::logic_error("HuxerUI tooltip service is not available");
  }
  const auto* service = std::any_cast<std::shared_ptr<TooltipService>>(value);
  if (!service || !*service) {
    throw std::logic_error("HuxerUI tooltip service environment value is invalid");
  }
  return *service;
}

class TooltipExtension final : public NodeExtension {
public:
  TooltipExtension(huxerui::MountedNode& node, const Tooltip& modifier)
      : target_(std::make_shared<TooltipTargetState>()) {
    Update(node, modifier);
  }

  ~TooltipExtension() override {
    if (service_) {
      try {
        service_->Dismiss(target_, false);
      } catch (...) {
      }
    }
  }

  void Update(huxerui::MountedNode& node, const Tooltip& modifier) {
    auto& mounted = static_cast<detail::MountedNode&>(node);
    if (!service_) {
      service_ = TooltipServiceFor(node);
    }
    message_ = modifier.message;
    style_ = ResolveTooltipStyle(mounted.environment);
    environment_ = mounted.environment;
    if (target_->visible) {
      service_->Show(target_, message_, style_, environment_);
    }
    InvalidateSemantics();
  }

  FrameResult OnFrame(huxerui::MountedNode& node, const FrameInfo& frame) override {
    const auto& mounted = static_cast<const detail::MountedNode&>(node);
    target_->focus_visible = mounted.enabled && mounted.focus_visible;
    if (!target_->anchor_hovered && !target_->surface_hovered && !target_->focus_visible &&
        !touch_pointer_.has_value()) {
      target_->blocked = false;
    }

    std::optional<double> next_deadline;
    const auto wake_at = [&](double deadline) {
      if (!next_deadline.has_value() || deadline < *next_deadline) {
        next_deadline = deadline;
      }
    };

    if (touch_pointer_.has_value() && !long_press_recognized_) {
      if (!long_press_deadline_.has_value()) {
        long_press_deadline_ = frame.timestamp + style_.long_press_delay;
      }
      if (frame.timestamp >= *long_press_deadline_) {
        long_press_recognized_ = true;
        Show();
      } else {
        wake_at(*long_press_deadline_);
      }
    }

    if (touch_duration_pending_) {
      touch_visible_until_ = frame.timestamp + style_.touch_show_duration;
      touch_duration_pending_ = false;
    }
    const bool touch_visible =
        long_press_recognized_ &&
        (touch_pointer_.has_value() || (touch_visible_until_.has_value() && frame.timestamp < *touch_visible_until_));
    if (!touch_pointer_.has_value() && touch_visible_until_.has_value() && frame.timestamp < *touch_visible_until_) {
      wake_at(*touch_visible_until_);
    }

    const bool hover_or_focus =
        !target_->blocked && (target_->anchor_hovered || target_->surface_hovered || target_->focus_visible);
    if (!target_->visible) {
      hide_deadline_.reset();
      if (target_->focus_visible && !target_->blocked) {
        Show();
      } else if (!touch_pointer_.has_value() && (target_->anchor_hovered || target_->surface_hovered) &&
                 !target_->blocked) {
        if (!hover_deadline_.has_value()) {
          hover_deadline_ = frame.timestamp + style_.hover_delay;
        }
        if (frame.timestamp >= *hover_deadline_) {
          Show();
        } else {
          wake_at(*hover_deadline_);
        }
      } else {
        hover_deadline_.reset();
      }
    } else if (hover_or_focus || touch_visible) {
      hide_deadline_.reset();
    } else {
      if (!hide_deadline_.has_value()) {
        hide_deadline_ = frame.timestamp + style_.exit_delay;
      }
      if (frame.timestamp >= *hide_deadline_) {
        Hide(false);
        hide_deadline_.reset();
        long_press_recognized_ = false;
        touch_visible_until_.reset();
      } else {
        wake_at(*hide_deadline_);
      }
    }

    if (!target_->visible && !touch_pointer_.has_value() &&
        (!touch_visible_until_.has_value() || frame.timestamp >= *touch_visible_until_)) {
      long_press_recognized_ = false;
      touch_visible_until_.reset();
    }
    if (next_deadline.has_value()) {
      return {
          .needs_frame = false,
          .wake_after = std::max(0.0, *next_deadline - frame.timestamp),
      };
    }
    return {};
  }

  bool PrepareGeometry(huxerui::MountedNode& node) override {
    const Rect bounds = node.PresentationBounds();
    if (target_->bounds == bounds) {
      return false;
    }
    target_->bounds = bounds;
    if (target_->visible) {
      service_->UpdatePlacement(target_, style_);
    }
    return false;
  }

  bool HitTest(huxerui::MountedNode& node, Point position) const override {
    return node.IsEnabled() && node.Bounds().Contains(position);
  }

  bool HoverHitTest(huxerui::MountedNode& node, Point position) const override {
    return node.Bounds().Contains(position);
  }

  bool HoverWhenDisabled() const noexcept override {
    return true;
  }

  void OnHoverChanged(huxerui::MountedNode& node, bool hovered) override {
    static_cast<void>(node);
    target_->anchor_hovered = hovered;
    if (!hovered) {
      hover_deadline_.reset();
      if (!target_->surface_hovered && !target_->focus_visible) {
        target_->blocked = false;
      }
    }
  }

  PointerResult OnPointer(huxerui::MountedNode& node, const PointerEvent& event) override {
    if (event.type == PointerEventType::Down) {
      Hide(event.device_kind != PointerDeviceKind::Touch);
      if (event.device_kind != PointerDeviceKind::Touch || !node.IsEnabled()) {
        return PointerResult::Ignored;
      }
      touch_pointer_ = event.pointer_id;
      touch_origin_ = event.position;
      long_press_deadline_.reset();
      long_press_recognized_ = false;
      touch_visible_until_.reset();
      touch_duration_pending_ = false;
      return PointerResult::Observe;
    }
    if (!touch_pointer_.has_value() || *touch_pointer_ != event.pointer_id) {
      return PointerResult::Ignored;
    }
    if (event.type == PointerEventType::Move && !long_press_recognized_ &&
        std::hypot(event.position.x - touch_origin_.x, event.position.y - touch_origin_.y) >= touch_gesture_slop) {
      ResetTouch();
      return PointerResult::Ignored;
    }
    if (event.type == PointerEventType::Up) {
      const bool recognized = long_press_recognized_;
      touch_pointer_.reset();
      long_press_deadline_.reset();
      touch_duration_pending_ = recognized;
      return recognized ? PointerResult::CancelTarget : PointerResult::Ignored;
    }
    if (event.type == PointerEventType::Cancel) {
      const bool recognized = long_press_recognized_;
      ResetTouch();
      touch_duration_pending_ = false;
      if (recognized) {
        long_press_recognized_ = false;
        touch_visible_until_.reset();
        Hide(false);
      }
      return PointerResult::Handled;
    }
    return long_press_recognized_ ? PointerResult::CancelTarget : PointerResult::Observe;
  }

  void BuildSemantics(SemanticBuilder& builder) const override {
    Semantics semantics;
    semantics.hint = message_;
    builder.SetOwner(std::move(semantics));
  }

private:
  void Show() {
    hover_deadline_.reset();
    service_->Show(target_, message_, style_, environment_);
  }

  void Hide(bool block) {
    hover_deadline_.reset();
    hide_deadline_.reset();
    if (service_) {
      service_->Dismiss(target_, block);
    }
  }

  void ResetTouch() {
    touch_pointer_.reset();
    long_press_deadline_.reset();
    if (!long_press_recognized_) {
      touch_visible_until_.reset();
    }
  }

  std::shared_ptr<TooltipService> service_;
  std::shared_ptr<TooltipTargetState> target_;
  std::shared_ptr<const Environment> environment_;
  StringVariant message_;
  TooltipStyle style_;
  std::optional<std::int64_t> touch_pointer_;
  Point touch_origin_;
  std::optional<double> hover_deadline_;
  std::optional<double> hide_deadline_;
  std::optional<double> long_press_deadline_;
  std::optional<double> touch_visible_until_;
  bool long_press_recognized_ = false;
  bool touch_duration_pending_ = false;
};

void InstallTooltip(RootContext& root) {
  root.Provide(std::make_shared<TooltipService>(root.Layers()));
}

} // namespace detail

Tooltip::Tooltip(StringVariant message) : message(std::move(message)) {
  if (detail::IsEmptyStringVariantLiteral(this->message)) {
    throw std::invalid_argument("HuxerUI tooltip message must not be empty");
  }
}

const detail::ModifierDescriptor& Tooltip::Descriptor() {
  return detail::ModifierDescriptorFor<Tooltip, detail::TooltipExtension>();
}

} // namespace huxerui
