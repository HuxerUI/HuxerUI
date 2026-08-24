#pragma once

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
struct NavigationItemAccess;

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

struct NavigationMotion {
  Point entering_offset_fraction{1.0F, 0.0F};
  Point covered_offset_fraction{-1.0F, 0.0F};
  float entering_scale = 1.0F;
  float covered_scale = 1.0F;
  float entering_opacity = 1.0F;
  float covered_opacity = 1.0F;
  AnimationSpec push = TweenSpec{.duration = 0.3};
  AnimationSpec pop = TweenSpec{.duration = 0.25};

  bool operator==(const NavigationMotion&) const = default;
};

struct NavigationStyle {
  std::optional<NavigationMotion> motion = NavigationMotion{};

  static NavigationStyle Default();

  bool operator==(const NavigationStyle&) const = default;
};

enum class TopAppBarTitleAlignment {
  Start,
  Center,
};

struct TopAppBarStyle {
  Color background = Color::White();
  TextStyle title_style{Font::System(20.0F).WithWeight(FontWeight::Bold), Color::Rgb(31, 35, 40)};
  float height = 48.0F;
  float horizontal_padding = 8.0F;
  float title_inset = 16.0F;
  float title_spacing = 4.0F;
  float action_spacing = 0.0F;

  static TopAppBarStyle Default();

  bool operator==(const TopAppBarStyle&) const = default;
};

struct NavigationBarStyle {
  Color background = Color::Transparent();
  TextStyle label_style{Font::System(12.0F), Color::Rgb(87, 96, 106)};
  Color selected_content = Color::Rgb(31, 111, 235);
  Color disabled_content = Color::Rgb(31, 35, 40, 0.38F);
  Color indicator = Color::Rgb(31, 111, 235, 0.12F);
  Size indicator_size{56.0F, 32.0F};
  EdgeInsets item_padding = EdgeInsets::Symmetric(4.0F, 6.0F);
  float icon_size = 24.0F;
  float icon_spacing = 4.0F;
  float minimum_item_width = 64.0F;
  float height = 64.0F;
  float indicator_corner_radius = 16.0F;
  bool show_unselected_labels = true;
  std::optional<Indication> indication;
  AnimationSpec selection_animation = TweenSpec{.duration = 0.16};

  static NavigationBarStyle Default();

  bool operator==(const NavigationBarStyle&) const = default;
};

struct NavigationPaneStyle {
  Color background = Color::Transparent();
  TextStyle label_style{Font::System(14.0F), Color::Rgb(87, 96, 106)};
  Color selected_content = Color::Rgb(31, 111, 235);
  Color disabled_content = Color::Rgb(31, 35, 40, 0.38F);
  Color indicator = Color::Rgb(31, 111, 235, 0.12F);
  EdgeInsets item_margin = EdgeInsets::Symmetric(8.0F, 0.0F);
  EdgeInsets item_padding = EdgeInsets::Symmetric(12.0F, 0.0F);
  float compact_width = 72.0F;
  float expanded_min_width = 256.0F;
  float item_height = 48.0F;
  float icon_size = 24.0F;
  float icon_spacing = 12.0F;
  Size compact_indicator_size{56.0F, 32.0F};
  float indicator_corner_radius = 16.0F;
  std::optional<Indication> indication;
  AnimationSpec selection_animation = TweenSpec{.duration = 0.16};

  static NavigationPaneStyle Default();

  bool operator==(const NavigationPaneStyle&) const = default;
};

struct DrawerMotion {
  AnimationSpec open = TweenSpec{.duration = 0.3};
  AnimationSpec close = TweenSpec{.duration = 0.2};

  bool operator==(const DrawerMotion&) const = default;
};

struct DrawerStyle {
  Color background = Color::White();
  Color scrim = Color::Rgb(0, 0, 0, 0.4F);
  // Shadow and corner_radius belong to modal presentation; persistent inline drawers are rectangular and flat.
  Shadow shadow{Color::Rgb(0, 0, 0, 0.2F), {}, 16.0F, 0.0F};
  // Inline drawers shrink from preferred_width to minimum_width before falling back to modal placement so the
  // surrounding content retains at least minimum_content_width.
  float preferred_width = 320.0F;
  float minimum_width = 240.0F;
  float minimum_content_width = 360.0F;
  // Modal drawers keep this much application content visible when their preferred width does not fit.
  float modal_content_reveal = 56.0F;
  float edge_drag_width = 24.0F;
  float corner_radius = 16.0F;
  std::optional<DrawerMotion> motion = DrawerMotion{};

  static DrawerStyle Default();

  bool operator==(const DrawerStyle&) const = default;
};

class TopAppBar final : public Layout<TopAppBar> {
public:
  explicit TopAppBar(StringVariant title, std::optional<View> leading = std::nullopt, std::vector<View> actions = {});
  TopAppBar(StringVariant title, std::optional<View> leading, std::initializer_list<View> actions)
      : TopAppBar(std::move(title), std::move(leading), std::vector<View>(actions)) {}

  TopAppBar TitleAlignment(TopAppBarTitleAlignment alignment) &&;

  static LayoutResult Measure(LayoutContext& context, MountedNode& node, Constraints constraints);

private:
  struct Construction;

  explicit TopAppBar(Construction construction);
  static Construction Build(StringVariant title, std::optional<View> leading, std::vector<View> actions);
  void UpdateConfiguration();

  TopAppBarTitleAlignment title_alignment_ = TopAppBarTitleAlignment::Start;
};

class NavigationItem final {
public:
  explicit NavigationItem(StringVariant label);
  NavigationItem(ImageVariant icon, StringVariant label);

  NavigationItem SelectedIcon(ImageVariant icon) &&;
  NavigationItem Enabled(bool enabled) &&;

private:
  std::optional<ImageVariant> icon_;
  std::optional<ImageVariant> selected_icon_;
  StringVariant label_;
  bool enabled_ = true;

  friend struct detail::NavigationItemAccess;
};

class NavigationBar final : public detail::TypedView<NavigationBar> {
public:
  NavigationBar(std::initializer_list<NavigationItem> items, std::size_t selected_index)
      : NavigationBar(std::vector<NavigationItem>(items), selected_index) {}
  NavigationBar(std::initializer_list<NavigationItem> items, const State<std::size_t>& selected_index)
      : NavigationBar(std::vector<NavigationItem>(items), selected_index.Get()) {}
  NavigationBar(std::vector<NavigationItem> items, std::size_t selected_index);
  NavigationBar(std::vector<NavigationItem> items, const State<std::size_t>& selected_index)
      : NavigationBar(std::move(items), selected_index.Get()) {}

  template <class Function> NavigationBar OnChanged(Function&& function) && {
    return std::move(*this).On<NavigationEvents::Changed>(std::forward<Function>(function));
  }
};

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

  template <class Function> NavigationPane OnChanged(Function&& function) && {
    return std::move(*this).On<NavigationEvents::Changed>(std::forward<Function>(function));
  }
};

class StartDrawer final : public Layout<StartDrawer> {
public:
  explicit StartDrawer(View content);

  StartDrawer Open(bool open) &&;
  StartDrawer Open(const State<bool>& open) && {
    return std::move(*this).Open(open.Get());
  }

  template <class Function> StartDrawer OnOpenChanged(Function&& function) && {
    return std::move(*this).On<DrawerEvents::OpenChanged>(std::forward<Function>(function));
  }

  static LayoutResult Measure(LayoutContext& context, MountedNode& node, Constraints constraints);

private:
  bool open_ = false;

  friend class DrawerLayout;
};

class EndDrawer final : public Layout<EndDrawer> {
public:
  explicit EndDrawer(View content);

  EndDrawer Open(bool open) &&;
  EndDrawer Open(const State<bool>& open) && {
    return std::move(*this).Open(open.Get());
  }

  template <class Function> EndDrawer OnOpenChanged(Function&& function) && {
    return std::move(*this).On<DrawerEvents::OpenChanged>(std::forward<Function>(function));
  }

  static LayoutResult Measure(LayoutContext& context, MountedNode& node, Constraints constraints);

private:
  bool open_ = false;

  friend class DrawerLayout;
};

class DrawerLayout final : public Layout<DrawerLayout> {
public:
  explicit DrawerLayout(View content);
  DrawerLayout(View content, StartDrawer start);
  DrawerLayout(View content, EndDrawer end);
  DrawerLayout(View content, StartDrawer start, EndDrawer end);

  static LayoutResult Measure(LayoutContext& context, MountedNode& node, Constraints constraints);

private:
  struct Construction;

  explicit DrawerLayout(Construction construction);
  static Construction Build(View content, std::optional<StartDrawer> start, std::optional<EndDrawer> end);
};

template <detail::NavigationRouteValue Route> class RouteNavigationController;

template <detail::NavigationRouteValue Route> class NavigationPath final {
public:
  NavigationPath() = default;
  NavigationPath(std::initializer_list<Route> routes) : routes_(routes) {}
  explicit NavigationPath(std::vector<Route> routes) : routes_(std::move(routes)) {}

  [[nodiscard]] bool Empty() const noexcept {
    return routes_.empty();
  }

  [[nodiscard]] std::size_t Size() const noexcept {
    return routes_.size();
  }

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

template <detail::NavigationRouteValue Route> RouteNavigationController<Route> UseNavigation();

template <detail::NavigationRouteValue Route> RouteNavigationController<Route> UseRootNavigation();

template <detail::NavigationRouteValue Route> class RouteNavigationController final {
public:
  RouteNavigationController() = default;

  void Push(Route route) const {
    RequireConnected();
    NavigationPath<Route> next = path_.Get();
    next.routes_.push_back(std::move(route));
    Commit(detail::NavigationHistoryAction::Push, std::move(next));
  }

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

  void Replace(Route route) const {
    RequireConnected();
    if (path_.Get().routes_.empty()) {
      throw std::logic_error("HuxerUI routed navigation cannot replace its fixed root");
    }
    NavigationPath<Route> next = path_.Get();
    next.routes_.back() = std::move(route);
    Commit(detail::NavigationHistoryAction::Replace, std::move(next));
  }

  void SetPath(NavigationPath<Route> path) const {
    RequireConnected();
    Commit(detail::NavigationHistoryAction::Replace, std::move(path));
  }

  [[nodiscard]] bool CanPop() const {
    return detail::CheckNavigationAccess(state_) && !path_.Get().Empty();
  }

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

class NavigationController {
public:
  NavigationController() = default;

  void Push(std::function<View()> page) const;

  template <class Factory, class... Arguments>
    requires detail::ViewFactoryFor<Factory, Arguments...>
  void Push(Factory&& page, Arguments&&... arguments) const {
    Push(detail::BindViewFactory(std::forward<Factory>(page), std::forward<Arguments>(arguments)...));
  }

  bool Pop() const;
  void Replace(std::function<View()> page) const;

  template <class Factory, class... Arguments>
    requires detail::ViewFactoryFor<Factory, Arguments...>
  void Replace(Factory&& page, Arguments&&... arguments) const {
    Replace(detail::BindViewFactory(std::forward<Factory>(page), std::forward<Arguments>(arguments)...));
  }

  [[nodiscard]] bool CanPop() const;
  [[nodiscard]] std::size_t Depth() const;

private:
  explicit NavigationController(std::weak_ptr<detail::NavigationState> state) : state_(std::move(state)) {}

  std::weak_ptr<detail::NavigationState> state_;

  friend View NavigationStack(std::function<View()> root);
  friend NavigationController UseNavigation();
  friend NavigationController UseRootNavigation();
};

View NavigationStack(std::function<View()> root);

template <class Factory, class... Arguments>
  requires detail::ViewFactoryFor<Factory, Arguments...>
View NavigationStack(Factory&& root, Arguments&&... arguments) {
  return NavigationStack(detail::BindViewFactory(std::forward<Factory>(root), std::forward<Arguments>(arguments)...));
}

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

NavigationController UseNavigation();
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
