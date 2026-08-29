#include "win32_platform_view.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/app.h>
#include <huxerui/windows/platform_registry.h>

#include "internal.h"

namespace huxerui::detail {

namespace {

constexpr wchar_t kContainerClassName[] = L"HuxerUI.Win32.PlatformViewContainer";
constexpr wchar_t kOverlayClassName[] = L"HuxerUI.Win32.PlatformViewOverlay";

RECT PixelBounds(Rect bounds, float scale) noexcept {
  return {
      static_cast<LONG>(std::floor(bounds.x * scale)),
      static_cast<LONG>(std::floor(bounds.y * scale)),
      static_cast<LONG>(std::ceil((bounds.x + bounds.width) * scale)),
      static_cast<LONG>(std::ceil((bounds.y + bounds.height) * scale)),
  };
}

Rect VisibleBounds(const PlatformViewPlacement& placement) noexcept {
  if (!placement.visible) {
    return {};
  }
  return placement.clip.has_value() ? placement.world_bounds.Intersection(*placement.clip) : placement.world_bounds;
}

bool IsDescendant(HWND window, HWND ancestor) noexcept {
  return window != nullptr && ancestor != nullptr && (window == ancestor || IsChild(ancestor, window) != FALSE);
}

LRESULT CALLBACK ContainerWindowProcedure(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
  if (message == WM_ERASEBKGND) {
    return 1;
  }
  if (message == WM_GETOBJECT) {
    return 0;
  }
  return DefWindowProcW(window, message, w_param, l_param);
}

} // namespace

struct Win32PlatformViews::State {
  struct EventRoute {
    Runtime* runtime = nullptr;
    UIThreadDispatcher dispatch;
    std::uint64_t identity = 0;
    bool active = false;
  };

  struct HostedView {
    std::uint64_t properties_revision = 0;
    std::uint64_t controller_revision = 0;
    std::string type;
    std::shared_ptr<EventRoute> event_route;
    std::shared_ptr<const windows::detail::Win32ViewFactory> factory;
    std::shared_ptr<void> instance;
    PlatformValue controller;
    HWND container = nullptr;
    HWND view = nullptr;
    bool visible = false;
    bool controller_connected = false;

    ~HostedView() {
      if (event_route) {
        event_route->active = false;
      }
      if (instance && controller_connected && factory && factory->disconnect) {
        try {
          factory->disconnect(instance, controller);
        } catch (...) {
        }
      }
      if (instance && factory && factory->dispose) {
        try {
          factory->dispose(instance);
        } catch (...) {
        }
      }
      if (view != nullptr && IsWindow(view)) {
        DestroyWindow(view);
      }
      if (container != nullptr && IsWindow(container)) {
        DestroyWindow(container);
      }
    }
  };

  State(HINSTANCE instance_value, HWND root_value, PlatformRegistry& registry_value, Runtime& runtime_value,
        UIThreadDispatcher dispatch_value, OverlayMessageHandler overlay_message_handler_value)
      : instance(instance_value), root(root_value), registry(&registry_value), runtime(&runtime_value),
        dispatch(std::move(dispatch_value)), overlay_message_handler(std::move(overlay_message_handler_value)) {
    RegisterClasses();
    overlay = CreateWindowExW(
        WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
        kOverlayClassName,
        L"",
        WS_CHILD | WS_CLIPSIBLINGS,
        0,
        0,
        0,
        0,
        root,
        nullptr,
        instance,
        this
    );
    if (overlay == nullptr) {
      if (overlay_atom != 0) {
        UnregisterClassW(kOverlayClassName, instance);
        overlay_atom = 0;
      }
      if (container_atom != 0) {
        UnregisterClassW(kContainerClassName, instance);
        container_atom = 0;
      }
      throw std::runtime_error("HuxerUI could not create the Windows PlatformView input overlay");
    }
  }

  void RegisterClasses() {
    WNDCLASSEXW container_class{
        sizeof(WNDCLASSEXW),
        0,
        ContainerWindowProcedure,
        0,
        0,
        instance,
        nullptr,
        LoadCursor(nullptr, IDC_ARROW),
        nullptr,
        nullptr,
        kContainerClassName,
        nullptr,
    };
    container_atom = RegisterClassExW(&container_class);
    if (container_atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
      throw std::runtime_error("HuxerUI could not register the Windows PlatformView container class");
    }

    WNDCLASSEXW overlay_class{
        sizeof(WNDCLASSEXW),
        CS_DBLCLKS,
        OverlayWindowProcedure,
        0,
        0,
        instance,
        nullptr,
        LoadCursor(nullptr, IDC_ARROW),
        nullptr,
        nullptr,
        kOverlayClassName,
        nullptr,
    };
    overlay_atom = RegisterClassExW(&overlay_class);
    if (overlay_atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
      if (container_atom != 0) {
        UnregisterClassW(kContainerClassName, instance);
        container_atom = 0;
      }
      throw std::runtime_error("HuxerUI could not register the Windows PlatformView input overlay class");
    }
  }

  std::unique_ptr<HostedView> Create(const PlatformViewPlacement& placement) {
    const PlacePlatformViewCommand& command = *placement.command;
    std::shared_ptr<const windows::detail::Win32ViewFactory> factory =
        registry->FindView<windows::detail::Win32ViewFactory>(command.Type(), command.Properties().Type(),
                                                              command.Controller().Type());
    if (!factory->create) {
      throw std::logic_error("HuxerUI Windows PlatformView factory must provide create");
    }

    auto hosted = std::make_unique<HostedView>();
    hosted->properties_revision = command.PropertiesRevision();
    hosted->controller_revision = command.ControllerRevision();
    hosted->type = command.Type();
    hosted->factory = std::move(factory);
    hosted->controller = command.Controller();

    HWND container = CreateWindowExW(
        WS_EX_CONTROLPARENT,
        kContainerClassName,
        L"",
        WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0,
        0,
        0,
        0,
        root,
        nullptr,
        instance,
        nullptr
    );
    if (container == nullptr) {
      throw std::runtime_error("HuxerUI could not create a Windows PlatformView container");
    }
    hosted->container = container;

    auto route = std::make_shared<EventRoute>(EventRoute{runtime, dispatch, command.Identity(), false});
    const std::weak_ptr<EventRoute> weak_route = route;
    PlatformEventEmitter events = MakePlatformEventEmitter(
        [weak_route](std::type_index key, PlatformValue value) mutable {
          const std::shared_ptr<EventRoute> route = weak_route.lock();
          if (!route || !route->dispatch) {
            return;
          }
          route->dispatch([weak_route, key, value = std::move(value)]() mutable {
            const std::shared_ptr<EventRoute> active_route = weak_route.lock();
            if (!active_route || !active_route->active || active_route->runtime == nullptr) {
              return;
            }
            static_cast<void>(
                RuntimeAccess::DispatchPlatformViewEvent(*active_route->runtime, active_route->identity, key, value));
          });
        },
        [weak_route](std::string name, PlatformPayload payload) mutable {
          const std::shared_ptr<EventRoute> route = weak_route.lock();
          if (!route || !route->dispatch) {
            return;
          }
          route->dispatch([weak_route, name = std::move(name), payload = std::move(payload)]() mutable {
            const std::shared_ptr<EventRoute> active_route = weak_route.lock();
            if (!active_route || !active_route->active || active_route->runtime == nullptr) {
              return;
            }
            static_cast<void>(RuntimeAccess::DispatchPlatformViewEvent(*active_route->runtime, active_route->identity,
                                                                       name, payload));
          });
        });

    hosted->event_route = std::move(route);
    hosted->instance = hosted->factory->create(container, command.Properties(), std::move(events));
    if (!hosted->instance || !hosted->factory->view) {
      throw std::logic_error("HuxerUI Windows PlatformView factory returned an empty instance");
    }
    hosted->view = hosted->factory->view(hosted->instance);
    if (hosted->view == nullptr || !IsWindow(hosted->view)) {
      throw std::logic_error("HuxerUI Windows PlatformView factory returned an invalid HWND");
    }

    DWORD process = 0;
    const DWORD thread = GetWindowThreadProcessId(hosted->view, &process);
    const LONG_PTR style = GetWindowLongPtrW(hosted->view, GWL_STYLE);
    if (GetParent(hosted->view) != hosted->container || process != GetCurrentProcessId() ||
        thread != GetCurrentThreadId() || (style & WS_CHILD) == 0) {
      if (process != GetCurrentProcessId() || thread != GetCurrentThreadId()) {
        hosted->view = nullptr;
      }
      throw std::logic_error("HuxerUI Windows PlatformView factory must return a same-process, same-thread child HWND");
    }

    if (hosted->controller.HasValue()) {
      if (!hosted->factory->connect || !hosted->factory->disconnect) {
        throw std::logic_error("HuxerUI controlled Windows PlatformView factory must provide connect and disconnect");
      }
      hosted->factory->connect(hosted->instance, hosted->controller);
      hosted->controller_connected = true;
    }
    return hosted;
  }

  void Update(HostedView& hosted, const PlacePlatformViewCommand& command) {
    if (hosted.properties_revision != command.PropertiesRevision()) {
      if (!hosted.factory->update) {
        throw std::logic_error("HuxerUI Windows PlatformView factory does not support property updates");
      }
      hosted.factory->update(hosted.instance, command.Properties());
      hosted.properties_revision = command.PropertiesRevision();
    }
    if (hosted.controller_revision != command.ControllerRevision()) {
      if (hosted.controller_connected) {
        if (!hosted.factory->disconnect) {
          throw std::logic_error("HuxerUI controlled Windows PlatformView factory must provide disconnect");
        }
        hosted.factory->disconnect(hosted.instance, hosted.controller);
        hosted.controller_connected = false;
      }
      hosted.controller = command.Controller();
      if (hosted.controller.HasValue()) {
        if (!hosted.factory->connect) {
          throw std::logic_error("HuxerUI controlled Windows PlatformView factory must provide connect");
        }
        hosted.factory->connect(hosted.instance, hosted.controller);
        hosted.controller_connected = true;
      }
      hosted.controller_revision = command.ControllerRevision();
    }
  }

  void Retire(std::unique_ptr<HostedView> hosted_view) {
    hosted_view->event_route->active = false;
    if (hosted_view->visible) {
      retired.push_back(std::move(hosted_view));
    }
  }

  void Place(HostedView& hosted, const PlatformViewPlacement& placement, float scale) {
    const Rect visible_bounds = VisibleBounds(placement);
    const bool visible = placement.visible && !visible_bounds.IsEmpty();
    hosted.visible = visible;
    if (!visible) {
      ShowWindow(hosted.container, SW_HIDE);
      return;
    }

    const RECT container = PixelBounds(visible_bounds, scale);
    const RECT view = PixelBounds(
        {
            placement.world_bounds.x - visible_bounds.x,
            placement.world_bounds.y - visible_bounds.y,
            placement.world_bounds.width,
            placement.world_bounds.height,
        },
        scale
    );
    // The view and clipping container have different parents, so Win32 forbids batching them in one transaction.
    if (!SetWindowPos(
            hosted.view,
            nullptr,
            view.left,
            view.top,
            std::max(0L, view.right - view.left),
            std::max(0L, view.bottom - view.top),
            SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW
        )) {
      throw std::runtime_error("HuxerUI could not position a Windows PlatformView HWND");
    }
    if (!SetWindowPos(
            hosted.container,
            HWND_TOP,
            container.left,
            container.top,
            std::max(0L, container.right - container.left),
            std::max(0L, container.bottom - container.top),
            SWP_NOACTIVATE | SWP_SHOWWINDOW
        )) {
      throw std::runtime_error("HuxerUI could not position a Windows PlatformView container");
    }
  }

  std::optional<std::uint64_t> IdentityForWindow(HWND window) const noexcept {
    for (const auto& [identity, hosted] : hosted_views) {
      if (IsDescendant(window, hosted->view)) {
        return identity;
      }
    }
    return std::nullopt;
  }

  HWND FocusTarget(const HostedView& hosted) const noexcept {
    const HWND first_tab_stop = GetNextDlgTabItem(hosted.container, nullptr, FALSE);
    return first_tab_stop != nullptr ? first_tab_stop : hosted.view;
  }

  void ResizeOverlay() const noexcept {
    if (overlay == nullptr || root == nullptr) {
      return;
    }
    RECT client{};
    GetClientRect(root, &client);
    SetWindowPos(overlay, HWND_TOP, 0, 0, std::max(0L, client.right - client.left),
                 std::max(0L, client.bottom - client.top),
                 SWP_NOACTIVATE | (hosted_views.empty() && retired.empty() ? SWP_HIDEWINDOW : SWP_SHOWWINDOW));
  }

  static LRESULT CALLBACK OverlayWindowProcedure(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    State* state = reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
      const auto* create = reinterpret_cast<const CREATESTRUCTW*>(l_param);
      state = static_cast<State*>(create->lpCreateParams);
      SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state == nullptr) {
      return DefWindowProcW(window, message, w_param, l_param);
    }
    if (message == WM_NCHITTEST) {
      if (state->overlay_message_handler &&
          state->overlay_message_handler(window, message, w_param, l_param) != HTCLIENT) {
        // The root HWND must own resizing, dragging, and native caption behavior.
        return HTTRANSPARENT;
      }
      POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
      ScreenToClient(state->root, &point);
      const float scale = std::max(state->dpi_scale, 0.01F);
      const std::optional<std::uint64_t> identity = RuntimeAccess::HitTestPlatformView(
          *state->runtime,
          {static_cast<float>(point.x) / scale, static_cast<float>(point.y) / scale}
      );
      if (identity.has_value()) {
        const auto found = state->hosted_views.find(*identity);
        if (found != state->hosted_views.end() && found->second->visible) {
          return HTTRANSPARENT;
        }
      }
      return HTCLIENT;
    }
    if (message == WM_ERASEBKGND) {
      return 1;
    }
    if (message == WM_GETOBJECT) {
      return 0;
    }
    if (state->overlay_message_handler) {
      return state->overlay_message_handler(window, message, w_param, l_param);
    }
    return DefWindowProcW(window, message, w_param, l_param);
  }

  HINSTANCE instance = nullptr;
  HWND root = nullptr;
  HWND overlay = nullptr;
  ATOM container_atom = 0;
  ATOM overlay_atom = 0;
  PlatformRegistry* registry = nullptr;
  Runtime* runtime = nullptr;
  UIThreadDispatcher dispatch;
  OverlayMessageHandler overlay_message_handler;
  float dpi_scale = 1.0F;
  std::optional<std::uint64_t> platform_view_focus_identity;
  bool pending_focus_visible = false;
  std::unordered_map<std::uint64_t, std::unique_ptr<HostedView>> hosted_views;
  // Removed HWNDs stay behind the previous aperture until the replacement HuxerUI frame is presented.
  std::vector<std::unique_ptr<HostedView>> retired;
};

Win32PlatformViews::Win32PlatformViews(HINSTANCE instance, HWND root, PlatformRegistry& registry, Runtime& runtime,
                                       UIThreadDispatcher dispatch_to_ui_thread,
                                       OverlayMessageHandler overlay_message_handler)
    : state_(std::make_unique<State>(instance, root, registry, runtime, std::move(dispatch_to_ui_thread),
                                     std::move(overlay_message_handler))) {}

Win32PlatformViews::~Win32PlatformViews() {
  Shutdown();
}

bool Win32PlatformViews::Commit(const RenderFrame& frame, float dpi_scale) {
  // Windows currently uses one HuxerUI surface, but the shared slice sequence remains the ordering contract.
  const RenderComposition composition = BuildRenderComposition(frame.scene);
  state_->dpi_scale = std::isfinite(dpi_scale) && dpi_scale > 0.0F ? dpi_scale : 1.0F;

  std::unordered_set<std::uint64_t> retained;
  std::vector<std::pair<std::uint64_t, std::unique_ptr<State::HostedView>>> pending;
  std::vector<const PlatformViewPlacement*> placements;
  for (const RenderCompositionLayer& layer : composition.layers) {
    if (const auto* placement = std::get_if<PlatformViewPlacement>(&layer)) {
      const PlacePlatformViewCommand& command = *placement->command;
      retained.insert(command.Identity());
      placements.push_back(placement);
      const auto found = state_->hosted_views.find(command.Identity());
      if (found == state_->hosted_views.end() || found->second->type != command.Type()) {
        pending.emplace_back(command.Identity(), state_->Create(*placement));
      } else {
        state_->Update(*found->second, command);
      }
    }
  }

  const std::optional<std::uint64_t> platform_view_focus = state_->IdentityForWindow(GetFocus());
  const bool focused_instance_removed =
      platform_view_focus.has_value() &&
      (!retained.contains(*platform_view_focus) ||
       std::ranges::any_of(
           pending,
           [platform_view_focus](const auto& entry) { return entry.first == *platform_view_focus; }
       ));
  if (focused_instance_removed) {
    SetFocus(state_->root);
    state_->platform_view_focus_identity.reset();
  }
  for (auto& [identity, hosted] : pending) {
    const auto found = state_->hosted_views.find(identity);
    if (found != state_->hosted_views.end()) {
      state_->Retire(std::move(found->second));
      state_->hosted_views.erase(found);
    }
    state_->hosted_views.emplace(identity, std::move(hosted));
  }
  for (auto iterator = state_->hosted_views.begin(); iterator != state_->hosted_views.end();) {
    if (retained.contains(iterator->first)) {
      ++iterator;
      continue;
    }
    state_->Retire(std::move(iterator->second));
    iterator = state_->hosted_views.erase(iterator);
  }
  for (const PlatformViewPlacement* placement : placements) {
    const PlacePlatformViewCommand& command = *placement->command;
    const auto found = state_->hosted_views.find(command.Identity());
    state_->Place(*found->second, *placement, state_->dpi_scale);
  }

  for (auto& [identity, hosted] : state_->hosted_views) {
    static_cast<void>(identity);
    hosted->event_route->active = true;
  }
  state_->ResizeOverlay();

  const std::optional<std::uint64_t> focused = RuntimeAccess::FocusedPlatformView(*state_->runtime);
  const std::optional<std::uint64_t> current = state_->IdentityForWindow(GetFocus());
  if (focused.has_value()) {
    const auto found = state_->hosted_views.find(*focused);
    if (found == state_->hosted_views.end() || !found->second->visible) {
      if (current == focused) {
        SetFocus(state_->root);
      }
      RuntimeAccess::SynchronizePlatformViewFocus(*state_->runtime, std::nullopt, false);
    } else if (current != focused) {
      SetFocus(state_->FocusTarget(*found->second));
      if (state_->IdentityForWindow(GetFocus()) != focused) {
        RuntimeAccess::SynchronizePlatformViewFocus(*state_->runtime, std::nullopt, false);
      }
    }
  } else if (current.has_value()) {
    SetFocus(state_->root);
  }
  state_->platform_view_focus_identity = state_->IdentityForWindow(GetFocus());
  return !state_->hosted_views.empty() || !state_->retired.empty();
}

void Win32PlatformViews::DidPresent() {
  if (!state_->retired.empty()) {
    // Present queues the composition buffer; wait until DWM has consumed it before exposing retired apertures.
    static_cast<void>(DwmFlush());
  }
  state_->retired.clear();
  state_->ResizeOverlay();
}

void Win32PlatformViews::Resize() {
  state_->ResizeOverlay();
}

bool Win32PlatformViews::HandleFocusTraversal(const MSG& message) {
  if (message.message != WM_KEYDOWN || message.wParam != VK_TAB) {
    return false;
  }
  const HWND focused = GetFocus();
  const std::optional<std::uint64_t> identity = state_->IdentityForWindow(focused);
  if (!identity.has_value()) {
    return false;
  }
  const auto found = state_->hosted_views.find(*identity);
  if (found == state_->hosted_views.end()) {
    return false;
  }
  std::vector<HWND> tab_order;
  HWND candidate = GetNextDlgTabItem(found->second->container, nullptr, FALSE);
  while (candidate != nullptr && std::ranges::find(tab_order, candidate) == tab_order.end()) {
    tab_order.push_back(candidate);
    candidate = GetNextDlgTabItem(found->second->container, candidate, FALSE);
  }
  const auto current = std::ranges::find_if(tab_order, [focused](HWND item) { return IsDescendant(focused, item); });
  const bool reverse = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
  const bool leaves_view = current == tab_order.end() || (reverse && current == tab_order.begin()) ||
                           (!reverse && std::next(current) == tab_order.end());
  state_->pending_focus_visible = true;
  if (leaves_view) {
    RuntimeAccess::MoveFocusFromPlatformView(*state_->runtime, *identity, reverse);
    return true;
  }
  SetFocus(reverse ? *std::prev(current) : *std::next(current));
  return true;
}

void Win32PlatformViews::SynchronizeFocus(HWND focused) {
  const std::optional<std::uint64_t> identity = state_->IdentityForWindow(focused);
  if (identity == state_->platform_view_focus_identity) {
    return;
  }
  const std::optional<std::uint64_t> previous = state_->platform_view_focus_identity;
  state_->platform_view_focus_identity = identity;
  if (identity.has_value()) {
    RuntimeAccess::SynchronizePlatformViewFocus(*state_->runtime, identity, state_->pending_focus_visible);
    state_->pending_focus_visible = false;
  } else if (previous.has_value() && RuntimeAccess::FocusedPlatformView(*state_->runtime) == previous) {
    RuntimeAccess::SynchronizePlatformViewFocus(*state_->runtime, std::nullopt, false);
  }
}

HWND Win32PlatformViews::AccessibilityView(std::uint64_t identity) const noexcept {
  const auto found = state_->hosted_views.find(identity);
  return found == state_->hosted_views.end() || !found->second->visible ? nullptr : found->second->view;
}

void Win32PlatformViews::Shutdown() noexcept {
  if (!state_) {
    return;
  }
  for (auto& [identity, hosted] : state_->hosted_views) {
    static_cast<void>(identity);
    hosted->event_route->active = false;
    hosted->event_route->runtime = nullptr;
  }
  state_->hosted_views.clear();
  state_->retired.clear();
  if (state_->overlay != nullptr && IsWindow(state_->overlay)) {
    DestroyWindow(state_->overlay);
  }
  state_->overlay = nullptr;
  state_->runtime = nullptr;
  if (state_->container_atom != 0) {
    UnregisterClassW(kContainerClassName, state_->instance);
    state_->container_atom = 0;
  }
  if (state_->overlay_atom != 0) {
    UnregisterClassW(kOverlayClassName, state_->instance);
    state_->overlay_atom = 0;
  }
}

} // namespace huxerui::detail
