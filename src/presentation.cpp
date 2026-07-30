#include <huxerui/presentation.h>

#include <cmath>
#include <optional>
#include <stdexcept>

#include <huxerui/theme.h>

#include "internal.h"

namespace huxerui {

namespace {

struct ToastLifetime {
  std::weak_ptr<ToastService> service;
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
  std::weak_ptr<ToastService> service_;
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
Style ResolvePresentationStyle(const std::shared_ptr<const detail::EnvironmentFrame>& environment, Style fallback) {
  if (const std::any* value = detail::FindThemeStyleValue(environment, typeid(Style))) {
    if (const auto* style = std::any_cast<Style>(value)) {
      return *style;
    }
    throw std::logic_error("HuxerUI presentation style environment value has an invalid type");
  }
  return fallback;
}

ToastStyle ResolveToastStyle(const std::shared_ptr<const detail::EnvironmentFrame>& environment) {
  return ResolvePresentationStyle<ToastStyle>(environment, DefaultToastStyle(detail::ResolveThemeSpec(environment)));
}

DialogStyle ResolveDialogStyle(const std::shared_ptr<const detail::EnvironmentFrame>& environment) {
  return ResolvePresentationStyle<DialogStyle>(
      environment,
      DefaultDialogStyle(detail::ResolveThemeSpec(environment))
  );
}

std::shared_ptr<DialogService> DialogServiceFor(const detail::MountedNode& node) {
  const std::any* value = detail::FindEnvironmentValue(node.environment, typeid(DialogService));
  if (!value) {
    throw std::logic_error("HuxerUI dialog service is not available");
  }
  const auto* service = std::any_cast<std::shared_ptr<DialogService>>(value);
  if (!service || !*service) {
    throw std::logic_error("HuxerUI dialog service environment value is invalid");
  }
  return *service;
}

LayerOptions
DialogLayerOptions(DialogOptions options, const std::shared_ptr<const detail::EnvironmentFrame>& environment) {
  return {
      .kind = LayerKind::Modal,
      .input_policy = LayerInputPolicy::Modal,
      .dismiss_on_outside_press = options.dismiss_on_outside_press,
      .on_dismiss_request = std::move(options.on_dismiss_request),
      .modal_scrim = ResolveDialogStyle(environment).scrim,
  };
}

class DialogExtension final : public NodeExtension {
public:
  DialogExtension(MountedNode& node, const Dialog& modifier) {
    Update(node, modifier);
  }

  ~DialogExtension() override {
    if (service_ && layer_.has_value()) {
      service_->Dismiss(*layer_);
    }
  }

  void Update(MountedNode& node, const Dialog& modifier);

private:
  std::shared_ptr<DialogService> service_;
  std::optional<LayerId> layer_;
};

} // namespace

namespace detail {

void InstallBuiltinPresentation(RootContext& root) {
  root.Provide(std::make_shared<ToastService>(root.Layers()));
  root.Provide(std::make_shared<DialogService>(root.Layers()));
}

struct DialogModifierAccess {
  static LayerId Show(
      DialogService& service,
      ViewFactory content,
      DialogOptions options,
      std::shared_ptr<const EnvironmentFrame> environment
  ) {
    return service.Show(std::move(content), options, std::move(environment));
  }

  static bool Update(
      DialogService& service,
      LayerId id,
      ViewFactory content,
      DialogOptions options,
      std::shared_ptr<const EnvironmentFrame> environment
  ) {
    return service.Update(id, std::move(content), std::move(options), std::move(environment));
  }
};

} // namespace detail

void DialogExtension::Update(MountedNode& node, const Dialog& modifier) {
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
  if (modifier.dismiss_on_outside_press && !modifier.on_dismiss_request) {
    throw std::invalid_argument("HuxerUI dismissible Dialog modifier requires "
                                "on_dismiss_request");
  }
  DialogOptions options{
      .dismiss_on_outside_press = modifier.dismiss_on_outside_press,
      .on_dismiss_request = modifier.on_dismiss_request,
  };
  if (layer_.has_value()) {
    detail::DialogModifierAccess::Update(*service_, *layer_, modifier.content, std::move(options), mounted.environment);
    return;
  }
  layer_ = detail::DialogModifierAccess::Show(*service_, modifier.content, std::move(options), mounted.environment);
}

LayerId ToastHandle::Show(std::string message, ToastOptions options) const {
  return service_->Show(std::move(message), options, environment_);
}

bool ToastHandle::Dismiss(LayerId id) const {
  return service_->Dismiss(id);
}

LayerId ToastService::Show(
    std::string message, ToastOptions options, std::shared_ptr<const detail::EnvironmentFrame> environment
) {
  if (!std::isfinite(options.duration) || options.duration < 0.0) {
    throw std::invalid_argument("HuxerUI toast duration must be finite and non-negative");
  }
  const ToastStyle style = ResolveToastStyle(environment);
  auto id = std::make_shared<LayerId>(0);
  std::weak_ptr<ToastService> service = weak_from_this();
  const LayerId attached = layers_.AttachCaptured(
      LayerOptions{
          .kind = LayerKind::Toast,
          .input_policy = LayerInputPolicy::PassThrough,
      },
      [service, id, message = std::move(message), options, style] {
        return Text(message).With(
            Padding{style.padding},
            Background{style.background},
            Foreground{style.foreground},
            CornerRadius{style.corner_radius},
            ToastLifetime{
                service,
                *id,
                options.duration,
            }
        );
      },
      std::move(environment)
  );
  *id = attached;
  return attached;
}

bool ToastService::Dismiss(LayerId id) {
  return layers_.Dismiss(id);
}

ToastHandle UseToast() {
  return ToastHandle{
      UseService<ToastService>(),
      detail::CurrentEnvironmentFrame(),
  };
}

LayerId DialogHandle::Show(ViewFactory content, DialogOptions options) const {
  return service_->Show(std::move(content), options, environment_);
}

LayerId DialogHandle::Show(DialogFactory content, DialogOptions options) const {
  return service_->Show(std::move(content), options, environment_);
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

LayerId DialogService::Show(
    ViewFactory content, DialogOptions options, std::shared_ptr<const detail::EnvironmentFrame> environment
) {
  const LayerOptions layer_options = DialogLayerOptions(options, environment);
  return layers_.AttachCaptured(layer_options, std::move(content), std::move(environment));
}

LayerId DialogService::Show(
    DialogFactory content, DialogOptions options, std::shared_ptr<const detail::EnvironmentFrame> environment
) {
  if (!content) {
    throw std::invalid_argument("HuxerUI dialog content factory must not be empty");
  }
  auto id = std::make_shared<LayerId>(0);
  const LayerOptions layer_options = DialogLayerOptions(options, environment);
  const LayerId attached = layers_.AttachCaptured(
      layer_options,
      [layers = layers_, id, content = std::move(content)] {
        return content(DialogContext{layers, *id});
      },
      std::move(environment)
  );
  *id = attached;
  return attached;
}

bool DialogService::Update(LayerId id, ViewFactory content) {
  return layers_.Update(id, std::move(content));
}

bool DialogService::Update(
    LayerId id, ViewFactory content, DialogOptions options, std::shared_ptr<const detail::EnvironmentFrame> environment
) {
  return layers_.Update(id, DialogLayerOptions(std::move(options), environment), std::move(content));
}

bool DialogService::Update(LayerId id, DialogFactory content) {
  if (!content) {
    throw std::invalid_argument("HuxerUI dialog content factory must not be empty");
  }
  return layers_.Update(id, [layers = layers_, id, content = std::move(content)] {
    return content(DialogContext{layers, id});
  });
}

bool DialogService::Dismiss(LayerId id) {
  return layers_.Dismiss(id);
}

DialogHandle UseDialog() {
  return DialogHandle{
      UseService<DialogService>(),
      detail::CurrentEnvironmentFrame(),
  };
}

ToastStyle ToastStyle::Default() {
  return DefaultToastStyle(ThemeSpec::Default());
}

DialogStyle DialogStyle::Default() {
  return DefaultDialogStyle(ThemeSpec::Default());
}

const detail::ModifierDescriptor& Dialog::Descriptor() {
  return detail::ModifierDescriptorFor<Dialog, DialogExtension>();
}

} // namespace huxerui
