#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <huxerui/color.h>
#include <huxerui/geometry.h>
#include <huxerui/layer.h>
#include <huxerui/modifier.h>

namespace huxerui {

class Environment;

namespace detail {
class BottomSheetService;
class DialogExtension;
class DialogService;
class LayerAnchorExtension;
class MenuService;
class PopupService;
class ToastService;
struct LayerAnchorState;
} // namespace detail

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

class ToastHandle {
public:
  LayerId Show(std::string message, ToastOptions options = {}) const;
  bool Dismiss(LayerId id) const;

private:
  ToastHandle(std::shared_ptr<detail::ToastService> service, std::shared_ptr<const Environment> environment)
      : service_(std::move(service)), environment_(std::move(environment)) {}

  std::shared_ptr<detail::ToastService> service_;
  std::shared_ptr<const Environment> environment_;

  friend ToastHandle UseToast();
};

ToastHandle UseToast();

struct DialogOptions {
  bool dismiss_on_outside_press = true;
  bool dismiss_on_cancel = true;
  std::function<void()> on_dismiss_request;
};

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

  friend class detail::DialogService;
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
  DialogHandle(std::shared_ptr<detail::DialogService> service, std::shared_ptr<const Environment> environment)
      : service_(std::move(service)), environment_(std::move(environment)) {}

  std::shared_ptr<detail::DialogService> service_;
  std::shared_ptr<const Environment> environment_;

  friend DialogHandle UseDialog();
};

DialogHandle UseDialog();

struct BottomSheetOptions {
  bool dismiss_on_outside_press = true;
  bool dismiss_on_cancel = true;
  std::function<void()> on_dismiss_request;
};

class BottomSheetContext {
public:
  [[nodiscard]] LayerId Id() const noexcept {
    return id_;
  }

  bool Dismiss() const {
    return layers_.Dismiss(id_);
  }

private:
  BottomSheetContext(LayerController layers, LayerId id) : layers_(std::move(layers)), id_(id) {}

  LayerController layers_;
  LayerId id_;

  friend class detail::BottomSheetService;
};

using BottomSheetFactory = std::function<View(BottomSheetContext)>;

class BottomSheetHandle {
public:
  LayerId Show(ViewFactory content, BottomSheetOptions options = {}) const;
  LayerId Show(BottomSheetFactory content, BottomSheetOptions options = {}) const;
  bool Dismiss(LayerId id) const;

private:
  BottomSheetHandle(std::shared_ptr<detail::BottomSheetService> service, std::shared_ptr<const Environment> environment)
      : service_(std::move(service)), environment_(std::move(environment)) {}

  std::shared_ptr<detail::BottomSheetService> service_;
  std::shared_ptr<const Environment> environment_;

  friend BottomSheetHandle UseBottomSheet();
};

BottomSheetHandle UseBottomSheet();

enum class AnchorPlacement {
  Below,
  Above,
  Right,
  Left,
};

struct PopupOptions {
  AnchorPlacement placement = AnchorPlacement::Below;
  float gap = 4.0F;
  float viewport_margin = 8.0F;
  bool dismiss_on_outside_press = true;
  bool dismiss_on_cancel = true;
  bool trap_focus = false;
  std::function<void()> on_dismiss_request;
};

struct MenuOptions {
  AnchorPlacement placement = AnchorPlacement::Below;
  float gap = 4.0F;
  float viewport_margin = 8.0F;
  bool dismiss_on_outside_press = true;
  bool dismiss_on_cancel = true;
  std::function<void()> on_dismiss_request;
};

class LayerAnchor {
public:
  static const detail::ModifierDescriptor& Descriptor();

private:
  explicit LayerAnchor(std::shared_ptr<detail::LayerAnchorState> state) : state_(std::move(state)) {}

  std::shared_ptr<detail::LayerAnchorState> state_;

  friend class PopupHandle;
  friend class MenuHandle;
  friend class detail::LayerAnchorExtension;
};

class PopupContext {
public:
  [[nodiscard]] LayerId Id() const noexcept {
    return id_;
  }

  bool Dismiss() const;

private:
  PopupContext(std::shared_ptr<detail::LayerAnchorState> anchor, LayerId id) : anchor_(std::move(anchor)), id_(id) {}

  std::shared_ptr<detail::LayerAnchorState> anchor_;
  LayerId id_;

  friend class detail::PopupService;
};

using PopupFactory = std::function<View(PopupContext)>;

class PopupHandle {
public:
  [[nodiscard]] LayerAnchor Anchor() const;
  LayerId Show(ViewFactory content, PopupOptions options = {}) const;
  LayerId Show(PopupFactory content, PopupOptions options = {}) const;
  LayerId ShowAt(Point point, ViewFactory content, PopupOptions options = {}) const;
  LayerId ShowAt(Point point, PopupFactory content, PopupOptions options = {}) const;
  bool Dismiss(LayerId id) const;

private:
  PopupHandle(
      std::shared_ptr<detail::PopupService> service,
      std::shared_ptr<const Environment> environment,
      std::shared_ptr<detail::LayerAnchorState> anchor
  )
      : service_(std::move(service)), environment_(std::move(environment)), anchor_(std::move(anchor)) {}

  std::shared_ptr<detail::PopupService> service_;
  std::shared_ptr<const Environment> environment_;
  std::shared_ptr<detail::LayerAnchorState> anchor_;

  friend PopupHandle UsePopup();
};

PopupHandle UsePopup();

class MenuContext {
public:
  [[nodiscard]] LayerId Id() const noexcept {
    return id_;
  }

  bool Dismiss() const;

private:
  MenuContext(std::shared_ptr<detail::LayerAnchorState> anchor, LayerId id) : anchor_(std::move(anchor)), id_(id) {}

  std::shared_ptr<detail::LayerAnchorState> anchor_;
  LayerId id_;

  friend class detail::MenuService;
};

using MenuFactory = std::function<View(MenuContext)>;

class MenuHandle {
public:
  [[nodiscard]] LayerAnchor Anchor() const;
  LayerId Show(ViewFactory content, MenuOptions options = {}) const;
  LayerId Show(MenuFactory content, MenuOptions options = {}) const;
  LayerId ShowAt(Point point, ViewFactory content, MenuOptions options = {}) const;
  LayerId ShowAt(Point point, MenuFactory content, MenuOptions options = {}) const;
  bool Dismiss(LayerId id) const;

private:
  MenuHandle(
      std::shared_ptr<detail::MenuService> service,
      std::shared_ptr<const Environment> environment,
      std::shared_ptr<detail::LayerAnchorState> anchor
  )
      : service_(std::move(service)), environment_(std::move(environment)), anchor_(std::move(anchor)) {}

  std::shared_ptr<detail::MenuService> service_;
  std::shared_ptr<const Environment> environment_;
  std::shared_ptr<detail::LayerAnchorState> anchor_;

  friend MenuHandle UseMenu();
};

MenuHandle UseMenu();

struct Dialog {
  bool visible = false;
  ViewFactory content;
  bool dismiss_on_outside_press = false;
  bool dismiss_on_cancel = false;
  std::function<void()> on_dismiss_request;

  static const detail::ModifierDescriptor& Descriptor();
};

} // namespace huxerui
