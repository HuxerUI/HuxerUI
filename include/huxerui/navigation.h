#pragma once

/// @file
/// Controlled navigation chrome, responsive drawers, and factory or routed page stacks.

#include <concepts>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/animation.h>
#include <huxerui/color.h>
#include <huxerui/geometry.h>
#include <huxerui/indication.h>
#include <huxerui/modifier.h>
#include <huxerui/resource.h>
#include <huxerui/state.h>
#include <huxerui/text.h>
#include <huxerui/vector.h>
#include <huxerui/view.h>

namespace huxerui {

namespace detail {
class NavigationState;
struct InternalAccess;

template <class Route>
concept NavigationRouteValue = std::copy_constructible<Route> && std::equality_comparable<Route>;

struct NavigationRouteDescriptor {
  std::shared_ptr<const void> value;
  bool (*equals)(const void* first, const void* second) = nullptr;
  std::function<View()> factory;
};

// Logical history intent is independent from the retained page transitions resolved by NavigationState.
enum class NavigationHistoryAction {
  Push,
  Pop,
  Replace,
};

struct NavigationRouteBinding {
  std::type_index route_type{typeid(void)};
  std::shared_ptr<void> path_state;
  std::shared_ptr<void> history_commit;
  std::function<bool()> request_pop;
};

struct NavigationAccess {
  std::weak_ptr<NavigationState> state;
  std::shared_ptr<void> path_state;
  std::shared_ptr<void> history_commit;
};

View BuildRoutedNavigationStack(
    std::function<View()> root, std::vector<NavigationRouteDescriptor> routes, NavigationRouteBinding binding
);
NavigationAccess FindNavigationAccess(std::optional<std::type_index> route_type, bool outermost);
bool CheckNavigationAccess(const std::weak_ptr<NavigationState>& state);
} // namespace detail

/// Selects all page transition operations through one root modifier.
///
/// The complete policy overrides NavigationStyle::motion; default members finish immediately. The selected value is
/// frozen for an operation. Declare it on the page root, including transparent Scope or Environment boundaries.
/// @code
/// const TransitionSpec enter{SlideTransition{}, TweenSpec{0.28}};
/// return Text("Details").With(PageTransition{.push = enter, .pop = enter.Reversed(), .replace = enter});
/// @endcode
/// Page content is disabled for interaction during an active operation and restored when it ends.
/// Container-level Back and predictive cancellation remain available; visual fragments add no input targets.
struct PageTransition {
  /// Effect selected from the incoming page for Push.
  TransitionSpec push;
  /// Effect selected from the departing page for Pop, including predictive Back.
  TransitionSpec pop;
  /// Effect selected from the incoming page for Replace.
  TransitionSpec replace;

  /// Returns the root modifier descriptor used by View::With().
  /// @return The framework descriptor; applications normally attach the value with With().
  static const detail::ModifierDescriptor& Descriptor();
  bool operator==(const PageTransition&) const = default;
};

/// Supplies inherited default transition policy to NavigationStack.
///
/// A default-constructed style has no motion; Default() provides the generic slide policy.
/// @code
/// NavigationStyle style = NavigationStyle::Default();
/// style.motion = PageTransition{};
/// @endcode
struct NavigationStyle {
  /// Default complete page policy. An absent value disables page motion unless the page supplies PageTransition.
  std::optional<PageTransition> motion;

  /// Returns a generic slide policy for Push, Pop, and Replace.
  /// @return A standalone default; active themes may supply different effects and timing.
  static NavigationStyle Default();

  bool operator==(const NavigationStyle&) const = default;
};

/// Selects title placement within the available TopAppBar title region.
enum class TopAppBarTitleAlignment {
  /// Aligns the title after leading content, or at the configured start inset.
  Start,
  /// Centers the title where possible while keeping leading content and actions unobscured.
  Center,
};

/// Configures TopAppBar typography, color, and logical spacing through the active Environment.
/// @code
/// TopAppBarStyle style = TopAppBarStyle::Default();
/// style.height = 56.0F;
/// @endcode
struct TopAppBarStyle {
  /// Background fill of the complete app bar.
  Color background = Color::White();
  /// Typography and color of the title.
  TextStyle title_style{Font::System(20.0F).WithWeight(FontWeight::Bold), Color::Rgb(31, 35, 40)};
  /// Preferred bar height in logical units.
  float height = 48.0F;
  /// Horizontal inset of leading content and actions in logical units.
  float horizontal_padding = 8.0F;
  /// Title's start inset when no leading content is present, in logical units.
  float title_inset = 16.0F;
  /// Gap between leading content and the title in logical units.
  float title_spacing = 4.0F;
  /// Gap between adjacent actions in logical units.
  float action_spacing = 0.0F;

  /// Returns the standalone baseline style.
  /// @return Default field values; active themes may provide different values.
  static TopAppBarStyle Default();

  bool operator==(const TopAppBarStyle&) const = default;
};

/// Configures the horizontal controlled NavigationBar through the active Environment.
/// @code
/// NavigationBarStyle style = NavigationBarStyle::Default();
/// style.show_unselected_labels = false;
/// @endcode
struct NavigationBarStyle {
  /// Background of the horizontal navigation bar.
  Color background = Color::Transparent();
  /// Typography and unselected label color.
  TextStyle label_style{Font::System(12.0F), Color::Rgb(87, 96, 106)};
  /// Selected label and tintable icon color.
  Color selected_content = Color::Rgb(31, 111, 235);
  /// Disabled label and tintable icon color.
  Color disabled_content = Color::Rgb(31, 35, 40, 0.38F);
  /// Fill of the selected item's animated indicator.
  Color indicator = Color::Rgb(31, 111, 235, 0.12F);
  /// Preferred indicator dimensions in logical units.
  Size indicator_size{56.0F, 32.0F};
  /// Insets around each item's content in logical units.
  EdgeInsets item_padding = EdgeInsets::Symmetric(4.0F, 6.0F);
  /// Icon width and height in logical units.
  float icon_size = 24.0F;
  /// Vertical gap between the icon and label in logical units.
  float icon_spacing = 4.0F;
  /// Preferred minimum width of each item in logical units.
  float minimum_item_width = 64.0F;
  /// Preferred bar height in logical units.
  float height = 64.0F;
  /// Uniform selected-indicator corner radius in logical units.
  float indicator_corner_radius = 16.0F;
  /// Whether unselected items retain visible labels.
  bool show_unselected_labels = true;
  /// Optional pointer and keyboard interaction feedback; absence disables the indication.
  std::optional<Indication> indication;
  /// Timing used when the controlled selected item changes.
  AnimationSpec selection_animation = TweenSpec{.duration = 0.16};

  /// Returns the standalone baseline style.
  /// @return Default field values; active themes may provide different values.
  static NavigationBarStyle Default();

  bool operator==(const NavigationBarStyle&) const = default;
};

/// Configures compact and expanded NavigationPane presentation through the active Environment.
/// @code
/// NavigationPaneStyle style = NavigationPaneStyle::Default();
/// style.expanded_min_width = 280.0F;
/// @endcode
struct NavigationPaneStyle {
  /// Background of the vertical navigation pane.
  Color background = Color::Transparent();
  /// Typography and unselected label color.
  TextStyle label_style{Font::System(14.0F), Color::Rgb(87, 96, 106)};
  /// Selected label and tintable icon color.
  Color selected_content = Color::Rgb(31, 111, 235);
  /// Disabled label and tintable icon color.
  Color disabled_content = Color::Rgb(31, 35, 40, 0.38F);
  /// Fill of the selected item's animated indicator.
  Color indicator = Color::Rgb(31, 111, 235, 0.12F);
  /// Outer item spacing in logical units.
  EdgeInsets item_margin = EdgeInsets::Symmetric(8.0F, 0.0F);
  /// Insets around each item's content in logical units.
  EdgeInsets item_padding = EdgeInsets::Symmetric(12.0F, 0.0F);
  /// Preferred pane width while compact, in logical units.
  float compact_width = 72.0F;
  /// Preferred minimum pane width while expanded, in logical units.
  float expanded_min_width = 256.0F;
  /// Preferred item height in logical units.
  float item_height = 48.0F;
  /// Icon width and height in logical units.
  float icon_size = 24.0F;
  /// Gap between the icon and expanded label in logical units.
  float icon_spacing = 12.0F;
  /// Preferred indicator dimensions in compact mode, in logical units.
  Size compact_indicator_size{56.0F, 32.0F};
  /// Uniform selected-indicator corner radius in logical units.
  float indicator_corner_radius = 16.0F;
  /// Optional pointer and keyboard interaction feedback; absence disables the indication.
  std::optional<Indication> indication;
  /// Timing used when the controlled selected item changes.
  AnimationSpec selection_animation = TweenSpec{.duration = 0.16};

  /// Returns the standalone baseline style.
  /// @return Default field values; active themes may provide different values.
  static NavigationPaneStyle Default();

  bool operator==(const NavigationPaneStyle&) const = default;
};

/// Describes modal drawer opening and closing animation independently from controlled visibility.
/// @code
/// DrawerMotion motion{.open = TweenSpec{0.24}, .close = TweenSpec{0.16}};
/// @endcode
struct DrawerMotion {
  /// Timing for opening the modal drawer.
  AnimationSpec open = TweenSpec{.duration = 0.3};
  /// Timing for closing the modal drawer.
  AnimationSpec close = TweenSpec{.duration = 0.2};

  bool operator==(const DrawerMotion&) const = default;
};

/// Configures DrawerLayout panel geometry and modal presentation through the active Environment.
///
/// Inline drawers shrink within their allowed width before falling back to modal placement when content cannot fit.
/// @code
/// DrawerStyle style = DrawerStyle::Default();
/// style.preferred_width = 300.0F;
/// style.motion = std::nullopt;
/// @endcode
struct DrawerStyle {
  /// Drawer panel background.
  Color background = Color::White();
  /// Backdrop color used while a modal drawer covers application content.
  Color scrim = Color::Rgb(0, 0, 0, 0.4F);
  /// Modal panel shadow; persistent inline drawers remain flat.
  Shadow shadow{Color::Rgb(0, 0, 0, 0.2F), {}, 16.0F, 0.0F};
  /// Preferred panel width in logical units; inline panels may shrink toward minimum_width.
  float preferred_width = 320.0F;
  /// Minimum inline panel width before falling back to modal placement, in logical units.
  float minimum_width = 240.0F;
  /// Minimum logical width preserved for application content during inline placement.
  float minimum_content_width = 360.0F;
  /// Minimum application content width left visible beside a constrained modal panel, in logical units.
  float modal_content_reveal = 56.0F;
  /// Width of the edge gesture activation region in logical units.
  float edge_drag_width = 24.0F;
  /// Modal panel corner radius in logical units; inline panels remain rectangular.
  float corner_radius = 16.0F;
  /// Optional modal opening and closing timing; absence disables motion.
  std::optional<DrawerMotion> motion = DrawerMotion{};

  /// Returns the standalone baseline style.
  /// @return Default field values; active themes may provide different values.
  static DrawerStyle Default();

  bool operator==(const DrawerStyle&) const = default;
};

/// Arranges a title, optional leading View, and trailing action Views in one horizontal app bar.
/// @code
/// return TopAppBar("Catalog", Button("Back"), {Button("Search")})
///     .TitleAlignment(TopAppBarTitleAlignment::Center);
/// @endcode
class TopAppBar final : public Layout<TopAppBar> {
public:
  explicit TopAppBar(StringVariant title, std::optional<View> leading = std::nullopt, std::vector<View> actions = {});
  TopAppBar(StringVariant title, std::optional<View> leading, std::initializer_list<View> actions)
      : TopAppBar(std::move(title), std::move(leading), std::vector<View>(actions)) {}

  /// Selects the title's horizontal placement.
  /// @param alignment Start-aligned or centered title placement.
  /// @return The configured app bar for continued chaining.
  TopAppBar TitleAlignment(TopAppBarTitleAlignment alignment) &&;

  /// Measures and places this layout's children through the framework layout protocol.
  /// @param context Measurement access to the current children and environment geometry.
  /// @param node Mounted instance of this layout; child references are valid only during this call.
  /// @param constraints Bounds imposed by the parent layout.
  /// @return Constrained layout size and child placements.
  static LayoutResult Measure(LayoutContext& context, ViewNode& node, Constraints constraints);

private:
  struct Construction;

  explicit TopAppBar(Construction construction);
  static Construction Build(StringVariant title, std::optional<View> leading, std::vector<View> actions);
  void UpdateConfiguration();

  TopAppBarTitleAlignment title_alignment_ = TopAppBarTitleAlignment::Start;
};

/// Describes one labeled destination for NavigationBar or NavigationPane.
///
/// The owner controls selection; an item only supplies its label, optional icons, and enabled state.
/// @code
/// auto destination = NavigationItem("Settings").Enabled(false);
/// @endcode
class NavigationItem final {
public:
  explicit NavigationItem(StringVariant label);
  NavigationItem(ImageVariant icon, StringVariant label);

  /// Supplies an alternate icon shown when this item is selected.
  /// @param icon Valid image or vector value used instead of the ordinary icon.
  /// @return The configured item.
  /// @throws std::invalid_argument If icon is invalid.
  NavigationItem SelectedIcon(ImageVariant icon) &&;
  /// Controls whether the item can request selection.
  /// @param enabled False to disable user activation without changing the owner's selected index.
  /// @return The configured item.
  NavigationItem Enabled(bool enabled) &&;

private:
  std::optional<ImageVariant> icon_;
  std::optional<ImageVariant> selected_icon_;
  StringVariant label_;
  bool enabled_ = true;

  friend struct detail::InternalAccess;
};

/// Displays controlled peer destinations in a horizontal bar.
///
/// Each item requires an icon and a non-blank label. The selected index must refer to an item; disabled items do not
/// request changes. OnChanged() reports an index, and the owner supplies the next authoritative selected value.
/// The example uses application-provided home_icon and search_icon ImageVariant values.
/// @code
/// auto selected = UseState<std::size_t>(0);
/// return NavigationBar({NavigationItem(home_icon, "Home"), NavigationItem(search_icon, "Search")}, selected)
///     .OnChanged([selected](std::size_t index) { selected = index; });
/// @endcode
class NavigationBar final : public detail::TypedView<NavigationBar> {
public:
  NavigationBar(std::initializer_list<NavigationItem> items, std::size_t selected_index)
      : NavigationBar(std::vector<NavigationItem>(items), selected_index) {}
  NavigationBar(std::initializer_list<NavigationItem> items, const State<std::size_t>& selected_index)
      : NavigationBar(std::vector<NavigationItem>(items), selected_index.Get()) {}
  NavigationBar(std::vector<NavigationItem> items, std::size_t selected_index);
  NavigationBar(std::vector<NavigationItem> items, const State<std::size_t>& selected_index)
      : NavigationBar(std::move(items), selected_index.Get()) {}

  /// Handles a requested selected-index change through NavigationEvents::Changed.
  /// @tparam Function Callable accepting the requested std::size_t index.
  /// @param function Handler that updates the owner's controlled selection.
  /// @return The configured navigation View.
  template <class Function> NavigationBar OnChanged(Function&& function) && {
    return std::move(*this).On<NavigationEvents::Changed>(std::forward<Function>(function));
  }
};

/// Displays controlled peer destinations as a compact rail or an expanded vertical pane.
///
/// Compact items require icons; expanded items may be label-only. Labels must be non-blank and the selected index must
/// refer to an item. Selection remains application-owned.
/// @code
/// auto selected = UseState<std::size_t>(0);
/// return NavigationPane({NavigationItem("Inbox"), NavigationItem("Archive")}, selected, true)
///     .OnChanged([selected](std::size_t index) { selected = index; });
/// @endcode
class NavigationPane final : public detail::TypedView<NavigationPane> {
public:
  NavigationPane(std::initializer_list<NavigationItem> items, std::size_t selected_index, bool expanded = false)
      : NavigationPane(std::vector<NavigationItem>(items), selected_index, expanded) {}
  NavigationPane(
      std::initializer_list<NavigationItem> items, const State<std::size_t>& selected_index, bool expanded = false
  )
      : NavigationPane(std::vector<NavigationItem>(items), selected_index.Get(), expanded) {}
  NavigationPane(std::vector<NavigationItem> items, std::size_t selected_index, bool expanded = false);
  NavigationPane(std::vector<NavigationItem> items, const State<std::size_t>& selected_index, bool expanded = false)
      : NavigationPane(std::move(items), selected_index.Get(), expanded) {}

  /// Handles a requested selected-index change through NavigationEvents::Changed.
  /// @tparam Function Callable accepting the requested std::size_t index.
  /// @param function Handler that updates the owner's controlled selection.
  /// @return The configured navigation View.
  template <class Function> NavigationPane OnChanged(Function&& function) && {
    return std::move(*this).On<NavigationEvents::Changed>(std::forward<Function>(function));
  }
};

/// Declares the start-side panel of a DrawerLayout.
///
/// Open() is controlled visibility. Gesture, backdrop, and Back dismissal requests are emitted through OnOpenChanged().
/// @code
/// auto open = UseState(false);
/// return DrawerLayout(
///     Text("Content"),
///     StartDrawer(Text("Panel")).Open(open).OnOpenChanged([open](bool next) { open = next; })
/// );
/// @endcode
class StartDrawer final : public Layout<StartDrawer> {
public:
  explicit StartDrawer(View content);

  /// Sets the authoritative visibility of this drawer.
  /// @param open True to show the panel; false to close it.
  /// @return The configured drawer.
  StartDrawer Open(bool open) &&;
  /// Reads controlled visibility from an existing State value during composition.
  /// @param open Valid owner state; the drawer never assigns to it directly.
  /// @return The configured drawer.
  StartDrawer Open(const State<bool>& open) && {
    return std::move(*this).Open(open.Get());
  }

  /// Handles a requested visibility change through DrawerEvents::OpenChanged.
  /// @tparam Function Callable accepting the requested bool visibility.
  /// @param function Handler that updates the owner's controlled open state.
  /// @return The configured drawer.
  template <class Function> StartDrawer OnOpenChanged(Function&& function) && {
    return std::move(*this).On<DrawerEvents::OpenChanged>(std::forward<Function>(function));
  }

  /// Measures and places this layout's children through the framework layout protocol.
  /// @param context Measurement access to the current children and environment geometry.
  /// @param node Mounted instance of this layout; child references are valid only during this call.
  /// @param constraints Bounds imposed by the parent layout.
  /// @return Constrained layout size and child placements.
  static LayoutResult Measure(LayoutContext& context, ViewNode& node, Constraints constraints);

private:
  bool open_ = false;

  friend class DrawerLayout;
};

/// Declares the end-side panel of a DrawerLayout.
///
/// Open() is controlled visibility. Gesture, backdrop, and Back dismissal requests are emitted through OnOpenChanged().
/// @code
/// auto open = UseState(false);
/// return DrawerLayout(
///     Text("Content"),
///     EndDrawer(Text("Panel")).Open(open).OnOpenChanged([open](bool next) { open = next; })
/// );
/// @endcode
class EndDrawer final : public Layout<EndDrawer> {
public:
  explicit EndDrawer(View content);

  /// Sets the authoritative visibility of this drawer.
  /// @param open True to show the panel; false to close it.
  /// @return The configured drawer.
  EndDrawer Open(bool open) &&;
  /// Reads controlled visibility from an existing State value during composition.
  /// @param open Valid owner state; the drawer never assigns to it directly.
  /// @return The configured drawer.
  EndDrawer Open(const State<bool>& open) && {
    return std::move(*this).Open(open.Get());
  }

  /// Handles a requested visibility change through DrawerEvents::OpenChanged.
  /// @tparam Function Callable accepting the requested bool visibility.
  /// @param function Handler that updates the owner's controlled open state.
  /// @return The configured drawer.
  template <class Function> EndDrawer OnOpenChanged(Function&& function) && {
    return std::move(*this).On<DrawerEvents::OpenChanged>(std::forward<Function>(function));
  }

  /// Measures and places this layout's children through the framework layout protocol.
  /// @param context Measurement access to the current children and environment geometry.
  /// @param node Mounted instance of this layout; child references are valid only during this call.
  /// @param constraints Bounds imposed by the parent layout.
  /// @return Constrained layout size and child placements.
  static LayoutResult Measure(LayoutContext& context, ViewNode& node, Constraints constraints);

private:
  bool open_ = false;

  friend class DrawerLayout;
};

/// Arranges application content with optional start and end drawer panels.
///
/// Available local width and DrawerStyle select persistent inline or modal placement. Modal panels own their scrim and
/// Back/gesture dismissal requests; changing placement does not rewrite the owner's controlled visibility.
/// @code
/// return DrawerLayout(Text("Workspace"), StartDrawer(Text("Folders")).Open(true));
/// @endcode
class DrawerLayout final : public Layout<DrawerLayout> {
public:
  explicit DrawerLayout(View content);
  DrawerLayout(View content, StartDrawer start);
  DrawerLayout(View content, EndDrawer end);
  DrawerLayout(View content, StartDrawer start, EndDrawer end);

  /// Measures and places this layout's children through the framework layout protocol.
  /// @param context Measurement access to the current children and environment geometry.
  /// @param node Mounted instance of this layout; child references are valid only during this call.
  /// @param constraints Bounds imposed by the parent layout.
  /// @return Constrained layout size and child placements.
  static LayoutResult Measure(LayoutContext& context, ViewNode& node, Constraints constraints);

private:
  struct Construction;

  explicit DrawerLayout(Construction construction);
  static Construction Build(View content, std::optional<StartDrawer> start, std::optional<EndDrawer> end);
};

template <detail::NavigationRouteValue Route> class RouteNavigationController;

/// Stores the ordered route values above a routed NavigationStack's fixed root.
///
/// The root is not an element of the path. Put this value in State to control a routed stack; replacing the value
/// reconciles destinations while preserving the longest equal route prefix.
/// @tparam Route Copyable, equality-comparable application route value.
/// @code
/// NavigationPath<int> path{42, 57};
/// const bool has_destinations = !path.Empty();
/// @endcode
template <detail::NavigationRouteValue Route> class NavigationPath final {
public:
  NavigationPath() = default;
  NavigationPath(std::initializer_list<Route> routes) : routes_(routes) {}
  explicit NavigationPath(std::vector<Route> routes) : routes_(std::move(routes)) {}

  /// Reports whether only the fixed root is represented.
  /// @return True when no route values follow the root.
  [[nodiscard]] bool Empty() const noexcept {
    return routes_.empty();
  }

  /// Counts route values, excluding the fixed root.
  /// @return The number of destinations stored in this path.
  [[nodiscard]] std::size_t Size() const noexcept {
    return routes_.size();
  }

  /// Borrows the ordered route values without copying them.
  /// @return A read-only span valid until this path is assigned, moved from, or destroyed.
  [[nodiscard]] std::span<const Route> Routes() const noexcept {
    return routes_;
  }

  bool operator==(const NavigationPath&) const = default;

private:
  std::vector<Route> routes_;

  friend class RouteNavigationController<Route>;
};

namespace detail {

template <NavigationRouteValue Route>
using NavigationHistoryCommit = std::function<void(NavigationHistoryAction, NavigationPath<Route>)>;

template <NavigationRouteValue Route, class RootFactory, class Resolver>
  requires ViewFactoryFor<RootFactory> && std::copy_constructible<std::decay_t<Resolver>> &&
           requires(std::decay_t<Resolver>& resolver, const Route& route) {
             { std::invoke(resolver, route) } -> std::convertible_to<View>;
           }
View BuildTypedNavigationStack(
    RootFactory&& root,
    State<NavigationPath<Route>> path,
    Resolver&& resolver,
    std::shared_ptr<NavigationHistoryCommit<Route>> history_commit
) {
  const auto callable_is_empty = []<class Callable>(const Callable& callable) {
    if constexpr (std::is_pointer_v<Callable>) {
      return callable == nullptr;
    } else if constexpr (requires { static_cast<bool>(callable); }) {
      return !static_cast<bool>(callable);
    }
    return false;
  };
  if (callable_is_empty(root)) {
    throw std::invalid_argument("HuxerUI navigation root factory must not be empty");
  }
  if (callable_is_empty(resolver)) {
    throw std::invalid_argument("HuxerUI navigation destination resolver must not be empty");
  }
  if (!path.IsValid()) {
    throw std::invalid_argument("HuxerUI navigation path state must not be empty");
  }
  std::function<View()> root_factory = BindViewFactory(std::forward<RootFactory>(root));
  using StoredResolver = std::decay_t<Resolver>;
  StoredResolver stored_resolver(std::forward<Resolver>(resolver));

  return Scope(
      [path,
       root_factory = std::move(root_factory),
       resolver = std::move(stored_resolver),
       history_commit = std::move(history_commit)]() mutable -> View {
        auto shared_resolver = std::make_shared<StoredResolver>(resolver);
        const NavigationPath<Route>& current_path = path.Get();
        std::vector<NavigationRouteDescriptor> routes;
        routes.reserve(current_path.Size());
        for (const Route& route : current_path.Routes()) {
          auto value = std::make_shared<Route>(route);
          routes.push_back({
              value,
              [](const void* first, const void* second) {
                return *static_cast<const Route*>(first) == *static_cast<const Route*>(second);
              },
              [shared_resolver, value]() mutable -> View { return std::invoke(*shared_resolver, *value); },
          });
        }

        NavigationRouteBinding binding{
            .route_type = typeid(Route),
            .path_state = std::make_shared<State<NavigationPath<Route>>>(path),
            .history_commit = history_commit,
            .request_pop = [path, history_commit] {
              const NavigationPath<Route>& current_path = path.Get();
              if (current_path.Empty()) {
                return false;
              }
              std::vector<Route> routes(current_path.Routes().begin(), current_path.Routes().end());
              routes.pop_back();
              NavigationPath<Route> next(std::move(routes));
              if (history_commit && *history_commit) {
                (*history_commit)(NavigationHistoryAction::Pop, std::move(next));
              } else {
                path = std::move(next);
              }
              return true;
            },
        };
        return BuildRoutedNavigationStack(root_factory, std::move(routes), std::move(binding));
      }
  );
}

} // namespace detail

/// Finds the nearest routed stack with the requested route type in the current composition Environment.
/// @tparam Route Route value type used by the enclosing controlled stack.
/// @return A controller for the nearest matching stack.
/// @throws std::logic_error If no compatible routed stack encloses this composition.
/// @see RouteNavigationController
template <detail::NavigationRouteValue Route> RouteNavigationController<Route> UseNavigation();

/// Finds the outermost routed stack with the requested route type in the current composition Environment.
/// @tparam Route Route value type used by the enclosing controlled stack.
/// @return A controller for the outermost matching stack.
/// @throws std::logic_error If no compatible routed stack encloses this composition.
/// @see RouteNavigationController
template <detail::NavigationRouteValue Route> RouteNavigationController<Route> UseRootNavigation();

/// Updates the authoritative path of a compatible routed NavigationStack.
///
/// Controllers do not retain the mounted stack. Use them on the Runtime UI thread; after disconnection, mutations
/// other than Pop() throw and read-only queries report an empty stack.
/// @tparam Route Copyable, equality-comparable route type of the owning stack.
/// @code
/// auto navigation = UseNavigation<int>();
/// return Button("Open product").OnClick([navigation] { navigation.Push(42); });
/// @endcode
template <detail::NavigationRouteValue Route> class RouteNavigationController final {
public:
  RouteNavigationController() = default;

  /// Appends a destination to the current path.
  /// @param route Application route value resolved by the stack's destination factory.
  /// @throws std::logic_error If this controller is disconnected.
  void Push(Route route) const {
    RequireConnected();
    NavigationPath<Route> next = path_.Get();
    next.routes_.push_back(std::move(route));
    Commit(detail::NavigationHistoryAction::Push, std::move(next));
  }

  /// Removes the last route when connected and above the fixed root.
  /// @return True when a path change was requested; false at the root or after disconnection.
  bool Pop() const {
    if (!detail::CheckNavigationAccess(state_)) {
      return false;
    }
    if (path_.Get().routes_.empty()) {
      return false;
    }
    NavigationPath<Route> next = path_.Get();
    next.routes_.pop_back();
    Commit(detail::NavigationHistoryAction::Pop, std::move(next));
    return true;
  }

  /// Replaces the last route without replacing the stack's fixed root.
  /// @param route Application route value for the replacement destination.
  /// @throws std::logic_error If disconnected or the current path is empty.
  void Replace(Route route) const {
    RequireConnected();
    if (path_.Get().routes_.empty()) {
      throw std::logic_error("HuxerUI routed navigation cannot replace its fixed root");
    }
    NavigationPath<Route> next = path_.Get();
    next.routes_.back() = std::move(route);
    Commit(detail::NavigationHistoryAction::Replace, std::move(next));
  }

  /// Replaces the complete authoritative route sequence.
  /// @param path New destinations above the fixed root; an empty value returns to that root.
  /// @throws std::logic_error If this controller is disconnected.
  /// @code
  /// navigation.SetPath(NavigationPath<Route>{});
  /// @endcode
  void SetPath(NavigationPath<Route> path) const {
    RequireConnected();
    Commit(detail::NavigationHistoryAction::Replace, std::move(path));
  }

  /// Reports whether a route can be removed.
  /// @return True when connected and the authoritative path is non-empty.
  [[nodiscard]] bool CanPop() const {
    return detail::CheckNavigationAccess(state_) && !path_.Get().Empty();
  }

  /// Reports logical depth, including the fixed root.
  /// @return Path size plus one while connected, or zero after disconnection.
  [[nodiscard]] std::size_t Depth() const {
    return detail::CheckNavigationAccess(state_) ? path_.Get().Size() + 1 : 0;
  }

private:
  RouteNavigationController(
      std::weak_ptr<detail::NavigationState> state,
      State<NavigationPath<Route>> path,
      std::shared_ptr<detail::NavigationHistoryCommit<Route>> history_commit
  )
      : state_(std::move(state)), path_(std::move(path)), history_commit_(std::move(history_commit)) {}

  void Commit(detail::NavigationHistoryAction action, NavigationPath<Route> path) const {
    if (history_commit_ && *history_commit_) {
      (*history_commit_)(action, std::move(path));
      return;
    }
    path_ = std::move(path);
  }

  void RequireConnected() const {
    if (!detail::CheckNavigationAccess(state_)) {
      throw std::logic_error("HuxerUI routed navigation controller is disconnected");
    }
  }

  std::weak_ptr<detail::NavigationState> state_;
  State<NavigationPath<Route>> path_;
  std::shared_ptr<detail::NavigationHistoryCommit<Route>> history_commit_;

  friend RouteNavigationController<Route> UseNavigation<Route>();
  friend RouteNavigationController<Route> UseRootNavigation<Route>();
};

/// Controls a factory-based NavigationStack through Push, Pop, and Replace.
///
/// Factories are retained and invoked within each page's composition scope. Controllers do not retain the mounted stack
/// and must be used on the Runtime UI thread. Factory navigation does not provide serializable route data.
/// @code
/// auto navigation = UseNavigation();
/// return Button("Details").OnClick([navigation] {
///   navigation.Push([] { return Text("Details"); });
/// });
/// @endcode
class NavigationController {
public:
  NavigationController() = default;

  /// Adds a logical page and starts its selected push transition.
  /// @param page Non-empty factory invoked within the new page's composition scope.
  /// @throws std::invalid_argument If page is empty.
  /// @throws std::logic_error If this controller is disconnected.
  void Push(std::function<View()> page) const;

  /// Binds a page factory and its arguments by value before pushing it.
  /// @tparam Factory Copyable callable producing a value convertible to View.
  /// @tparam Arguments Types of values retained for each factory invocation.
  /// @param page Page factory; pass the callable itself rather than an already-built View.
  /// @param arguments Values forwarded into owned storage for subsequent page composition.
  /// @throws std::logic_error If this controller is disconnected.
  /// @code
  /// navigation.Push([](int id) { return Text::Format("Product {}", id); }, 42);
  /// @endcode
  template <class Factory, class... Arguments>
    requires detail::ViewFactoryFor<Factory, Arguments...>
  void Push(Factory&& page, Arguments&&... arguments) const {
    Push(detail::BindViewFactory(std::forward<Factory>(page), std::forward<Arguments>(arguments)...));
  }

  /// Removes the logical top page, retaining its visual content until the transition finishes.
  /// @return True when the operation is accepted; false at the root or after disconnection.
  bool Pop() const;
  /// Replaces the logical top page, including a depth-one factory root.
  /// @param page Non-empty factory invoked within the replacement page's composition scope.
  /// @throws std::invalid_argument If page is empty.
  /// @throws std::logic_error If this controller is disconnected.
  void Replace(std::function<View()> page) const;

  /// Binds a replacement page factory and its arguments by value.
  /// @tparam Factory Copyable callable producing a value convertible to View.
  /// @tparam Arguments Types of values retained for each factory invocation.
  /// @param page Replacement factory; its result is composed inside the stack.
  /// @param arguments Values forwarded into owned storage for subsequent page composition.
  /// @throws std::logic_error If this controller is disconnected.
  template <class Factory, class... Arguments>
    requires detail::ViewFactoryFor<Factory, Arguments...>
  void Replace(Factory&& page, Arguments&&... arguments) const {
    Replace(detail::BindViewFactory(std::forward<Factory>(page), std::forward<Arguments>(arguments)...));
  }

  /// Reports whether the logical stack has a page above its root.
  /// @return True when connected and above the root; visual execution may be queued behind another operation.
  [[nodiscard]] bool CanPop() const;
  /// Counts logical pages, including the root and excluding retired visual-only pages.
  /// @return Logical page count while connected, or zero after disconnection.
  [[nodiscard]] std::size_t Depth() const;

private:
  explicit NavigationController(std::weak_ptr<detail::NavigationState> state) : state_(std::move(state)) {}

  std::weak_ptr<detail::NavigationState> state_;

  friend View NavigationStack(std::function<View()> root);
  friend NavigationController UseNavigation();
  friend NavigationController UseRootNavigation();
};

/// Creates a factory-based page stack with an independent scope for each retained page.
/// @param root Non-empty factory for the initial page.
/// @return A navigation container filling its assigned layout bounds.
/// @throws std::invalid_argument If root is empty.
/// @code
/// return NavigationStack([] { return Text("Home"); });
/// @endcode
View NavigationStack(std::function<View()> root);

/// Creates a factory stack with arguments bound by value to its root factory.
/// @tparam Factory Copyable callable producing a value convertible to View.
/// @tparam Arguments Types of values retained for root composition.
/// @param root Root page factory.
/// @param arguments Values forwarded into owned storage for each root invocation.
/// @return A factory-based navigation container.
template <class Factory, class... Arguments>
  requires detail::ViewFactoryFor<Factory, Arguments...>
View NavigationStack(Factory&& root, Arguments&&... arguments) {
  return NavigationStack(detail::BindViewFactory(std::forward<Factory>(root), std::forward<Arguments>(arguments)...));
}

/// Creates a stack whose destinations are controlled by State<NavigationPath<Route>>.
/// @tparam Route Copyable, equality-comparable application route value.
/// @tparam RootFactory Copyable factory for the fixed root.
/// @tparam Resolver Copyable callable mapping const Route& to a value convertible to View.
/// @param root Non-empty factory for the fixed root, which is excluded from path.
/// @param path Valid authoritative route state; external assignments reconcile the mounted destinations.
/// @param resolver Non-empty destination factory invoked in each page's composition scope.
/// @return A controlled routed navigation container.
/// @throws std::invalid_argument If a factory is empty or path is invalid.
/// @code
/// View App() {
///   auto path = UseState(NavigationPath<int>{});
///   return NavigationStack(
///       [] { return Text("Catalog"); }, path,
///       [](const int& id) { return Text::Format("Product {}", id); }
///   );
/// }
/// @endcode
template <detail::NavigationRouteValue Route, class RootFactory, class Resolver>
  requires detail::ViewFactoryFor<RootFactory> && std::copy_constructible<std::decay_t<Resolver>> &&
           requires(std::decay_t<Resolver>& resolver, const Route& route) {
             { std::invoke(resolver, route) } -> std::convertible_to<View>;
           }
View NavigationStack(RootFactory&& root, State<NavigationPath<Route>> path, Resolver&& resolver) {
  return detail::BuildTypedNavigationStack(
      std::forward<RootFactory>(root),
      std::move(path),
      std::forward<Resolver>(resolver),
      std::shared_ptr<detail::NavigationHistoryCommit<Route>>{}
  );
}

/// Finds the nearest factory-based stack in the current composition Environment.
/// @return A controller for the nearest compatible stack.
/// @throws std::logic_error If there is no enclosing factory-based stack.
/// @see NavigationController
NavigationController UseNavigation();
/// Finds the outermost factory-based stack in the current composition Environment.
/// @return A controller for the outermost compatible stack.
/// @throws std::logic_error If there is no enclosing factory-based stack.
/// @code
/// auto root_navigation = UseRootNavigation();
/// root_navigation.Push([] { return Text("Global details"); });
/// @endcode
NavigationController UseRootNavigation();

template <detail::NavigationRouteValue Route> RouteNavigationController<Route> UseNavigation() {
  detail::NavigationAccess access = detail::FindNavigationAccess(typeid(Route), false);
  if (access.state.expired() || !access.path_state) {
    throw std::logic_error("HuxerUI UseNavigation<Route>() requires an enclosing compatible routed NavigationStack");
  }
  return RouteNavigationController<Route>{
      std::move(access.state),
      *std::static_pointer_cast<State<NavigationPath<Route>>>(std::move(access.path_state)),
      std::static_pointer_cast<detail::NavigationHistoryCommit<Route>>(std::move(access.history_commit)),
  };
}

template <detail::NavigationRouteValue Route> RouteNavigationController<Route> UseRootNavigation() {
  detail::NavigationAccess access = detail::FindNavigationAccess(typeid(Route), true);
  if (access.state.expired() || !access.path_state) {
    throw std::logic_error(
        "HuxerUI UseRootNavigation<Route>() requires an enclosing compatible routed NavigationStack"
    );
  }
  return RouteNavigationController<Route>{
      std::move(access.state),
      *std::static_pointer_cast<State<NavigationPath<Route>>>(std::move(access.path_state)),
      std::static_pointer_cast<detail::NavigationHistoryCommit<Route>>(std::move(access.history_commit)),
  };
}

} // namespace huxerui
