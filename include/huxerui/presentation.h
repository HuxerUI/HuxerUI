#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/animation.h>
#include <huxerui/color.h>
#include <huxerui/geometry.h>
#include <huxerui/layer.h>
#include <huxerui/modifier.h>
#include <huxerui/resource.h>

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
  AnimationSpec enter = TweenSpec{.duration = 0.2};
  AnimationSpec exit = TweenSpec{.duration = 0.14};

  static DialogStyle Default();

  bool operator==(const DialogStyle&) const = default;
};

struct BottomSheetStyle {
  Color scrim = Color::Rgb(0, 0, 0, 0.42F);
  Color background = Color::White();
  Shadow shadow{Color::Rgb(0, 0, 0, 0.22F), {0.0F, -2.0F}, 18.0F, 0.0F};
  float corner_radius = 14.0F;
  float maximum_width = 640.0F;
  AnimationSpec enter = TweenSpec{.duration = 0.24};
  AnimationSpec exit = TweenSpec{.duration = 0.18};

  static BottomSheetStyle Default();

  bool operator==(const BottomSheetStyle&) const = default;
};

enum class MenuSeparatorMode {
  None,
  BetweenSections,
  BetweenItems,
};

struct MenuStyle {
  Color background = Color::White();
  Color foreground = Color::Rgb(31, 35, 40);
  Color separator_color = Color::Rgb(31, 35, 40, 0.12F);
  MenuSeparatorMode separator_mode = MenuSeparatorMode::BetweenItems;
  float separator_thickness = 1.0F;
  EdgeInsets separator_padding;
  EdgeInsets content_padding = EdgeInsets::All(4.0F);
  EdgeInsets item_padding = EdgeInsets::Symmetric(12.0F, 8.0F);
  float item_content_spacing = 8.0F;
  float icon_size = 18.0F;
  Shadow shadow{Color::Rgb(0, 0, 0, 0.2F), {0.0F, 4.0F}, 16.0F, 0.0F};
  float corner_radius = 8.0F;
  float minimum_width = 180.0F;
  float minimum_item_height = 36.0F;

  static MenuStyle Default();

  bool operator==(const MenuStyle&) const = default;
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

enum class AnchorSide {
  Below,
  Above,
  Right,
  Left,
};

enum class AnchorAlignment {
  Start,
  Center,
  End,
};

struct AnchorPlacement {
  AnchorSide side = AnchorSide::Below;
  AnchorAlignment alignment = AnchorAlignment::Start;

  bool operator==(const AnchorPlacement&) const = default;
};

struct PopupOptions {
  AnchorPlacement placement;
  float gap = 4.0F;
  float viewport_margin = 8.0F;
  Point offset;
  bool dismiss_on_outside_press = true;
  bool dismiss_on_cancel = true;
  bool trap_focus = false;
  std::function<void()> on_dismiss_request;
};

struct MenuOptions {
  AnchorPlacement placement;
  float gap = 4.0F;
  float viewport_margin = 8.0F;
  Point offset;
  // When omitted, the surface uses its widest item's natural width subject to the theme minimum and viewport limits.
  std::optional<float> width;
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

class MenuEntry;

// Marks a logical boundary between adjacent items; the active MenuStyle decides whether it paints a separator.
struct MenuSection {};

class MenuItem {
public:
  MenuItem(std::string_view label, std::function<void()> on_item_click);
  MenuItem(StringResource label, std::function<void()> on_item_click);
  MenuItem(ImageResource icon, std::string_view label, std::function<void()> on_item_click);
  MenuItem(ImageResource icon, StringResource label, std::function<void()> on_item_click);
  MenuItem(ImageAsset icon, std::string_view label, std::function<void()> on_item_click);
  MenuItem(ImageAsset icon, StringResource label, std::function<void()> on_item_click);

  MenuItem(std::string_view label, std::vector<MenuEntry> children);
  MenuItem(StringResource label, std::vector<MenuEntry> children);
  MenuItem(ImageResource icon, std::string_view label, std::vector<MenuEntry> children);
  MenuItem(ImageResource icon, StringResource label, std::vector<MenuEntry> children);
  MenuItem(ImageAsset icon, std::string_view label, std::vector<MenuEntry> children);
  MenuItem(ImageAsset icon, StringResource label, std::vector<MenuEntry> children);

  MenuItem(const MenuItem& other);
  MenuItem(MenuItem&& other) noexcept;
  MenuItem& operator=(const MenuItem& other);
  MenuItem& operator=(MenuItem&& other) noexcept;
  ~MenuItem();

  MenuItem Enabled(bool enabled) &&;
  MenuItem Checked(bool checked) &&;

private:
  using Label = std::variant<std::string, StringResource>;
  using Icon = std::variant<std::monostate, ImageResource, ImageAsset>;
  using Destination = std::variant<std::function<void()>, std::vector<MenuEntry>>;

  MenuItem(Label label, Icon icon, std::function<void()> on_item_click);
  MenuItem(Label label, Icon icon, std::vector<MenuEntry> children);

  Label label_;
  Icon icon_;
  Destination destination_;
  bool enabled_ = true;
  bool checked_ = false;

  friend class MenuEntry;
  friend class detail::MenuService;
};

class MenuEntry {
public:
  MenuEntry(MenuItem item) : value_(std::move(item)) {}
  MenuEntry(MenuSection section) : value_(section) {}

private:
  std::variant<MenuItem, MenuSection> value_;

  friend class detail::MenuService;
};

class MenuHandle {
public:
  [[nodiscard]] LayerAnchor Anchor() const;
  LayerId Show(std::vector<MenuEntry> entries, MenuOptions options = {}) const;
  LayerId ShowAt(Point point, std::vector<MenuEntry> entries, MenuOptions options = {}) const;
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
  friend class detail::MenuService;
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
