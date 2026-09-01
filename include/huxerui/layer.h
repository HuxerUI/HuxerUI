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
class SnackBarService;
class ToastService;
class TooltipService;
struct LayerPlacement;
} // namespace detail

/// Identifies one entry owned by a window's LayerController.
///
/// Identifiers are stable for the lifetime of an entry and are used by Update() and Dismiss().
using LayerId = std::uint64_t;

/// Produces declarative View content when Runtime composes a retained presentation.
using ViewFactory = std::function<View()>;

/// Selects the paint and input ordering group for a layer entry.
enum class LayerLevel {
  /// Application presentation such as dialogs, menus, and popups.
  Presentation,
  /// Non-modal feedback painted above ordinary presentations.
  Notification,
  /// Framework or application chrome that remains above other layers.
  System,
};

/// Defines how pointer hit testing interacts with a layer entry.
enum class LayerPointerPolicy {
  /// Ignores this layer for pointer hit testing so content behind it remains reachable.
  PassThrough,
  /// Routes pointer input only through the layer's visible content.
  Content,
  /// Covers the viewport with a pointer barrier and optionally dismisses on an outside press.
  Barrier,
};

/// Defines how a platform or keyboard cancel request is handled by a layer entry.
enum class LayerCancelPolicy {
  /// Leaves the request unhandled so a lower layer, navigation, or the platform can respond.
  PassThrough,
  /// Handles the request without dismissing the layer.
  Consume,
  /// Requests dismissal of the layer.
  Dismiss,
};

/// Configures input, focus, dismissal, and barrier behavior for a layer entry.
struct LayerOptions {
  /// Paint and input ordering group.
  LayerLevel level = LayerLevel::Presentation;
  /// Pointer hit-testing policy.
  LayerPointerPolicy pointer_policy = LayerPointerPolicy::Content;
  /// Whether keyboard focus is confined to this layer while it remains attached.
  bool trap_focus = false;
  /// Whether a pointer press outside content requests dismissal; requires Barrier pointer policy.
  bool dismiss_on_outside_press = false;
  /// Response to a cancel request such as Escape or platform Back.
  LayerCancelPolicy cancel_policy = LayerCancelPolicy::PassThrough;
  /// Optional owner callback that receives dismissal requests instead of automatic removal.
  std::function<void()> on_dismiss_request;
  /// Optional full-viewport barrier fill; requires Barrier pointer policy.
  std::optional<Color> barrier_color;
};

/// Controls arbitrary window-scoped content outside the application root tree.
///
/// Attached content captures the current Environment. Prefer typed presentation services such as UseDialog(),
/// UsePopup(), and UseToast() when their semantics fit; use this controller for application-specific layers.
/// @code
/// RootHook InstallGlobalBanner() {
///   return [](RootContext& root) {
///     root.Layers().Attach(
///         {.level = LayerLevel::System, .pointer_policy = LayerPointerPolicy::PassThrough},
///         [] { return GlobalBanner(); }
///     );
///   };
/// }
/// @endcode
class LayerController {
public:
  LayerController(const LayerController&) = default;
  LayerController& operator=(const LayerController&) = default;

  /// Attaches content and returns its new identifier.
  ///
  /// Throws std::invalid_argument for an empty factory or incompatible options, and std::logic_error after the
  /// controller disconnects from its Runtime.
  LayerId Attach(LayerOptions options, ViewFactory content) const;

  /// Binds copyable factory arguments and attaches the resulting content.
  template <class Factory, class... Arguments>
    requires detail::ViewFactoryFor<Factory, Arguments...>
  LayerId Attach(LayerOptions options, Factory&& content, Arguments&&... arguments) const {
    return Attach(
        std::move(options),
        detail::BindViewFactory(std::forward<Factory>(content), std::forward<Arguments>(arguments)...)
    );
  }

  /// Replaces an existing entry's content while preserving its options and identifier.
  ///
  /// Returns false when the controller is disconnected or the identifier is stale.
  bool Update(LayerId id, ViewFactory content) const;

  /// Binds copyable factory arguments and updates an existing entry's content.
  template <class Factory, class... Arguments>
    requires detail::ViewFactoryFor<Factory, Arguments...>
  bool Update(LayerId id, Factory&& content, Arguments&&... arguments) const {
    return Update(id, detail::BindViewFactory(std::forward<Factory>(content), std::forward<Arguments>(arguments)...));
  }

  /// Replaces an existing entry's options and content while preserving its identifier.
  ///
  /// Returns false when the controller is disconnected or the identifier is stale.
  bool Update(LayerId id, LayerOptions options, ViewFactory content) const;

  /// Binds copyable factory arguments and updates an existing entry's options and content.
  template <class Factory, class... Arguments>
    requires detail::ViewFactoryFor<Factory, Arguments...>
  bool Update(LayerId id, LayerOptions options, Factory&& content, Arguments&&... arguments) const {
    return Update(
        id,
        std::move(options),
        detail::BindViewFactory(std::forward<Factory>(content), std::forward<Arguments>(arguments)...)
    );
  }

  /// Dismisses an entry and returns false when the identifier is stale or already exiting.
  ///
  /// Entries configured with presentation motion remain mounted but non-interactive until their exit completes.
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
  LayerId AttachCapturedReplacing(
      std::optional<LayerId> replaced,
      LayerOptions options,
      ViewFactory content,
      std::shared_ptr<const Environment> environment,
      detail::LayerPlacement placement,
      std::shared_ptr<detail::LayerTransitionState> transition,
      std::shared_ptr<const detail::SemanticModalGroupToken> semantic_modal_group = {}
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
  friend class detail::SnackBarService;
  friend class detail::ToastService;
  friend class detail::TooltipService;
  friend struct detail::LayerAnchorState;
};

} // namespace huxerui
