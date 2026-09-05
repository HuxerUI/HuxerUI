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
#include <huxerui/vector.h>

namespace huxerui {

class Environment;

namespace detail {
class BottomSheetService;
class DialogExtension;
class DialogService;
class LayerAnchorExtension;
class MenuService;
class SystemTrayService;
class PopupService;
class SnackBarService;
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

/// Configures shared enter and exit motion for a presentation surface.
///
/// A neutral scale and zero slide distance produce a fade-only transition. Store std::nullopt in a style's optional
/// motion field to disable transitions entirely.
struct PresentationMotion {
  /// Scale at the hidden endpoint; 1 keeps presentation size unchanged.
  float initial_scale = 1.0F;
  /// Translation distance in DIPs along the presentation's placement direction.
  float slide_distance = 0.0F;
  /// Animation used when the presentation becomes visible.
  AnimationSpec enter = TweenSpec{.duration = 0.2};
  /// Animation used when the presentation is dismissed.
  AnimationSpec exit = TweenSpec{.duration = 0.14};

  bool operator==(const PresentationMotion&) const = default;
};

/// Selects a vertical viewport placement for an unanchored presentation.
enum class VerticalPlacement {
  /// Places content near the top safe-area edge.
  Top,
  /// Centers content vertically.
  Center,
  /// Places content near the bottom safe-area edge.
  Bottom,
};

/// Defines the themed appearance and viewport placement of passive Toast feedback.
struct ToastStyle {
  /// Surface fill color.
  Color background = Color::Rgb(31, 35, 40, 0.94F);
  /// Message typography and foreground color.
  TextStyle text_style{Font::System(14.0F), Color::White()};
  /// Insets between the message and surface edges.
  EdgeInsets padding = EdgeInsets::Symmetric(16.0F, 12.0F);
  /// Surface shadow.
  Shadow shadow{Color::Rgb(0, 0, 0, 0.24F), {}, 10.0F, 0.0F};
  /// Surface corner radius in DIPs.
  float corner_radius = 8.0F;
  /// Minimum surface height in DIPs.
  float minimum_height = 0.0F;
  /// Maximum surface width in DIPs.
  float maximum_width = 480.0F;
  /// Minimum distance from viewport safe-area edges.
  EdgeInsets viewport_padding = EdgeInsets{16.0F, 16.0F, 24.0F, 16.0F};
  /// Vertical viewport placement.
  VerticalPlacement placement = VerticalPlacement::Bottom;
  /// Optional enter and exit motion; std::nullopt disables transitions.
  std::optional<PresentationMotion> motion;

  /// Returns the default Flat Toast appearance.
  static ToastStyle Default();

  bool operator==(const ToastStyle&) const = default;
};

/// Defines the themed appearance of an actionable SnackBar presentation.
///
/// The action remains an ordinary Button; these fields only provide its component-specific appearance.
struct SnackBarStyle {
  /// Surface fill color.
  Color background = Color::Rgb(31, 35, 40);
  /// Message typography and foreground color.
  TextStyle message_style{Font::System(14.0F), Color::White()};
  /// Action label typography and foreground color.
  TextStyle action_text_style{Font::System(14.0F), Color::Rgb(121, 192, 255)};
  /// Action button fill color.
  Color action_background = Color::Transparent();
  /// Insets inside the action button.
  EdgeInsets action_padding = EdgeInsets::Symmetric(12.0F, 8.0F);
  /// Minimum action button height in DIPs.
  float action_minimum_height = 36.0F;
  /// Action button corner radius in DIPs.
  float action_corner_radius = 6.0F;
  /// Optional hover, press, focus, and disabled indication for the action button.
  std::optional<Indication> action_indication;
  /// Insets between SnackBar content and surface edges.
  EdgeInsets padding = EdgeInsets::Symmetric(16.0F, 8.0F);
  /// Spacing between the message and optional action in DIPs.
  float content_spacing = 8.0F;
  /// Surface shadow.
  Shadow shadow{Color::Rgb(0, 0, 0, 0.24F), {}, 10.0F, 0.0F};
  /// Surface corner radius in DIPs.
  float corner_radius = 8.0F;
  /// Minimum surface height in DIPs.
  float minimum_height = 48.0F;
  /// Maximum surface width in DIPs.
  float maximum_width = 600.0F;
  /// Minimum distance from viewport safe-area edges.
  EdgeInsets viewport_padding = EdgeInsets{16.0F, 16.0F, 24.0F, 16.0F};
  /// Optional enter and exit motion; std::nullopt disables transitions.
  std::optional<PresentationMotion> motion;

  /// Returns the default Flat SnackBar appearance.
  static SnackBarStyle Default();

  bool operator==(const SnackBarStyle&) const = default;
};

/// Defines the themed appearance, layout, barrier, and motion of standard Dialog content.
struct DialogStyle {
  /// Full-viewport modal barrier fill color.
  Color scrim = Color::Rgb(0, 0, 0, 0.42F);
  /// Dialog surface fill color.
  Color background = Color::White();
  /// Dialog surface shadow.
  Shadow shadow{Color::Rgb(0, 0, 0, 0.24F), {}, 24.0F, 0.0F};
  /// Title typography and foreground color.
  TextStyle title_style{Font::System(20.0F).WithWeight(FontWeight::Bold), Color::Rgb(31, 35, 40)};
  /// Message typography and foreground color.
  TextStyle message_style{Font::System(14.0F), Color::Rgb(31, 35, 40)};
  /// Positive action typography and foreground color.
  TextStyle positive_action_style{Font::System(14.0F), Color::White()};
  /// Negative action typography and foreground color.
  TextStyle negative_action_style{Font::System(14.0F), Color::Rgb(31, 35, 40)};
  /// Positive action button fill color.
  Color positive_action_background = Color::Rgb(31, 111, 235);
  /// Negative action button fill color.
  Color negative_action_background = Color::Transparent();
  /// Positive action interaction indication.
  Indication positive_action_indication{
      .hover = IndicationLayer{.fill = Color::Rgb(255, 255, 255, 0.1F)},
      .press = IndicationLayer{.fill = Color::Rgb(255, 255, 255, 0.18F)},
  };
  /// Negative action interaction indication.
  Indication negative_action_indication{
      .hover = IndicationLayer{.fill = Color::Rgb(0, 0, 0, 0.06F)},
      .press = IndicationLayer{.fill = Color::Rgb(0, 0, 0, 0.12F)},
  };
  /// Separator color used between the content and action region.
  Color action_separator_color = Color::Rgb(31, 35, 40, 0.12F);
  /// Insets between standard title and message content and the surface edges.
  EdgeInsets content_padding = EdgeInsets::All(24.0F);
  /// Insets inside each standard action button.
  EdgeInsets action_padding = EdgeInsets::Symmetric(14.0F, 8.0F);
  /// Spacing between standard content elements in DIPs.
  float content_spacing = 12.0F;
  /// Spacing between standard action buttons in DIPs.
  float action_spacing = 8.0F;
  /// Thickness of the content-action separator in DIPs; zero disables it.
  float action_separator_thickness = 0.0F;
  /// Standard action button corner radius in DIPs.
  float action_corner_radius = 6.0F;
  /// Minimum standard action button height in DIPs.
  float minimum_action_height = 36.0F;
  /// Dialog surface corner radius in DIPs.
  float corner_radius = 12.0F;
  /// Minimum Dialog width in DIPs.
  float minimum_width = 0.0F;
  /// Maximum Dialog width in DIPs.
  float maximum_width = 480.0F;
  /// Minimum distance from viewport safe-area edges in DIPs.
  float viewport_margin = 24.0F;
  /// Vertical viewport placement.
  VerticalPlacement placement = VerticalPlacement::Center;
  /// Horizontal alignment of standard title and message content.
  HorizontalAlignment content_alignment = HorizontalAlignment::Start;
  /// Axis used to arrange standard action buttons.
  Axis action_layout = Axis::Horizontal;
  /// Horizontal alignment of the standard action region.
  HorizontalAlignment action_alignment = HorizontalAlignment::End;
  /// Optional enter and exit motion; std::nullopt disables transitions.
  std::optional<PresentationMotion> motion = PresentationMotion{};

  /// Returns the default Flat Dialog appearance.
  static DialogStyle Default();

  bool operator==(const DialogStyle&) const = default;
};

/// Defines the themed appearance and motion of a modal BottomSheet surface.
struct BottomSheetStyle {
  /// Full-viewport modal barrier fill color.
  Color scrim = Color::Rgb(0, 0, 0, 0.42F);
  /// BottomSheet surface fill color.
  Color background = Color::White();
  /// BottomSheet surface shadow.
  Shadow shadow{Color::Rgb(0, 0, 0, 0.22F), {}, 18.0F, 0.0F};
  /// Surface corner radii, normally applied only to the top edge.
  CornerRadii corner_radii = CornerRadii::Top(14.0F);
  /// Drag-handle fill color; transparent disables the visible handle.
  Color drag_handle = Color::Transparent();
  /// Drag-handle dimensions in DIPs.
  Size drag_handle_size;
  /// Insets around the drag handle.
  EdgeInsets drag_handle_padding;
  /// Maximum BottomSheet width in DIPs.
  float maximum_width = 640.0F;
  /// Animation used when the BottomSheet becomes visible.
  AnimationSpec enter = TweenSpec{.duration = 0.24};
  /// Animation used when the BottomSheet is dismissed.
  AnimationSpec exit = TweenSpec{.duration = 0.18};

  /// Returns the default Flat BottomSheet appearance.
  static BottomSheetStyle Default();

  bool operator==(const BottomSheetStyle&) const = default;
};

/// Selects where a Menu surface paints item separators.
enum class MenuSeparatorMode {
  /// Paints no separators.
  None,
  /// Paints separators only at MenuSection boundaries.
  BetweenSections,
  /// Paints separators between every adjacent item.
  BetweenItems,
};

/// Defines the themed appearance and sizing of Menu content.
struct MenuStyle {
  /// Menu surface fill color.
  Color background = Color::White();
  /// Default item label color.
  Color foreground = Color::Rgb(31, 35, 40);
  /// Default item icon tint.
  Color icon_tint = Color::Rgb(31, 35, 40);
  /// Item hover, press, focus, and disabled indication.
  Indication item_indication{
      .hover = IndicationLayer{.fill = Color::Rgb(0, 0, 0, 0.06F)},
      .press = IndicationLayer{.fill = Color::Rgb(0, 0, 0, 0.12F)},
  };
  /// Separator color.
  Color separator_color = Color::Rgb(31, 35, 40, 0.12F);
  /// Separator placement policy.
  MenuSeparatorMode separator_mode = MenuSeparatorMode::BetweenItems;
  /// Separator thickness in DIPs.
  float separator_thickness = 1.0F;
  /// Insets applied to each separator.
  EdgeInsets separator_padding;
  /// Insets between Menu items and surface edges.
  EdgeInsets content_padding = EdgeInsets::All(4.0F);
  /// Insets inside each Menu item.
  EdgeInsets item_padding = EdgeInsets::Symmetric(12.0F, 8.0F);
  /// Spacing between an item's icon, label, state, and submenu indicator in DIPs.
  float item_content_spacing = 8.0F;
  /// Item icon size in DIPs.
  float icon_size = 18.0F;
  /// Menu surface shadow.
  Shadow shadow{Color::Rgb(0, 0, 0, 0.2F), {}, 16.0F, 0.0F};
  /// Menu surface corner radius in DIPs.
  float corner_radius = 8.0F;
  /// Minimum Menu width in DIPs.
  float minimum_width = 180.0F;
  /// Minimum item height in DIPs.
  float minimum_item_height = 36.0F;
  /// Optional enter and exit motion; std::nullopt disables transitions.
  std::optional<PresentationMotion> motion;

  /// Returns the default Flat Menu appearance.
  static MenuStyle Default();

  bool operator==(const MenuStyle&) const = default;
};

/// Configures one passive Toast request.
struct ToastOptions {
  /// Visible duration in seconds before automatic dismissal.
  double duration = 2.0;

  bool operator==(const ToastOptions&) const = default;
};

/// Presents independent passive Toast notifications in the current window.
///
/// Each Show() call returns a distinct identifier and does not replace earlier Toasts.
/// @code
/// auto toast = UseToast();
///
/// return Button("Save").OnClick([toast] { toast.Show("Changes saved", ToastOptions{2.5}); });
/// @endcode
class ToastHandle {
public:
  /// Presents a localized message and returns its new LayerId.
  LayerId Show(StringVariant message, ToastOptions options = {}) const;
  /// Dismisses a matching Toast and returns false for a stale identifier.
  bool Dismiss(LayerId id) const;

private:
  ToastHandle(std::shared_ptr<detail::ToastService> service, std::shared_ptr<const Environment> environment)
      : service_(std::move(service)), environment_(std::move(environment)) {}

  std::shared_ptr<detail::ToastService> service_;
  std::shared_ptr<const Environment> environment_;

  friend ToastHandle UseToast();
};

/// Returns the Toast handle installed for the current window and captures the current Environment.
///
/// Reusable functions that call this composition-bound facility should be marked composable.
ToastHandle UseToast();

/// Configures how long a SnackBar remains visible while it is not being interacted with.
struct SnackBarOptions {
  /// Remaining visible time in seconds, or std::nullopt for an indefinite presentation.
  ///
  /// The timer pauses while the surface or action is hovered, while the action is focused or pressed, and while the
  /// application is inactive.
  std::optional<double> duration = 4.0;

  bool operator==(const SnackBarOptions&) const = default;
};

/// Presents one window-scoped SnackBar at a time.
///
/// Showing a newer SnackBar atomically replaces the current request. Its LayerId is distinct, so stale dismissal,
/// timeout, and action paths cannot affect the replacement.
/// @code
/// auto snack_bar = UseSnackBar();
///
/// return Button("Delete").OnClick([snack_bar] {
///   snack_bar.Show("Item deleted", "Undo", RestoreItem);
/// });
/// @endcode
class SnackBarHandle {
public:
  /// Presents a message-only SnackBar and returns its request's LayerId.
  LayerId Show(StringVariant message, SnackBarOptions options = {}) const;

  /// Presents a SnackBar with one action that dismisses before invoking on_action.
  LayerId Show(StringVariant message, StringVariant action, std::function<void()> on_action,
               SnackBarOptions options = {}) const;

  /// Dismisses the matching current request and returns false for stale or disconnected identifiers.
  bool Dismiss(LayerId id) const;

private:
  SnackBarHandle(std::shared_ptr<detail::SnackBarService> service, std::shared_ptr<const Environment> environment)
      : service_(std::move(service)), environment_(std::move(environment)) {}

  std::shared_ptr<detail::SnackBarService> service_;
  std::shared_ptr<const Environment> environment_;

  friend SnackBarHandle UseSnackBar();
};

/// Returns the SnackBar handle installed for the current window and captures the current Environment.
///
/// Reusable functions that call this composition-bound facility should be marked composable.
SnackBarHandle UseSnackBar();

/// Configures dismissal behavior for a Dialog request.
struct DialogOptions {
  /// Whether a pointer press on the modal barrier requests dismissal.
  bool dismiss_on_outside_press = true;
  /// Whether Escape, platform Back, or another cancel request requests dismissal.
  bool dismiss_on_cancel = true;
  /// Optional owner callback that receives dismissal requests instead of automatic removal.
  std::function<void()> on_dismiss_request{};
};

/// Gives custom Dialog content access to the identifier and dismissal operation of its owning request.
class DialogContext {
public:
  /// Returns the owning Dialog's LayerId.
  [[nodiscard]] LayerId Id() const noexcept {
    return id_;
  }

  /// Dismisses the owning Dialog and returns false if it is stale or already exiting.
  bool Dismiss() const {
    return layers_.Dismiss(id_);
  }

private:
  DialogContext(LayerController layers, LayerId id) : layers_(std::move(layers)), id_(id) {}

  LayerController layers_;
  LayerId id_;

  friend class detail::DialogService;
};

/// Produces custom Dialog content with access to its owning request.
using DialogFactory = std::function<View(DialogContext)>;

/// Presents and controls modal Dialog content in the current window.
///
/// Standard overloads build themed title, message, and action content. Factory overloads retain Dialog placement,
/// barrier, focus, and motion policy while allowing application-defined content.
/// @code
/// auto dialogs = UseDialog();
///
/// return Button("Delete").OnClick([dialogs] {
///   dialogs.Show("Delete item?", "This cannot be undone.", "Delete", DeleteItem);
/// });
/// @endcode
class DialogHandle {
public:
  /// Presents a standard Dialog with one optional positive action and returns its LayerId.
  LayerId Show(
      StringVariant title,
      StringVariant message,
      StringVariant positive = {},
      std::function<void()> on_positive_click = {},
      DialogOptions options = {}
  ) const;
  /// Presents a standard Dialog with positive and negative actions and returns its LayerId.
  LayerId Show(
      StringVariant title,
      StringVariant message,
      StringVariant positive,
      StringVariant negative,
      std::function<void()> on_positive_click = {},
      std::function<void()> on_negative_click = {},
      DialogOptions options = {}
  ) const;
  /// Presents custom Dialog content that does not need its owning context.
  LayerId Show(ViewFactory content, DialogOptions options = {}) const;
  /// Presents custom Dialog content with access to its owning context.
  LayerId Show(DialogFactory content, DialogOptions options = {}) const;

  /// Binds copyable factory arguments and presents custom Dialog content.
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

  /// Replaces custom Dialog content and returns false for a stale identifier.
  bool Update(LayerId id, ViewFactory content) const;
  /// Replaces context-aware custom Dialog content and returns false for a stale identifier.
  bool Update(LayerId id, DialogFactory content) const;

  /// Binds copyable factory arguments and replaces custom Dialog content.
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

  /// Dismisses a matching Dialog and returns false if it is stale or already exiting.
  bool Dismiss(LayerId id) const;

private:
  DialogHandle(std::shared_ptr<detail::DialogService> service, std::shared_ptr<const Environment> environment)
      : service_(std::move(service)), environment_(std::move(environment)) {}

  std::shared_ptr<detail::DialogService> service_;
  std::shared_ptr<const Environment> environment_;

  friend DialogHandle UseDialog();
};

/// Returns the Dialog handle installed for the current window and captures the current Environment.
///
/// Reusable functions that call this composition-bound facility should be marked composable.
DialogHandle UseDialog();

/// Configures dismissal behavior for a BottomSheet request.
struct BottomSheetOptions {
  /// Whether a pointer press on the modal barrier requests dismissal.
  bool dismiss_on_outside_press = true;
  /// Whether Escape, platform Back, or another cancel request requests dismissal.
  bool dismiss_on_cancel = true;
  /// Optional owner callback that receives dismissal requests instead of automatic removal.
  std::function<void()> on_dismiss_request;
};

/// Gives custom BottomSheet content access to the identifier and dismissal operation of its owning request.
class BottomSheetContext {
public:
  /// Returns the owning BottomSheet's LayerId.
  [[nodiscard]] LayerId Id() const noexcept {
    return id_;
  }

  /// Dismisses the owning BottomSheet and returns false if it is stale or already exiting.
  bool Dismiss() const {
    return layers_.Dismiss(id_);
  }

private:
  BottomSheetContext(LayerController layers, LayerId id) : layers_(std::move(layers)), id_(id) {}

  LayerController layers_;
  LayerId id_;

  friend class detail::BottomSheetService;
};

/// Produces BottomSheet content with access to its owning request.
using BottomSheetFactory = std::function<View(BottomSheetContext)>;

/// Presents and controls modal bottom-aligned content in the current window.
/// @code
/// auto sheets = UseBottomSheet();
///
/// return Button("Filters").OnClick([sheets] {
///   sheets.Show([](BottomSheetContext sheet) {
///     return Column {
///       FilterControls(),
///       Button("Done").OnClick([sheet] { sheet.Dismiss(); }),
///     };
///   });
/// });
/// @endcode
class BottomSheetHandle {
public:
  /// Presents content that does not need its owning context and returns its LayerId.
  LayerId Show(ViewFactory content, BottomSheetOptions options = {}) const;
  /// Presents content with access to its owning context and returns its LayerId.
  LayerId Show(BottomSheetFactory content, BottomSheetOptions options = {}) const;

  /// Binds copyable factory arguments and presents BottomSheet content.
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

  /// Dismisses a matching BottomSheet and returns false if it is stale or already exiting.
  bool Dismiss(LayerId id) const;

private:
  BottomSheetHandle(std::shared_ptr<detail::BottomSheetService> service, std::shared_ptr<const Environment> environment)
      : service_(std::move(service)), environment_(std::move(environment)) {}

  std::shared_ptr<detail::BottomSheetService> service_;
  std::shared_ptr<const Environment> environment_;

  friend BottomSheetHandle UseBottomSheet();
};

/// Returns the BottomSheet handle installed for the current window and captures the current Environment.
///
/// Reusable functions that call this composition-bound facility should be marked composable.
BottomSheetHandle UseBottomSheet();

/// Selects the preferred side of an anchor for Menu, Popup, and Tooltip placement.
enum class AnchorSide {
  /// Places the presentation below its anchor.
  Below,
  /// Places the presentation above its anchor.
  Above,
  /// Places the presentation to the right of its anchor.
  Right,
  /// Places the presentation to the left of its anchor.
  Left,
};

/// Selects alignment along the cross axis of an anchored presentation.
enum class AnchorAlignment {
  /// Aligns the leading edges.
  Start,
  /// Centers the presentation on the anchor.
  Center,
  /// Aligns the trailing edges.
  End,
};

/// Describes the preferred side and alignment of an anchored presentation.
struct AnchorPlacement {
  /// Preferred side; Runtime may flip it when the requested side does not fit.
  AnchorSide side = AnchorSide::Below;
  /// Alignment along the anchor's cross axis.
  AnchorAlignment alignment = AnchorAlignment::Start;

  bool operator==(const AnchorPlacement&) const = default;
};

/// Defines the themed appearance, placement, and timing of Tooltip content.
struct TooltipStyle {
  /// Tooltip surface fill color.
  Color background = Color::Rgb(31, 35, 40, 0.94F);
  /// Message typography and foreground color.
  TextStyle text_style{Font::System(13.0F), Color::White()};
  /// Insets between the message and surface edges.
  EdgeInsets padding = EdgeInsets::Symmetric(8.0F, 4.0F);
  /// Tooltip surface shadow.
  Shadow shadow{Color::Rgb(0, 0, 0, 0.18F), {}, 6.0F, 0.0F};
  /// Surface corner radius in DIPs.
  float corner_radius = 4.0F;
  /// Minimum Tooltip height in DIPs.
  float minimum_height = 24.0F;
  /// Maximum Tooltip width in DIPs.
  float maximum_width = 320.0F;
  /// Preferred anchor placement.
  AnchorPlacement placement{AnchorSide::Above, AnchorAlignment::Center};
  /// Distance from the anchor in DIPs.
  float gap = 4.0F;
  /// Minimum distance from viewport safe-area edges in DIPs.
  float viewport_margin = 8.0F;
  /// Stationary pointer duration in seconds before showing hover-owned content.
  double hover_delay = 0.5;
  /// Grace period in seconds before a visible Tooltip hides after hover leaves.
  double exit_delay = 0.1;
  /// Touch press duration in seconds required to show the Tooltip.
  double long_press_delay = 0.5;
  /// Visible duration in seconds for a touch-owned Tooltip.
  double touch_show_duration = 1.5;

  /// Returns the default Flat Tooltip appearance.
  static TooltipStyle Default();

  bool operator==(const TooltipStyle&) const = default;
};

/// Configures anchor placement, focus, and dismissal for a Popup request.
struct PopupOptions {
  /// Preferred side and cross-axis alignment.
  AnchorPlacement placement;
  /// Distance from the anchor in DIPs.
  float gap = 4.0F;
  /// Minimum distance from viewport safe-area edges in DIPs.
  float viewport_margin = 8.0F;
  /// Additional translation from the resolved anchored position in DIPs.
  Point offset;
  /// Whether a pointer press outside content requests dismissal.
  bool dismiss_on_outside_press = true;
  /// Whether Escape, platform Back, or another cancel request requests dismissal.
  bool dismiss_on_cancel = true;
  /// Whether keyboard focus is confined to Popup content while it remains visible.
  bool trap_focus = false;
  /// Keeps keyboard focus on the mounted anchor when pointer input targets popup content without its own focus target.
  bool retain_anchor_focus = false;
  /// Optional owner callback that receives dismissal requests instead of automatic removal.
  std::function<void()> on_dismiss_request;
};

/// Configures anchor placement, width, and dismissal for a Menu request.
struct MenuOptions {
  /// Preferred side and cross-axis alignment.
  AnchorPlacement placement;
  /// Distance from the anchor in DIPs.
  float gap = 4.0F;
  /// Minimum distance from viewport safe-area edges in DIPs.
  float viewport_margin = 8.0F;
  /// Additional translation from the resolved anchored position in DIPs.
  Point offset;
  /// Fixed Menu width in DIPs.
  ///
  /// When omitted, the surface uses its widest item's natural width subject to the theme minimum and viewport limits.
  std::optional<float> width;
  /// Whether a pointer press outside content requests dismissal.
  bool dismiss_on_outside_press = true;
  /// Whether Escape, platform Back, or another cancel request requests dismissal.
  bool dismiss_on_cancel = true;
  /// Optional owner callback that receives dismissal requests instead of automatic removal.
  std::function<void()> on_dismiss_request;
};

/// Retained modifier that provides live anchor geometry to one PopupHandle or MenuHandle.
///
/// Obtain this modifier from the same handle that will show the presentation and apply it to the anchor View.
class LayerAnchor {
public:
  /// Returns the retained modifier descriptor used by View::With().
  static const detail::ModifierDescriptor& Descriptor();

private:
  explicit LayerAnchor(std::shared_ptr<detail::LayerAnchorState> state) : state_(std::move(state)) {}

  std::shared_ptr<detail::LayerAnchorState> state_;

  friend class PopupHandle;
  friend class MenuHandle;
  friend class detail::LayerAnchorExtension;
};

/// Gives custom Popup content access to the identifier and dismissal operation of its owning request.
class PopupContext {
public:
  /// Returns the owning Popup's LayerId.
  [[nodiscard]] LayerId Id() const noexcept {
    return id_;
  }

  /// Dismisses the owning Popup and returns false if it is stale or already exiting.
  bool Dismiss() const;

private:
  PopupContext(std::shared_ptr<detail::LayerAnchorState> anchor, LayerId id) : anchor_(std::move(anchor)), id_(id) {}

  std::shared_ptr<detail::LayerAnchorState> anchor_;
  LayerId id_;

  friend class detail::PopupService;
};

/// Produces Popup content with access to its owning request.
using PopupFactory = std::function<View(PopupContext)>;

/// Presents arbitrary content relative to a retained anchor or a window point.
/// @code
/// auto popup = UsePopup();
///
/// return Button("Details")
///     .With(popup.Anchor())
///     .OnClick([popup] { popup.Show([](PopupContext context) { return DetailsCard(context); }); });
/// @endcode
class PopupHandle {
public:
  /// Returns the retained modifier that tracks this handle's anchor View.
  [[nodiscard]] LayerAnchor Anchor() const;
  /// Presents content relative to the tracked anchor and returns its LayerId.
  LayerId Show(ViewFactory content, PopupOptions options = {}) const;
  /// Presents context-aware content relative to the tracked anchor and returns its LayerId.
  LayerId Show(PopupFactory content, PopupOptions options = {}) const;
  /// Presents content relative to node-local bounds associated with the tracked anchor.
  /// The bounds may extend outside the anchor View.
  LayerId ShowAtAnchor(Rect local_anchor, ViewFactory content, PopupOptions options = {}) const;
  /// Presents context-aware content relative to node-local bounds associated with the tracked anchor.
  /// The bounds may extend outside the anchor View.
  LayerId ShowAtAnchor(Rect local_anchor, PopupFactory content, PopupOptions options = {}) const;
  /// Replaces anchored Popup content and returns false for a stale identifier.
  bool Update(LayerId id, ViewFactory content) const;
  /// Replaces context-aware anchored Popup content and returns false for a stale identifier.
  bool Update(LayerId id, PopupFactory content) const;
  /// Repositions a Popup shown by ShowAtAnchor() without replacing its layer or content, or returns false when stale.
  bool UpdateAnchor(LayerId id, Rect local_anchor) const;

  /// Binds copyable factory arguments and presents content relative to the tracked anchor.
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

  /// Binds copyable factory arguments and presents content relative to node-local anchor bounds.
  template <class Factory, class... Arguments>
    requires detail::PresentationFactoryFor<PopupContext, Factory, Arguments...>
  LayerId ShowAtAnchor(Rect local_anchor, Factory&& content, Arguments&&... arguments) const {
    return ShowAtAnchor(
        local_anchor,
        detail::BindPresentationFactory<PopupContext>(
            std::forward<Factory>(content),
            std::forward<Arguments>(arguments)...
        )
    );
  }

  /// Binds copyable factory arguments and replaces anchored Popup content.
  template <class Factory, class... Arguments>
    requires detail::PresentationFactoryFor<PopupContext, Factory, Arguments...>
  bool Update(LayerId id, Factory&& content, Arguments&&... arguments) const {
    return Update(id, detail::BindPresentationFactory<PopupContext>(
                          std::forward<Factory>(content), std::forward<Arguments>(arguments)...));
  }

  /// Presents content relative to a window point and returns its LayerId.
  LayerId ShowAt(Point point, ViewFactory content, PopupOptions options = {}) const;
  /// Presents context-aware content relative to a window point and returns its LayerId.
  LayerId ShowAt(Point point, PopupFactory content, PopupOptions options = {}) const;

  /// Binds copyable factory arguments and presents content relative to a window point.
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

  /// Dismisses a matching Popup and returns false if it is stale or already exiting.
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

/// Returns a Popup handle with a scope-stable anchor and captures the current Environment.
///
/// Reusable functions that call this composition-bound facility should be marked composable.
PopupHandle UsePopup();

class MenuEntry;

/// Marks a logical boundary between adjacent Menu items.
///
/// The active MenuStyle decides whether a MenuSection paints a separator.
struct MenuSection {};

/// Describes one actionable item or submenu in a Menu model.
class MenuItem {
public:
  MenuItem(StringVariant label, std::function<void()> on_item_click);
  MenuItem(ImageVariant icon, StringVariant label, std::function<void()> on_item_click);

  MenuItem(StringVariant label, std::vector<MenuEntry> children);
  MenuItem(ImageVariant icon, StringVariant label, std::vector<MenuEntry> children);

  MenuItem(const MenuItem& other);
  MenuItem(MenuItem&& other) noexcept;
  MenuItem& operator=(const MenuItem& other);
  MenuItem& operator=(MenuItem&& other) noexcept;
  ~MenuItem();

  /// Returns this item with activation and focus eligibility set to enabled.
  MenuItem Enabled(bool enabled) &&;
  /// Returns this item with a visible checked or unchecked state.
  MenuItem Checked(bool checked) &&;
  /// Returns this item with an icon tint that overrides MenuStyle::icon_tint.
  MenuItem IconTint(Color tint) &&;

private:
  using Destination = std::variant<std::function<void()>, std::vector<MenuEntry>>;

  StringVariant label_;
  std::optional<ImageVariant> icon_;
  Destination destination_;
  bool enabled_ = true;
  std::optional<bool> checked_;
  std::optional<Color> icon_tint_;

  friend class MenuEntry;
  friend class detail::MenuService;
  friend class detail::SystemTrayService;
};

/// Stores either a MenuItem or a MenuSection in an ordered Menu model.
class MenuEntry {
public:
  MenuEntry(MenuItem item) : value_(std::move(item)) {}
  MenuEntry(MenuSection section) : value_(section) {}

private:
  std::variant<MenuItem, MenuSection> value_;

  friend class detail::MenuService;
  friend class detail::SystemTrayService;
};

/// Presents a semantic Menu relative to a retained anchor or a window point.
/// @code
/// auto menu = UseMenu();
///
/// return Button("More")
///     .With(menu.Anchor())
///     .OnClick([menu] {
///       menu.Show({MenuItem("Rename", RenameItem), MenuItem("Delete", DeleteItem)});
///     });
/// @endcode
class MenuHandle {
public:
  /// Returns the retained modifier that tracks this handle's anchor View.
  [[nodiscard]] LayerAnchor Anchor() const;
  /// Presents entries relative to the tracked anchor and returns the root Menu LayerId.
  LayerId Show(std::vector<MenuEntry> entries, MenuOptions options = {}) const;
  /// Presents entries relative to a window point and returns the root Menu LayerId.
  LayerId ShowAt(Point point, std::vector<MenuEntry> entries, MenuOptions options = {}) const;
  /// Dismisses the whole matching Menu chain and returns false for a stale identifier.
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

/// Returns a Menu handle with a scope-stable anchor and captures the current Environment.
///
/// Reusable functions that call this composition-bound facility should be marked composable.
MenuHandle UseMenu();

/// Declarative retained modifier for application-controlled modal Dialog content.
///
/// Prefer UseDialog() for command-oriented presentation. Use this modifier when visibility is already controlled by
/// the declaring composition.
/// @code
/// return Stack {
///   PageContent(),
/// }.With(Dialog{
///     .visible = show_details,
///     .content = [] { return DetailsDialog(); },
///     .on_dismiss_request = [show_details] { show_details = false; },
/// });
/// @endcode
struct Dialog {
  /// Whether the Dialog is currently requested.
  bool visible = false;
  /// Factory for custom Dialog content.
  ViewFactory content;
  /// Whether a pointer press on the modal barrier requests dismissal.
  bool dismiss_on_outside_press = false;
  /// Whether Escape, platform Back, or another cancel request requests dismissal.
  bool dismiss_on_cancel = false;
  /// Owner callback that updates the controlled visible value.
  std::function<void()> on_dismiss_request;

  /// Returns the retained modifier descriptor used by View::With().
  static const detail::ModifierDescriptor& Descriptor();
};

/// Retained modifier that presents localized explanatory text for hover, focus, and long press.
/// @code
/// return IconButton(InfoIcon()).With(Tooltip("More information"));
/// @endcode
struct Tooltip {
  explicit Tooltip(StringVariant message);

  /// Returns the retained modifier descriptor used by View::With().
  static const detail::ModifierDescriptor& Descriptor();

  /// Localized Tooltip message.
  StringVariant message;

  bool operator==(const Tooltip&) const = default;
};

} // namespace huxerui
