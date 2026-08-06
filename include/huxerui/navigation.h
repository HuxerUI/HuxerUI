#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include <huxerui/animation.h>
#include <huxerui/geometry.h>
#include <huxerui/view.h>

namespace huxerui {

namespace detail {
class NavigationState;
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
