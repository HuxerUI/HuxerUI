#include <huxerui/presentation.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include <huxerui/theme.h>

#include "gesture_internal.h"
#include "runtime_internal.h"
#include "resource_internal.h"
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

struct TooltipConfiguration {
  std::string message;
  TooltipStyle style;

  bool operator==(const TooltipConfiguration&) const = default;
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

  void OnHover(huxerui::MountedNode& node, const HoverEvent& event) override {
    static_cast<void>(node);
    if (event.type == HoverEventType::Move) {
      return;
    }
    const bool hovered = event.type == HoverEventType::Enter;
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

ViewFactory TooltipContent(std::weak_ptr<TooltipTargetState> target, std::string message, TooltipStyle style) {
  return [target = std::move(target), message = std::move(message), style = std::move(style)] {
    Semantics semantics;
    semantics.descendants = SemanticDescendantPolicy::Exclude;
    semantics.hidden = true;
    return Text(std::move(message))
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

void CompileTooltipModifier(
    ViewSpec&,
    ModifierSpec& modifier,
    const std::shared_ptr<const Environment>& environment,
    AppResources& resources
) {
  const auto& declaration = *static_cast<const Tooltip*>(modifier.value.get());
  const Locale locale = NeedsResourceResolution(declaration.message)
                            ? ResolveResourceLocale(environment, resources)
                            : Locale::Default();
  std::string message = ResolveString(declaration.message, resources, locale);
  if (message.empty()) {
    throw std::invalid_argument("HuxerUI tooltip message must not be empty");
  }
  modifier.value = std::make_shared<TooltipConfiguration>(TooltipConfiguration{
      std::move(message),
      ResolveTooltipStyle(environment),
  });
}

class TooltipService final : public std::enable_shared_from_this<TooltipService> {
public:
  explicit TooltipService(LayerController layers) : layers_(std::move(layers)) {}

  void Show(
      const std::shared_ptr<TooltipTargetState>& target,
      std::string message,
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

class TooltipExtension;

class TooltipTouchRecognizer final : public GestureRecognizer {
public:
  TooltipTouchRecognizer(const PointerEvent& event, double timestamp, const TooltipStyle& style, float movement_slop)
      : origin_(event.position), deadline_(timestamp + style.long_press_delay), movement_slop_(movement_slop) {}

  GestureDecision Update(const GestureRecognizerInput& input) override {
    if (input.event.type == PointerEventType::Move &&
        std::hypot(input.event.position.x - origin_.x, input.event.position.y - origin_.y) >= movement_slop_) {
      deadline_.reset();
      return GestureDecision::Reject;
    }
    if (input.event.type == PointerEventType::Up || input.event.type == PointerEventType::Cancel) {
      deadline_.reset();
      return GestureDecision::Reject;
    }
    return GestureDecision::Continue;
  }

  std::optional<double> Deadline() const noexcept override {
    return deadline_;
  }

  GestureDecision AdvanceDeadline(double timestamp) override {
    if (!deadline_.has_value() || timestamp < *deadline_) {
      return GestureDecision::Continue;
    }
    deadline_.reset();
    return GestureDecision::Accept;
  }

  void Accepted(MountedNode& node, NodeExtension& extension, const GestureRecognizerInput& input) override;
  void UpdateAccepted(MountedNode& node, NodeExtension& extension, const GestureRecognizerInput& input) override;
  void Canceled(MountedNode& node, NodeExtension& extension, const GestureRecognizerInput& input) override;

private:
  Point origin_;
  std::optional<double> deadline_;
  float movement_slop_ = 0.0F;
  bool owns_touch_ = false;
};

class TooltipExtension final : public NodeExtension {
public:
  TooltipExtension(huxerui::MountedNode& node, const TooltipConfiguration& modifier)
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

  void Update(huxerui::MountedNode& node, const TooltipConfiguration& modifier) {
    auto& mounted = static_cast<detail::MountedNode&>(node);
    if (!service_) {
      service_ = TooltipServiceFor(node);
    }
    message_ = modifier.message;
    style_ = modifier.style;
    environment_ = mounted.environment;
    if (target_->visible) {
      service_->Show(target_, message_, style_, environment_);
    }
    InvalidateSemantics();
  }

  FrameResult OnFrame(huxerui::MountedNode& node, const FrameInfo& frame) override {
    const auto& mounted = static_cast<const detail::MountedNode&>(node);
    target_->focus_visible = mounted.interaction.enabled && mounted.interaction.focus_visible;
    if (!target_->anchor_hovered && !target_->surface_hovered && !target_->focus_visible &&
        active_touch_pointers_.empty()) {
      target_->blocked = false;
    }

    std::optional<double> next_deadline;
    const auto wake_at = [&](double deadline) {
      if (!next_deadline.has_value() || deadline < *next_deadline) {
        next_deadline = deadline;
      }
    };

    if (touch_duration_pending_) {
      touch_visible_until_ = frame.timestamp + style_.touch_show_duration;
      touch_duration_pending_ = false;
    }
    const bool touch_visible =
        long_press_recognized_ &&
        (!active_touch_pointers_.empty() ||
         (touch_visible_until_.has_value() && frame.timestamp < *touch_visible_until_));
    if (active_touch_pointers_.empty() && touch_visible_until_.has_value() &&
        frame.timestamp < *touch_visible_until_) {
      wake_at(*touch_visible_until_);
    }

    const bool hover_or_focus =
        !target_->blocked && (target_->anchor_hovered || target_->surface_hovered || target_->focus_visible);
    if (!target_->visible) {
      hide_deadline_.reset();
      if (target_->focus_visible && !target_->blocked) {
        Show();
      } else if (active_touch_pointers_.empty() && (target_->anchor_hovered || target_->surface_hovered) &&
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

    if (!target_->visible && active_touch_pointers_.empty() &&
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

  PaintInvalidation PrepareGeometry(huxerui::MountedNode& node, huxerui::TextMeasurer&) override {
    const Rect bounds = node.PresentationBounds();
    if (target_->bounds == bounds) {
      return PaintInvalidation::None;
    }
    target_->bounds = bounds;
    if (target_->visible) {
      service_->UpdatePlacement(target_, style_);
    }
    return PaintInvalidation::None;
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

  void OnHover(huxerui::MountedNode& node, const HoverEvent& event) override {
    static_cast<void>(node);
    if (event.type == HoverEventType::Move) {
      target_->anchor_hovered = true;
      hover_deadline_.reset();
      if (target_->visible && !target_->focus_visible) {
        Hide(false);
      }
      return;
    }
    const bool hovered = event.type == HoverEventType::Enter;
    target_->anchor_hovered = hovered;
    if (!hovered) {
      hover_deadline_.reset();
      if (!target_->surface_hovered && !target_->focus_visible) {
        target_->blocked = false;
      }
    }
  }

  PointerResult OnPointer(huxerui::MountedNode& node, const PointerEvent& event) override {
    static_cast<void>(node);
    if (event.type == PointerEventType::Down && event.device_kind != PointerDeviceKind::Touch) {
      Hide(true);
    }
    return PointerResult::Ignored;
  }

  void BuildSemantics(SemanticBuilder& builder) const override {
    Semantics semantics;
    semantics.hint = message_;
    builder.SetOwner(std::move(semantics));
  }

private:
  std::shared_ptr<GestureRecognizer>
  CreateGestureRecognizer(huxerui::MountedNode& node, const PointerEvent& event, double timestamp,
                          const GestureSettings& settings, Transform2D) override {
    if (event.device_kind != PointerDeviceKind::Touch || !node.IsEnabled()) {
      return {};
    }
    if (active_touch_pointers_.empty()) {
      Hide(false);
      long_press_recognized_ = false;
      touch_visible_until_.reset();
      touch_duration_pending_ = false;
    }
    return std::make_shared<TooltipTouchRecognizer>(event, timestamp, style_, settings.pointer_slop);
  }

  bool BeginTouch(std::int64_t pointer_id) {
    if (std::ranges::find(active_touch_pointers_, pointer_id) != active_touch_pointers_.end()) {
      return false;
    }
    active_touch_pointers_.push_back(pointer_id);
    long_press_recognized_ = true;
    Show();
    return true;
  }

  void EndTouch(std::int64_t pointer_id) {
    const auto active = std::ranges::find(active_touch_pointers_, pointer_id);
    if (active == active_touch_pointers_.end()) {
      return;
    }
    active_touch_pointers_.erase(active);
    if (active_touch_pointers_.empty()) {
      touch_duration_pending_ = true;
      InvalidatePaint();
    }
  }

  void CancelTouch(std::int64_t pointer_id) {
    const auto active = std::ranges::find(active_touch_pointers_, pointer_id);
    if (active == active_touch_pointers_.end()) {
      return;
    }
    active_touch_pointers_.erase(active);
    if (!active_touch_pointers_.empty()) {
      return;
    }
    touch_duration_pending_ = false;
    long_press_recognized_ = false;
    touch_visible_until_.reset();
    Hide(false);
  }

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

  std::shared_ptr<TooltipService> service_;
  std::shared_ptr<TooltipTargetState> target_;
  std::shared_ptr<const Environment> environment_;
  std::string message_;
  TooltipStyle style_;
  std::vector<std::int64_t> active_touch_pointers_;
  std::optional<double> hover_deadline_;
  std::optional<double> hide_deadline_;
  std::optional<double> touch_visible_until_;
  bool long_press_recognized_ = false;
  bool touch_duration_pending_ = false;

  friend class TooltipTouchRecognizer;
};

void TooltipTouchRecognizer::Accepted(MountedNode&, NodeExtension& extension,
                                      const GestureRecognizerInput& input) {
  owns_touch_ = static_cast<TooltipExtension&>(extension).BeginTouch(input.event.pointer_id);
}

void TooltipTouchRecognizer::UpdateAccepted(MountedNode&, NodeExtension& extension,
                                            const GestureRecognizerInput& input) {
  if (!owns_touch_ || input.event.type != PointerEventType::Up) {
    return;
  }
  owns_touch_ = false;
  static_cast<TooltipExtension&>(extension).EndTouch(input.event.pointer_id);
}

void TooltipTouchRecognizer::Canceled(MountedNode&, NodeExtension& extension,
                                      const GestureRecognizerInput& input) {
  if (!owns_touch_) {
    return;
  }
  owns_touch_ = false;
  static_cast<TooltipExtension&>(extension).CancelTouch(input.event.pointer_id);
}

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
  static const detail::ModifierDescriptor descriptor{
      detail::CompileTooltipModifier,
      [](MountedNode& node, const void* value) -> std::unique_ptr<NodeExtension> {
        return std::make_unique<detail::TooltipExtension>(
            node, *static_cast<const detail::TooltipConfiguration*>(value)
        );
      },
      [](NodeExtension& extension, MountedNode& node, const void* value) {
        static_cast<detail::TooltipExtension&>(extension).Update(
            node, *static_cast<const detail::TooltipConfiguration*>(value)
        );
      },
      false,
      detail::ErasedEqualsFor<detail::TooltipConfiguration>(),
  };
  return descriptor;
}

} // namespace huxerui
