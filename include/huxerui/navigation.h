#pragma once

#include <cstddef>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
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
  std::optional<IndicationSpec> indication;
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
  std::optional<IndicationSpec> indication;
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

class NavigationItem final {
public:
  explicit NavigationItem(StringVariant label);
  NavigationItem(ImageResource icon, StringVariant label);
  NavigationItem(ImageAsset icon, StringVariant label);
  NavigationItem(VectorAsset icon, StringVariant label);

  NavigationItem SelectedIcon(ImageResource icon) &&;
  NavigationItem SelectedIcon(ImageAsset icon) &&;
  NavigationItem SelectedIcon(VectorAsset icon) &&;
  NavigationItem Enabled(bool enabled) &&;

private:
  using Icon = std::variant<std::monostate, ImageResource, ImageAsset, VectorAsset>;

  NavigationItem(Icon icon, StringVariant label);

  Icon icon_;
  Icon selected_icon_;
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
  DrawerStyle style_;

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
  DrawerStyle style_;

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
};

View NavigationStack(std::function<View()> root);

template <class Factory, class... Arguments>
  requires detail::ViewFactoryFor<Factory, Arguments...>
View NavigationStack(Factory&& root, Arguments&&... arguments) {
  return NavigationStack(detail::BindViewFactory(std::forward<Factory>(root), std::forward<Arguments>(arguments)...));
}

NavigationController UseNavigation();

} // namespace huxerui
