#pragma once

#include <concepts>
#include <functional>
#include <memory>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/animation.h>
#include <huxerui/color.h>
#include <huxerui/geometry.h>
#include <huxerui/indication.h>
#include <huxerui/layer.h>
#include <huxerui/layout.h>
#include <huxerui/modifier.h>
#include <huxerui/resource.h>
#include <huxerui/text.h>

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

template <class Context, class Factory, class... Arguments>
concept PresentationFactoryFor =
    ViewFactoryFor<Factory, Arguments...> ||
    (std::copy_constructible<std::decay_t<Factory>> && (std::copy_constructible<std::decay_t<Arguments>> && ...) &&
     requires(std::decay_t<Factory>& factory, Context& context, const std::decay_t<Arguments>&... arguments) {
       { std::invoke(factory, context, arguments...) } -> std::convertible_to<View>;
     });

template <class Context, class Factory, class... Arguments>
  requires PresentationFactoryFor<Context, Factory, Arguments...>
auto BindPresentationFactory(Factory&& factory, Arguments&&... arguments) {
  if constexpr (ViewFactoryFor<Factory, Arguments...>) {
    return BindViewFactory(std::forward<Factory>(factory), std::forward<Arguments>(arguments)...);
  } else {
    using StoredFactory = std::decay_t<Factory>;
    using StoredArguments = std::tuple<std::decay_t<Arguments>...>;
    StoredFactory stored_factory(std::forward<Factory>(factory));
    StoredArguments stored_arguments(std::forward<Arguments>(arguments)...);
    return std::function<View(Context)>{
        [factory = std::move(stored_factory),
         arguments = std::move(stored_arguments)](Context context) mutable -> View {
          return std::apply(
              [&factory, &context](const auto&... values) -> View { return std::invoke(factory, context, values...); },
              arguments
          );
        },
    };
  }
}
} // namespace detail

struct PresentationMotion {
  float initial_scale = 1.0F;
  float slide_distance = 0.0F;
  AnimationSpec enter = TweenSpec{.duration = 0.2};
  AnimationSpec exit = TweenSpec{.duration = 0.14};

  bool operator==(const PresentationMotion&) const = default;
};

enum class VerticalPlacement {
  Top,
  Center,
  Bottom,
};

struct ToastStyle {
  Color background = Color::Rgb(31, 35, 40, 0.94F);
  TextStyle text_style{Font::System(14.0F), Color::White()};
  EdgeInsets padding = EdgeInsets::Symmetric(16.0F, 12.0F);
  Shadow shadow{Color::Rgb(0, 0, 0, 0.24F), {}, 10.0F, 0.0F};
  float corner_radius = 8.0F;
  float minimum_height = 0.0F;
  float maximum_width = 480.0F;
  EdgeInsets viewport_padding = EdgeInsets{16.0F, 16.0F, 24.0F, 16.0F};
  VerticalPlacement placement = VerticalPlacement::Bottom;
  std::optional<PresentationMotion> motion;

  static ToastStyle Default();

  bool operator==(const ToastStyle&) const = default;
};

struct DialogStyle {
  Color scrim = Color::Rgb(0, 0, 0, 0.42F);
  Color background = Color::White();
  Shadow shadow{Color::Rgb(0, 0, 0, 0.24F), {}, 24.0F, 0.0F};
  TextStyle title_style{Font::System(20.0F).WithWeight(FontWeight::Bold), Color::Rgb(31, 35, 40)};
  TextStyle message_style{Font::System(14.0F), Color::Rgb(31, 35, 40)};
  TextStyle positive_action_style{Font::System(14.0F), Color::White()};
  TextStyle negative_action_style{Font::System(14.0F), Color::Rgb(31, 35, 40)};
  Color positive_action_background = Color::Rgb(31, 111, 235);
  Color negative_action_background = Color::Transparent();
  IndicationSpec positive_action_indication = StateOverlayIndication{
      .color = Color::Rgb(255, 255, 255, 0.18F),
      .hover_color = Color::Rgb(255, 255, 255, 0.1F),
  };
  IndicationSpec negative_action_indication = StateOverlayIndication{};
  Color action_separator_color = Color::Rgb(31, 35, 40, 0.12F);
  EdgeInsets content_padding = EdgeInsets::All(24.0F);
  EdgeInsets action_padding = EdgeInsets::Symmetric(14.0F, 8.0F);
  float content_spacing = 12.0F;
  float action_spacing = 8.0F;
  float action_separator_thickness = 0.0F;
  float action_corner_radius = 6.0F;
  float minimum_action_height = 36.0F;
  float corner_radius = 12.0F;
  float minimum_width = 0.0F;
  float maximum_width = 480.0F;
  float viewport_margin = 24.0F;
  VerticalPlacement placement = VerticalPlacement::Center;
  HorizontalAlignment content_alignment = HorizontalAlignment::Start;
  Axis action_layout = Axis::Horizontal;
  HorizontalAlignment action_alignment = HorizontalAlignment::End;
  std::optional<PresentationMotion> motion = PresentationMotion{};

  static DialogStyle Default();

  bool operator==(const DialogStyle&) const = default;
};

struct BottomSheetStyle {
  Color scrim = Color::Rgb(0, 0, 0, 0.42F);
  Color background = Color::White();
  Shadow shadow{Color::Rgb(0, 0, 0, 0.22F), {}, 18.0F, 0.0F};
  CornerRadii corner_radii = CornerRadii::Top(14.0F);
  Color drag_handle = Color::Transparent();
  Size drag_handle_size;
  EdgeInsets drag_handle_padding;
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
  IndicationSpec item_indication = StateOverlayIndication{};
  Color separator_color = Color::Rgb(31, 35, 40, 0.12F);
  MenuSeparatorMode separator_mode = MenuSeparatorMode::BetweenItems;
  float separator_thickness = 1.0F;
  EdgeInsets separator_padding;
  EdgeInsets content_padding = EdgeInsets::All(4.0F);
  EdgeInsets item_padding = EdgeInsets::Symmetric(12.0F, 8.0F);
  float item_content_spacing = 8.0F;
  float icon_size = 18.0F;
  Shadow shadow{Color::Rgb(0, 0, 0, 0.2F), {}, 16.0F, 0.0F};
  float corner_radius = 8.0F;
  float minimum_width = 180.0F;
  float minimum_item_height = 36.0F;
  std::optional<PresentationMotion> motion;

  static MenuStyle Default();

  bool operator==(const MenuStyle&) const = default;
};

struct ToastOptions {
  double duration = 2.0;

  bool operator==(const ToastOptions&) const = default;
};

class ToastHandle {
public:
  LayerId Show(StringVariant message, ToastOptions options = {}) const;
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
  LayerId Show(
      StringVariant title,
      StringVariant message,
      StringVariant positive = {},
      std::function<void()> on_positive_click = {},
      DialogOptions options = {}
  ) const;
  LayerId Show(
      StringVariant title,
      StringVariant message,
      StringVariant positive,
      StringVariant negative,
      std::function<void()> on_positive_click = {},
      std::function<void()> on_negative_click = {},
      DialogOptions options = {}
  ) const;
  LayerId Show(ViewFactory content, DialogOptions options = {}) const;
  LayerId Show(DialogFactory content, DialogOptions options = {}) const;

  template <class Factory, class... Arguments>
    requires detail::PresentationFactoryFor<DialogContext, Factory, Arguments...>
  LayerId Show(Factory&& content, Arguments&&... arguments) const {
    return Show(
        detail::BindPresentationFactory<DialogContext>(
            std::forward<Factory>(content),
            std::forward<Arguments>(arguments)...
        )
    );
  }

  bool Update(LayerId id, ViewFactory content) const;
  bool Update(LayerId id, DialogFactory content) const;

  template <class Factory, class... Arguments>
    requires detail::PresentationFactoryFor<DialogContext, Factory, Arguments...>
  bool Update(LayerId id, Factory&& content, Arguments&&... arguments) const {
    return Update(
        id,
        detail::BindPresentationFactory<DialogContext>(
            std::forward<Factory>(content),
            std::forward<Arguments>(arguments)...
        )
    );
  }

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

  template <class Factory, class... Arguments>
    requires detail::PresentationFactoryFor<BottomSheetContext, Factory, Arguments...>
  LayerId Show(Factory&& content, Arguments&&... arguments) const {
    return Show(
        detail::BindPresentationFactory<BottomSheetContext>(
            std::forward<Factory>(content),
            std::forward<Arguments>(arguments)...
        )
    );
  }

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

struct TooltipStyle {
  Color background = Color::Rgb(31, 35, 40, 0.94F);
  TextStyle text_style{Font::System(13.0F), Color::White()};
  EdgeInsets padding = EdgeInsets::Symmetric(8.0F, 4.0F);
  Shadow shadow{Color::Rgb(0, 0, 0, 0.18F), {}, 6.0F, 0.0F};
  float corner_radius = 4.0F;
  float minimum_height = 24.0F;
  float maximum_width = 320.0F;
  AnchorPlacement placement{AnchorSide::Above, AnchorAlignment::Center};
  float gap = 4.0F;
  float viewport_margin = 8.0F;
  double hover_delay = 0.5;
  double exit_delay = 0.1;
  double long_press_delay = 0.5;
  double touch_show_duration = 1.5;

  static TooltipStyle Default();

  bool operator==(const TooltipStyle&) const = default;
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

  template <class Factory, class... Arguments>
    requires detail::PresentationFactoryFor<PopupContext, Factory, Arguments...>
  LayerId Show(Factory&& content, Arguments&&... arguments) const {
    return Show(
        detail::BindPresentationFactory<PopupContext>(
            std::forward<Factory>(content),
            std::forward<Arguments>(arguments)...
        )
    );
  }

  LayerId ShowAt(Point point, ViewFactory content, PopupOptions options = {}) const;
  LayerId ShowAt(Point point, PopupFactory content, PopupOptions options = {}) const;

  template <class Factory, class... Arguments>
    requires detail::PresentationFactoryFor<PopupContext, Factory, Arguments...>
  LayerId ShowAt(Point point, Factory&& content, Arguments&&... arguments) const {
    return ShowAt(
        point,
        detail::BindPresentationFactory<PopupContext>(
            std::forward<Factory>(content),
            std::forward<Arguments>(arguments)...
        )
    );
  }

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
  MenuItem(StringVariant label, std::function<void()> on_item_click);
  MenuItem(ImageResource icon, StringVariant label, std::function<void()> on_item_click);
  MenuItem(ImageAsset icon, StringVariant label, std::function<void()> on_item_click);

  MenuItem(StringVariant label, std::vector<MenuEntry> children);
  MenuItem(ImageResource icon, StringVariant label, std::vector<MenuEntry> children);
  MenuItem(ImageAsset icon, StringVariant label, std::vector<MenuEntry> children);

  MenuItem(const MenuItem& other);
  MenuItem(MenuItem&& other) noexcept;
  MenuItem& operator=(const MenuItem& other);
  MenuItem& operator=(MenuItem&& other) noexcept;
  ~MenuItem();

  MenuItem Enabled(bool enabled) &&;
  MenuItem Checked(bool checked) &&;

private:
  using Icon = std::variant<std::monostate, ImageResource, ImageAsset>;
  using Destination = std::variant<std::function<void()>, std::vector<MenuEntry>>;

  MenuItem(StringVariant label, Icon icon, std::function<void()> on_item_click);
  MenuItem(StringVariant label, Icon icon, std::vector<MenuEntry> children);

  StringVariant label_;
  Icon icon_;
  Destination destination_;
  bool enabled_ = true;
  std::optional<bool> checked_;

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

struct Tooltip {
  explicit Tooltip(StringVariant message);

  static const detail::ModifierDescriptor& Descriptor();

  StringVariant message;

  bool operator==(const Tooltip&) const = default;
};

} // namespace huxerui
