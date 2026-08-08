#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include <huxerui/color.h>
#include <huxerui/view.h>

namespace huxerui {

class Runtime;
class Environment;

namespace detail {
struct LayerAnchorState;
struct SemanticModalGroupToken;
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

  template <class Factory, class... Arguments>
    requires detail::ViewFactoryFor<Factory, Arguments...>
  LayerId Attach(LayerOptions options, Factory&& content, Arguments&&... arguments) const {
    return Attach(
        std::move(options),
        detail::BindViewFactory(std::forward<Factory>(content), std::forward<Arguments>(arguments)...)
    );
  }

  bool Update(LayerId id, ViewFactory content) const;

  template <class Factory, class... Arguments>
    requires detail::ViewFactoryFor<Factory, Arguments...>
  bool Update(LayerId id, Factory&& content, Arguments&&... arguments) const {
    return Update(id, detail::BindViewFactory(std::forward<Factory>(content), std::forward<Arguments>(arguments)...));
  }

  bool Update(LayerId id, LayerOptions options, ViewFactory content) const;

  template <class Factory, class... Arguments>
    requires detail::ViewFactoryFor<Factory, Arguments...>
  bool Update(LayerId id, LayerOptions options, Factory&& content, Arguments&&... arguments) const {
    return Update(
        id,
        std::move(options),
        detail::BindViewFactory(std::forward<Factory>(content), std::forward<Arguments>(arguments)...)
    );
  }

  bool Dismiss(LayerId id) const;

private:
  struct DismissRequestResult {
    bool handled = false;
    bool dismissed = false;
  };
  struct State;

  LayerId AttachCaptured(
      LayerOptions options,
      ViewFactory content,
      std::shared_ptr<const Environment> environment,
      detail::LayerPlacement placement,
      std::shared_ptr<detail::LayerTransitionState> transition = {},
      std::shared_ptr<const detail::SemanticModalGroupToken> semantic_modal_group = {}
  ) const;
  bool UpdateCaptured(
      LayerId id,
      LayerOptions options,
      ViewFactory content,
      std::shared_ptr<const Environment> environment,
      detail::LayerPlacement placement,
      std::shared_ptr<detail::LayerTransitionState> transition
  ) const;
  bool UpdateEntry(
      LayerId id,
      std::optional<LayerOptions> options,
      ViewFactory content,
      std::optional<std::shared_ptr<const Environment>> environment
  ) const;
  bool UpdatePlacement(LayerId id, detail::LayerPlacement placement) const;
  std::optional<LayerOptions> EntryOptions(LayerId id) const;
  std::shared_ptr<detail::LayerTransitionState> Transition(LayerId id) const;
  DismissRequestResult RequestDismiss(LayerId id) const;
  void InvalidateAllEntries() const;
  void BindTransitionCompletion(LayerId id, const std::shared_ptr<detail::LayerTransitionState>& transition) const;

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
