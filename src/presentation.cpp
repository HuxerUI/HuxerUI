#include <huxerui/presentation.h>

#include <cmath>
#include <optional>
#include <stdexcept>
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
  LayerId Show(std::string message, ToastOptions options, std::shared_ptr<const Environment> environment);

  LayerController layers_;

  friend class huxerui::ToastHandle;
};

class DialogService {
public:
  explicit DialogService(LayerController& layers) : layers_(layers) {}

  bool Update(LayerId id, ViewFactory content);
  bool Update(LayerId id, DialogFactory content);
  bool Dismiss(LayerId id);

private:
  LayerId Show(ViewFactory content, DialogOptions options, std::shared_ptr<const Environment> environment);
  LayerId Show(DialogFactory content, DialogOptions options, std::shared_ptr<const Environment> environment);
  bool Update(LayerId id, ViewFactory content, DialogOptions options, std::shared_ptr<const Environment> environment);

  LayerController layers_;

  friend class huxerui::DialogHandle;
  friend class DialogExtension;
};

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

ToastStyle DefaultToastStyle(const ThemeSpec& theme) {
  Color background = theme.colors.inverse_surface;
  background.alpha *= 0.94F;
  return {
      background,
      theme.colors.inverse_on_surface,
      theme.spacing.small + theme.spacing.extra_small,
      theme.shapes.medium,
  };
}

DialogStyle DefaultDialogStyle(const ThemeSpec& theme) {
  return {
      theme.colors.scrim,
  };
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
  return ResolvePresentationStyle<ToastStyle>(environment, DefaultToastStyle(detail::ResolveThemeSpec(environment)));
}

DialogStyle ResolveDialogStyle(const std::shared_ptr<const Environment>& environment) {
  return ResolvePresentationStyle<DialogStyle>(environment, DefaultDialogStyle(detail::ResolveThemeSpec(environment)));
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

LayerOptions DialogLayerOptions(DialogOptions options, const std::shared_ptr<const Environment>& environment) {
  return {
      .level = LayerLevel::Presentation,
      .pointer_policy = LayerPointerPolicy::Barrier,
      .trap_focus = true,
      .dismiss_on_outside_press = options.dismiss_on_outside_press,
      .cancel_policy =
          options.dismiss_on_cancel ? LayerCancelPolicy::Dismiss : LayerCancelPolicy::Consume,
      .on_dismiss_request = std::move(options.on_dismiss_request),
      .barrier_color = ResolveDialogStyle(environment).scrim,
  };
}

void ValidateAnchoredOptions(float gap, float viewport_margin, const std::optional<Point>& point) {
  if (!std::isfinite(gap) || gap < 0.0F) {
    throw std::invalid_argument("HuxerUI anchored presentation gap must be finite and non-negative");
  }
  if (!std::isfinite(viewport_margin) || viewport_margin < 0.0F) {
    throw std::invalid_argument("HuxerUI anchored presentation viewport margin must be finite and non-negative");
  }
  if (point.has_value() && (!std::isfinite(point->x) || !std::isfinite(point->y))) {
    throw std::invalid_argument("HuxerUI anchored presentation point must be finite");
  }
}

detail::LayerAnchorSide ResolveAnchorSide(AnchorPlacement placement) noexcept {
  switch (placement) {
  case AnchorPlacement::Below:
    return detail::LayerAnchorSide::Below;
  case AnchorPlacement::Above:
    return detail::LayerAnchorSide::Above;
  case AnchorPlacement::Right:
    return detail::LayerAnchorSide::Right;
  case AnchorPlacement::Left:
    return detail::LayerAnchorSide::Left;
  }
  return detail::LayerAnchorSide::Below;
}

detail::LayerPlacement AnchoredPlacement(Rect anchor, AnchorPlacement placement, float gap, float viewport_margin) {
  return detail::LayerPlacement{
      .kind = detail::LayerPlacementKind::Anchored,
      .anchor = anchor,
      .preferred_side = ResolveAnchorSide(placement),
      .gap = gap,
      .viewport_margin = viewport_margin,
  };
}

LayerOptions BottomSheetLayerOptions(
    BottomSheetOptions options, const std::shared_ptr<const Environment>& environment
) {
  return {
      .level = LayerLevel::Presentation,
      .pointer_policy = LayerPointerPolicy::Barrier,
      .trap_focus = true,
      .dismiss_on_outside_press = options.dismiss_on_outside_press,
      .cancel_policy =
          options.dismiss_on_cancel ? LayerCancelPolicy::Dismiss : LayerCancelPolicy::Consume,
      .on_dismiss_request = std::move(options.on_dismiss_request),
      .barrier_color = detail::ResolveThemeSpec(environment).colors.scrim,
  };
}

LayerOptions PopupLayerOptions(PopupOptions options) {
  return {
      .level = LayerLevel::Presentation,
      .pointer_policy = options.dismiss_on_outside_press ? LayerPointerPolicy::Barrier : LayerPointerPolicy::Content,
      .trap_focus = options.trap_focus,
      .dismiss_on_outside_press = options.dismiss_on_outside_press,
      .cancel_policy =
          options.dismiss_on_cancel ? LayerCancelPolicy::Dismiss : LayerCancelPolicy::Consume,
      .on_dismiss_request = std::move(options.on_dismiss_request),
      .barrier_color = std::nullopt,
  };
}

LayerOptions MenuLayerOptions(MenuOptions options) {
  return {
      .level = LayerLevel::Presentation,
      .pointer_policy = LayerPointerPolicy::Barrier,
      .trap_focus = true,
      .dismiss_on_outside_press = options.dismiss_on_outside_press,
      .cancel_policy =
          options.dismiss_on_cancel ? LayerCancelPolicy::Dismiss : LayerCancelPolicy::Consume,
      .on_dismiss_request = std::move(options.on_dismiss_request),
      .barrier_color = std::nullopt,
  };
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
      result.Place(node.ChildAt(0), {12.0F, 40.0F});
    }
    if (node.ChildCount() > 1) {
      MountedNode& banner = node.ChildAt(1);
      result.Place(
          banner,
          {
              std::max(
                  -banner.LayoutSize().width * 0.5F,
                  constraints.max_width - banner.LayoutSize().width * 0.5F - 26.0F
              ),
              26.0F - banner.LayoutSize().height * 0.5F,
          }
      );
    }
    result.SetSize(constraints.Constrain({constraints.max_width, constraints.max_height}));
    return result;
  }
};

constexpr Color debug_banner_background = Color::Rgb(198, 40, 40);
constexpr Color debug_banner_foreground = Color::Rgb(255, 245, 157);
constexpr Color debug_banner_shadow = Color::Rgb(0, 0, 0, 0.28F);
constexpr Color debug_panel_background = Color::Rgb(24, 28, 35, 0.96F);
constexpr Color debug_panel_foreground = Color::White();
constexpr Color debug_shadow_color = Color::Rgb(0, 0, 0, 0.35F);

struct DebugSampler {
  State<float> fps;

  static const detail::ModifierDescriptor& Descriptor();
};

class DebugSamplerExtension final : public NodeExtension {
public:
  DebugSamplerExtension(MountedNode& node, const DebugSampler& modifier) {
    Update(node, modifier);
  }

  void Update(MountedNode& node, const DebugSampler& modifier) {
    static_cast<void>(node);
    fps_ = modifier.fps;
  }

  NodeExtension::FrameResult OnFrame(MountedNode& node, const FrameInfo& frame) override {
    static_cast<void>(node);
    if (!window_started_at_.has_value()) {
      window_started_at_ = frame.timestamp;
    }
    ++frames_;
    const double elapsed = frame.timestamp - *window_started_at_;
    if (elapsed >= 0.5) {
      fps_ = static_cast<float>(static_cast<double>(frames_) / elapsed);
      window_started_at_ = frame.timestamp;
      frames_ = 0;
    }
    return {
        .needs_frame = true,
        .wake_after = std::nullopt,
    };
  }

private:
  State<float> fps_;
  std::optional<double> window_started_at_;
  std::size_t frames_ = 0;
};

const detail::ModifierDescriptor& DebugSampler::Descriptor() {
  return detail::ModifierDescriptorFor<DebugSampler, DebugSamplerExtension>();
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

  void Unmount() noexcept {
    mounted = false;
    bounds.reset();
    if (active_layer.has_value() && follows_anchor) {
      layers.Dismiss(*active_layer);
      active_layer.reset();
    }
    follows_anchor = false;
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
      layers.Dismiss(*active_layer);
    }
    active_layer = id;
    active_placement = std::move(placement);
    follows_anchor = should_follow_anchor;
  }

  bool Dismiss(LayerId id) {
    const bool dismissed = layers.Dismiss(id);
    if (active_layer == id) {
      active_layer.reset();
      follows_anchor = false;
    }
    return dismissed;
  }

  LayerId AttachLayer(
      std::optional<Point> point,
      ViewFactory content,
      AnchorPlacement preferred_placement,
      float gap,
      float viewport_margin,
      LayerOptions options,
      std::shared_ptr<const Environment> environment
  ) {
    ValidateAnchoredOptions(gap, viewport_margin, point);
    const Rect anchor_bounds = point.has_value() ? Rect{point->x, point->y, 0.0F, 0.0F} : RequireBounds();
    LayerPlacement placement =
        AnchoredPlacement(anchor_bounds, preferred_placement, gap, viewport_margin);
    auto id = std::make_shared<LayerId>(0);
    if (!options.on_dismiss_request) {
      options.on_dismiss_request = [anchor = shared_from_this(), id] { anchor->Dismiss(*id); };
    }
    const LayerId attached = layers.AttachCaptured(
        std::move(options), std::move(content), std::move(environment), placement
    );
    *id = attached;
    Bind(attached, std::move(placement), !point.has_value());
    return attached;
  }

  LayerController layers;
  std::optional<Rect> bounds;
  std::optional<LayerId> active_layer;
  LayerPlacement active_placement;
  bool mounted = false;
  bool follows_anchor = false;
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
  bool Dismiss(const std::shared_ptr<LayerAnchorState>& anchor, LayerId id);

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
      ViewFactory content,
      MenuOptions options,
      std::shared_ptr<const Environment> environment
  );
  LayerId Show(
      const std::shared_ptr<LayerAnchorState>& anchor,
      std::optional<Point> point,
      MenuFactory content,
      MenuOptions options,
      std::shared_ptr<const Environment> environment
  );
  bool Dismiss(const std::shared_ptr<LayerAnchorState>& anchor, LayerId id);

private:
  LayerController layers_;
};

class LayerAnchorExtension final : public NodeExtension {
public:
  LayerAnchorExtension(huxerui::MountedNode& node, const LayerAnchor& modifier) {
    Update(node, modifier);
  }

  ~LayerAnchorExtension() override {
    if (state_) {
      state_->Unmount();
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
  static void Install(RootContext& root) {
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
        [] {
          auto expanded = UseState(false);
          auto fps = UseState(0.0F);
          std::vector<View> children;
          if (expanded.Get()) {
            Frame panel_frame;
            panel_frame.width = 216.0F;
            children.push_back(
                Column {
                  Text("HuxerUI Debug").Style(
                      TextStyle{Font::System(16.0F).WithWeight(FontWeight::SemiBold), debug_panel_foreground}
                  ),
                  Text::Format("UI FPS: {}", static_cast<int>(std::lround(fps.Get())))
                      .Style(TextStyle{Font::System(13.0F), debug_panel_foreground}),
                }.With(
                    panel_frame,
                    Spacing{6.0F},
                    Padding{12.0F},
                    Background{debug_panel_background},
                    Shadow{
                        .color = debug_shadow_color,
                        .offset = {0.0F, 4.0F},
                        .blur_radius = 12.0F,
                        .spread = -1.0F,
                    },
                    CornerRadius{8.0F},
                    DebugSampler{fps}
                )
            );
          } else {
            children.push_back(Spacer());
          }
          Frame banner_frame;
          banner_frame.width = 136.0F;
          banner_frame.height = 28.0F;
          children.push_back(
              Row {
                Text("Debug").Style(
                    TextStyle{Font::System(11.0F).WithWeight(FontWeight::SemiBold), debug_banner_foreground}
                ),
              }
                  .With(
                      banner_frame,
                      MainAlign{MainAxisAlignment::Center},
                      CrossAlign{CrossAxisAlignment::Center},
                      Background{debug_banner_background},
                      Shadow{
                          .color = debug_banner_shadow,
                          .offset = {},
                          .blur_radius = 8.0F,
                      },
                      Rotation{45.0F}
                  )
                  .OnClick([expanded] { expanded = !expanded.Get(); })
          );
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
      service_->Dismiss(*layer_);
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
        layer_.reset();
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
      service_->Update(*layer_, modifier.content, std::move(options), mounted.environment);
      return;
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

void InstallDebugOverlay(RootContext& root) {
  DebugOverlayInstaller::Install(root);
}

} // namespace detail

LayerId ToastHandle::Show(std::string message, ToastOptions options) const {
  return service_->Show(std::move(message), options, environment_);
}

bool ToastHandle::Dismiss(LayerId id) const {
  return service_->Dismiss(id);
}

LayerId detail::ToastService::Show(
    std::string message, ToastOptions options, std::shared_ptr<const Environment> environment
) {
  if (!std::isfinite(options.duration) || options.duration < 0.0) {
    throw std::invalid_argument("HuxerUI toast duration must be finite and non-negative");
  }
  const ToastStyle style = ResolveToastStyle(environment);
  auto id = std::make_shared<LayerId>(0);
  std::weak_ptr<ToastService> service = weak_from_this();
  detail::LayerPlacement placement;
  placement.kind = detail::LayerPlacementKind::BottomCenter;
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
      [service, id, message = std::move(message), options, style] {
        return Stack {
          Text(message).With(
              Padding{style.padding},
              Background{style.background},
              Foreground{style.foreground},
              CornerRadius{style.corner_radius},
              ToastLifetime{
                  service,
                  *id,
                  options.duration,
              }
          ),
        }.With(
            Padding{EdgeInsets{
                0.0F,
                16.0F,
                24.0F,
                16.0F,
            }}
        );
      },
      std::move(environment),
      std::move(placement)
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

bool DialogHandle::Update(LayerId id, ViewFactory content) const {
  return service_->Update(id, std::move(content));
}

bool DialogHandle::Update(LayerId id, DialogFactory content) const {
  return service_->Update(id, std::move(content));
}

bool DialogHandle::Dismiss(LayerId id) const {
  return service_->Dismiss(id);
}

LayerId detail::DialogService::Show(
    ViewFactory content, DialogOptions options, std::shared_ptr<const Environment> environment
) {
  LayerOptions layer_options = DialogLayerOptions(std::move(options), environment);
  detail::LayerPlacement placement;
  placement.kind = detail::LayerPlacementKind::Center;
  return layers_
      .AttachCaptured(std::move(layer_options), std::move(content), std::move(environment), std::move(placement));
}

LayerId detail::DialogService::Show(
    DialogFactory content, DialogOptions options, std::shared_ptr<const Environment> environment
) {
  if (!content) {
    throw std::invalid_argument("HuxerUI dialog content factory must not be empty");
  }
  auto id = std::make_shared<LayerId>(0);
  LayerOptions layer_options = DialogLayerOptions(std::move(options), environment);
  detail::LayerPlacement placement;
  placement.kind = detail::LayerPlacementKind::Center;
  const LayerId attached = layers_.AttachCaptured(
      std::move(layer_options),
      [layers = layers_, id, content = std::move(content)] { return content(DialogContext{layers, *id}); },
      std::move(environment),
      std::move(placement)
  );
  *id = attached;
  return attached;
}

bool detail::DialogService::Update(LayerId id, ViewFactory content) {
  return layers_.Update(id, std::move(content));
}

bool detail::DialogService::Update(
    LayerId id, ViewFactory content, DialogOptions options, std::shared_ptr<const Environment> environment
) {
  LayerOptions layer_options = DialogLayerOptions(std::move(options), environment);
  return layers_.UpdateCaptured(
      id, std::move(layer_options), std::move(content), std::move(environment)
  );
}

bool detail::DialogService::Update(LayerId id, DialogFactory content) {
  if (!content) {
    throw std::invalid_argument("HuxerUI dialog content factory must not be empty");
  }
  return layers_.Update(id, [layers = layers_, id, content = std::move(content)] {
    return content(DialogContext{layers, id});
  });
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
  LayerOptions layer_options = BottomSheetLayerOptions(std::move(options), environment);
  detail::LayerPlacement placement;
  placement.kind = detail::LayerPlacementKind::BottomCenter;
  return layers_
      .AttachCaptured(std::move(layer_options), std::move(content), std::move(environment), std::move(placement));
}

LayerId detail::BottomSheetService::Show(
    BottomSheetFactory content, BottomSheetOptions options, std::shared_ptr<const Environment> environment
) {
  if (!content) {
    throw std::invalid_argument("HuxerUI bottom sheet content factory must not be empty");
  }
  auto id = std::make_shared<LayerId>(0);
  LayerOptions layer_options = BottomSheetLayerOptions(std::move(options), environment);
  detail::LayerPlacement placement;
  placement.kind = detail::LayerPlacementKind::BottomCenter;
  const LayerId attached = layers_.AttachCaptured(
      std::move(layer_options),
      [layers = layers_, id, content = std::move(content)] { return content(BottomSheetContext{layers, *id}); },
      std::move(environment),
      std::move(placement)
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
  return service_->Dismiss(anchor_, id);
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
  return anchor->AttachLayer(
      point,
      std::move(content),
      preferred_placement,
      gap,
      viewport_margin,
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

bool detail::PopupService::Dismiss(const std::shared_ptr<detail::LayerAnchorState>& anchor, LayerId id) {
  return anchor->Dismiss(id);
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

LayerId MenuHandle::Show(ViewFactory content, MenuOptions options) const {
  return service_->Show(anchor_, std::nullopt, std::move(content), std::move(options), environment_);
}

LayerId MenuHandle::Show(MenuFactory content, MenuOptions options) const {
  return service_->Show(anchor_, std::nullopt, std::move(content), std::move(options), environment_);
}

LayerId MenuHandle::ShowAt(Point point, ViewFactory content, MenuOptions options) const {
  return service_->Show(anchor_, point, std::move(content), std::move(options), environment_);
}

LayerId MenuHandle::ShowAt(Point point, MenuFactory content, MenuOptions options) const {
  return service_->Show(anchor_, point, std::move(content), std::move(options), environment_);
}

bool MenuHandle::Dismiss(LayerId id) const {
  return service_->Dismiss(anchor_, id);
}

LayerId detail::MenuService::Show(
    const std::shared_ptr<detail::LayerAnchorState>& anchor,
    std::optional<Point> point,
    ViewFactory content,
    MenuOptions options,
    std::shared_ptr<const Environment> environment
) {
  if (!content) {
    throw std::invalid_argument("HuxerUI menu content factory must not be empty");
  }
  const AnchorPlacement preferred_placement = options.placement;
  const float gap = options.gap;
  const float viewport_margin = options.viewport_margin;
  return anchor->AttachLayer(
      point,
      std::move(content),
      preferred_placement,
      gap,
      viewport_margin,
      MenuLayerOptions(std::move(options)),
      std::move(environment)
  );
}

LayerId detail::MenuService::Show(
    const std::shared_ptr<detail::LayerAnchorState>& anchor,
    std::optional<Point> point,
    MenuFactory content,
    MenuOptions options,
    std::shared_ptr<const Environment> environment
) {
  if (!content) {
    throw std::invalid_argument("HuxerUI menu content factory must not be empty");
  }
  auto id = std::make_shared<LayerId>(0);
  const LayerId attached = Show(
      anchor,
      point,
      [anchor, id, content = std::move(content)] { return content(MenuContext{anchor, *id}); },
      std::move(options),
      std::move(environment)
  );
  *id = attached;
  return attached;
}

bool detail::MenuService::Dismiss(const std::shared_ptr<detail::LayerAnchorState>& anchor, LayerId id) {
  return anchor->Dismiss(id);
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

bool MenuContext::Dismiss() const {
  return anchor_ && anchor_->Dismiss(id_);
}

ToastStyle ToastStyle::Default() {
  return DefaultToastStyle(ThemeSpec::Default());
}

DialogStyle DialogStyle::Default() {
  return DefaultDialogStyle(ThemeSpec::Default());
}

const detail::ModifierDescriptor& Dialog::Descriptor() {
  return detail::ModifierDescriptorFor<Dialog, detail::DialogExtension>();
}

const detail::ModifierDescriptor& LayerAnchor::Descriptor() {
  return detail::ModifierDescriptorFor<LayerAnchor, detail::LayerAnchorExtension>();
}

} // namespace huxerui
