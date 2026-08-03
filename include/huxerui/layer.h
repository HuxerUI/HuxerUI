#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

#include <huxerui/color.h>

namespace huxerui {

class Runtime;
class View;
class Environment;

namespace detail {
struct LayerAnchorState;
struct LayerTransitionState;
class BottomSheetService;
class DebugOverlayInstaller;
class DialogService;
class MenuService;
class PopupService;
class ToastService;
struct LayerPlacement;
} // namespace detail

using LayerId = std::uint64_t;
using ViewFactory = std::function<View()>;

enum class LayerLevel {
  Presentation,
  Notification,
  System,
};

enum class LayerPointerPolicy {
  PassThrough,
  Content,
  Barrier,
};

enum class LayerCancelPolicy {
  PassThrough,
  Consume,
  Dismiss,
};

struct LayerOptions {
  LayerLevel level = LayerLevel::Presentation;
  LayerPointerPolicy pointer_policy = LayerPointerPolicy::Content;
  bool trap_focus = false;
  bool dismiss_on_outside_press = false;
  LayerCancelPolicy cancel_policy = LayerCancelPolicy::PassThrough;
  std::function<void()> on_dismiss_request;
  std::optional<Color> barrier_color;
};

class LayerController {
public:
  LayerController(const LayerController&) = default;
  LayerController& operator=(const LayerController&) = default;

  LayerId Attach(LayerOptions options, ViewFactory content) const;

  bool Update(LayerId id, ViewFactory content) const;
  bool Update(LayerId id, LayerOptions options, ViewFactory content) const;
  bool Dismiss(LayerId id) const;

private:
  struct State;

  LayerId AttachCaptured(
      LayerOptions options,
      ViewFactory content,
      std::shared_ptr<const Environment> environment,
      detail::LayerPlacement placement,
      std::shared_ptr<detail::LayerTransitionState> transition = {}
  ) const;
  bool UpdateCaptured(
      LayerId id, LayerOptions options, ViewFactory content, std::shared_ptr<const Environment> environment
  ) const;
  bool UpdateEntry(
      LayerId id,
      std::optional<LayerOptions> options,
      ViewFactory content,
      std::optional<std::shared_ptr<const Environment>> environment
  ) const;
  bool UpdatePlacement(LayerId id, detail::LayerPlacement placement) const;
  std::shared_ptr<detail::LayerTransitionState> Transition(LayerId id) const;

  explicit LayerController(Runtime& runtime);
  void Disconnect() noexcept;

  std::shared_ptr<State> state_;

  friend class Runtime;
  friend class detail::BottomSheetService;
  friend class detail::DebugOverlayInstaller;
  friend class detail::DialogService;
  friend class detail::MenuService;
  friend class detail::PopupService;
  friend class detail::ToastService;
  friend struct detail::LayerAnchorState;
};

} // namespace huxerui
