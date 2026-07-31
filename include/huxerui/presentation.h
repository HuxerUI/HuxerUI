#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <huxerui/modifier.h>
#include <huxerui/root.h>

namespace huxerui {

namespace detail {
struct DialogModifierAccess;
}

struct ToastStyle {
  Color background = Color::Rgb(31, 35, 40, 0.94F);
  Color foreground = Color::White();
  float padding = 12.0F;
  float corner_radius = 8.0F;

  static ToastStyle Default();

  bool operator==(const ToastStyle&) const = default;
};

struct DialogStyle {
  Color scrim = Color::Rgb(0, 0, 0, 0.42F);

  static DialogStyle Default();

  bool operator==(const DialogStyle&) const = default;
};

struct ToastOptions {
  double duration = 2.0;

  bool operator==(const ToastOptions&) const = default;
};

class ToastService;

class ToastHandle {
public:
  LayerId Show(std::string message, ToastOptions options = {}) const;
  bool Dismiss(LayerId id) const;

private:
  ToastHandle(std::shared_ptr<ToastService> service, std::shared_ptr<const detail::EnvironmentFrame> environment)
      : service_(std::move(service)), environment_(std::move(environment)) {}

  std::shared_ptr<ToastService> service_;
  std::shared_ptr<const detail::EnvironmentFrame> environment_;

  friend ToastHandle UseToast();
};

class ToastService : public std::enable_shared_from_this<ToastService> {
public:
  explicit ToastService(LayerController& layers) : layers_(layers) {}

  bool Dismiss(LayerId id);

private:
  LayerId Show(std::string message, ToastOptions options, std::shared_ptr<const detail::EnvironmentFrame> environment);

  LayerController layers_;

  friend class ToastHandle;
};

ToastHandle UseToast();

struct DialogOptions {
  bool dismiss_on_outside_press = true;
  std::function<void()> on_dismiss_request;
};

class DialogService;

class DialogContext {
public:
  [[nodiscard]] LayerId Id() const noexcept {
    return id_;
  }

  bool Dismiss() const {
    return layers_.Dismiss(id_);
  }

private:
  DialogContext(LayerController layers, LayerId id) : layers_(std::move(layers)), id_(id) {}

  LayerController layers_;
  LayerId id_;

  friend class DialogService;
};

using DialogFactory = std::function<View(DialogContext)>;

class DialogHandle {
public:
  LayerId Show(ViewFactory content, DialogOptions options = {}) const;
  LayerId Show(DialogFactory content, DialogOptions options = {}) const;
  bool Update(LayerId id, ViewFactory content) const;
  bool Update(LayerId id, DialogFactory content) const;
  bool Dismiss(LayerId id) const;

private:
  DialogHandle(std::shared_ptr<DialogService> service, std::shared_ptr<const detail::EnvironmentFrame> environment)
      : service_(std::move(service)), environment_(std::move(environment)) {}

  std::shared_ptr<DialogService> service_;
  std::shared_ptr<const detail::EnvironmentFrame> environment_;

  friend DialogHandle UseDialog();
};

class DialogService {
public:
  explicit DialogService(LayerController& layers) : layers_(layers) {}

  bool Update(LayerId id, ViewFactory content);
  bool Update(LayerId id, DialogFactory content);
  bool Dismiss(LayerId id);

private:
  LayerId Show(ViewFactory content, DialogOptions options, std::shared_ptr<const detail::EnvironmentFrame> environment);
  LayerId
  Show(DialogFactory content, DialogOptions options, std::shared_ptr<const detail::EnvironmentFrame> environment);
  bool Update(
      LayerId id,
      ViewFactory content,
      DialogOptions options,
      std::shared_ptr<const detail::EnvironmentFrame> environment
  );

  LayerController layers_;

  friend class DialogHandle;
  friend struct detail::DialogModifierAccess;
};

DialogHandle UseDialog();

struct Dialog {
  bool visible = false;
  ViewFactory content;
  bool dismiss_on_outside_press = false;
  std::function<void()> on_dismiss_request;

  static const detail::ModifierDescriptor& Descriptor();
};

} // namespace huxerui
