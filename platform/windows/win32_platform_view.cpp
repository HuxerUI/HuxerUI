#include "win32_platform_view.h"
#include "internal_access.h"

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
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/app.h>
#include <huxerui/windows/platform_registry.h>

#include "runtime/ui_window_internal.h"
#include "macros_internal.h"

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

struct Win32PlatformViews::Impl {
  struct EventRoute {
    UiWindow* ui_window = nullptr;
    // Value-returning events are synchronous, so a route may enter UiWindow only from its owning UI thread.
    std::thread::id ui_thread;
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

  Impl(HINSTANCE instance_value, HWND platform_window_value, PlatformRegistry& registry_value,
        UiWindow& ui_window_value, OverlayMessageHandler overlay_message_handler_value)
      : instance(instance_value), platform_window(platform_window_value), registry(&registry_value), ui_window(&ui_window_value),
        overlay_message_handler(std::move(overlay_message_handler_value)) {
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
        platform_window,
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
        registry->FindView<windows::detail::Win32ViewFactory>(*ui_window, command.Type(), command.Properties().Type(),
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
        platform_window,
        nullptr,
        instance,
        nullptr
    );
    if (container == nullptr) {
      throw std::runtime_error("HuxerUI could not create a Windows PlatformView container");
    }
    hosted->container = container;

    auto route =
        std::make_shared<EventRoute>(EventRoute{ui_window, std::this_thread::get_id(), command.Identity(), false});
    const std::weak_ptr<EventRoute> weak_route = route;
    PlatformEventEmitter events = MakePlatformEventEmitter(
        [weak_route](std::type_index key, PlatformValue value) -> std::optional<PlatformValue> {
          const std::shared_ptr<EventRoute> route = weak_route.lock();
          if (!route || route->ui_thread != std::this_thread::get_id() || !route->active ||
              route->ui_window == nullptr) {
            return std::nullopt;
          }
          return InternalAccess::DispatchPlatformViewEvent(*route->ui_window, route->identity, key, value);
        },
        [weak_route](std::string name, PlatformPayload payload) -> std::optional<PlatformPayload> {
          const std::shared_ptr<EventRoute> route = weak_route.lock();
          if (!route || route->ui_thread != std::this_thread::get_id() || !route->active ||
              route->ui_window == nullptr) {
            return std::nullopt;
          }
          return InternalAccess::DispatchPlatformViewEvent(*route->ui_window, route->identity, name, payload);
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

  void ResizeOverlay() {
    if (overlay == nullptr || platform_window == nullptr) {
      return;
    }
    RECT client{};
    GetClientRect(platform_window, &client);
    SetWindowPos(overlay, HWND_TOP, 0, 0, std::max(0L, client.right - client.left),
                 std::max(0L, client.bottom - client.top),
                 SWP_NOACTIVATE | (hosted_views.empty() && retired.empty() ? SWP_HIDEWINDOW : SWP_SHOWWINDOW));
    UpdateOverlayRegion();
  }

  void UpdateOverlayRegion() {
    if (overlay == nullptr || platform_window == nullptr) {
      return;
    }
    RECT client{};
    if (!GetClientRect(platform_window, &client)) {
      return;
    }
    std::vector<RECT> holes;
    const auto collect = [this, &holes](const std::unique_ptr<HostedView>& hosted) {
      if (!hosted || !hosted->visible || hosted->container == nullptr) {
        return;
      }
      RECT rect{};
      if (!GetWindowRect(hosted->container, &rect)) {
        return;
      }
      POINT top_left{rect.left, rect.top};
      POINT bottom_right{rect.right, rect.bottom};
      if (!ScreenToClient(overlay, &top_left) || !ScreenToClient(overlay, &bottom_right)) {
        return;
      }
      holes.push_back(RECT{top_left.x, top_left.y, bottom_right.x, bottom_right.y});
    };
    for (const auto& [identity, hosted] : hosted_views) {
      static_cast<void>(identity);
      collect(hosted);
    }
    for (const auto& hosted : retired) {
      collect(hosted);
    }

    const bool unchanged =
        overlay_region_client.right == client.right && overlay_region_client.bottom == client.bottom &&
        overlay_region_holes.size() == holes.size() &&
        std::equal(holes.begin(), holes.end(), overlay_region_holes.begin(), [](const RECT& left, const RECT& right) {
          return left.left == right.left && left.top == right.top && left.right == right.right &&
                 left.bottom == right.bottom;
        });
    if (unchanged) {
      return;
    }

    // A transparent hit test alone does not reliably hand mouse input to a platform view's HWND.
    // Cut visible platform views out of the overlay's window region so the OS hit-tests them directly.
    HRGN region = CreateRectRgn(0, 0, client.right, client.bottom);
    if (region == nullptr) {
      return;
    }
    for (const RECT& hole : holes) {
      HRGN cut = CreateRectRgn(hole.left, hole.top, hole.right, hole.bottom);
      if (cut != nullptr) {
        CombineRgn(region, region, cut, RGN_DIFF);
        DeleteObject(cut);
      }
    }
    if (SetWindowRgn(overlay, region, TRUE) == 0) {
      DeleteObject(region);
      return;
    }
    overlay_region_client = client;
    overlay_region_holes = std::move(holes);
  }

  static LRESULT CALLBACK OverlayWindowProcedure(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    Impl* impl = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
      const auto* create = reinterpret_cast<const CREATESTRUCTW*>(l_param);
      impl = static_cast<Impl*>(create->lpCreateParams);
      SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(impl));
    }
    if (impl == nullptr) {
      return DefWindowProcW(window, message, w_param, l_param);
    }
    if (message == WM_NCHITTEST) {
      if (impl->overlay_message_handler &&
          impl->overlay_message_handler(window, message, w_param, l_param) != HTCLIENT) {
        // The root HWND must own resizing, dragging, and native caption behavior.
        return HTTRANSPARENT;
      }
      POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
      ScreenToClient(impl->platform_window, &point);
      const float scale = std::max(impl->dpi_scale, 0.01F);
      const std::optional<std::uint64_t> identity = InternalAccess::HitTestPlatformView(
          *impl->ui_window,
          {static_cast<float>(point.x) / scale, static_cast<float>(point.y) / scale}
      );
      if (identity.has_value()) {
        const auto found = impl->hosted_views.find(*identity);
        if (found != impl->hosted_views.end() && found->second->visible) {
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
    if (impl->overlay_message_handler) {
      return impl->overlay_message_handler(window, message, w_param, l_param);
    }
    return DefWindowProcW(window, message, w_param, l_param);
  }

  HINSTANCE instance = nullptr;
  HWND platform_window = nullptr;
  HWND overlay = nullptr;
  ATOM container_atom = 0;
  ATOM overlay_atom = 0;
  PlatformRegistry* registry = nullptr;
  UiWindow* ui_window = nullptr;
  OverlayMessageHandler overlay_message_handler;
  float dpi_scale = 1.0F;
  std::optional<std::uint64_t> platform_view_focus_identity;
  bool pending_focus_visible = false;
  std::unordered_map<std::uint64_t, std::unique_ptr<HostedView>> hosted_views;
  // Removed HWNDs stay behind the previous aperture until the replacement HuxerUI frame is presented.
  std::vector<std::unique_ptr<HostedView>> retired;
  // Last region applied to the input overlay: the client rect minus visible platform views.
  RECT overlay_region_client{};
  std::vector<RECT> overlay_region_holes;
};

Win32PlatformViews::Win32PlatformViews(HINSTANCE instance, HWND platform_window, PlatformRegistry& registry,
                                       UiWindow& ui_window, OverlayMessageHandler overlay_message_handler)
    : impl_(std::make_unique<Impl>(instance, platform_window, registry, ui_window,
                                  std::move(overlay_message_handler))) {}

Win32PlatformViews::~Win32PlatformViews() {
  Shutdown();
}

bool Win32PlatformViews::Commit(const RenderFrame& frame, float dpi_scale) {
  // Windows currently uses one HuxerUI surface, but the shared slice sequence remains the ordering contract.
  const RenderComposition composition = BuildRenderComposition(frame.scene);
  impl_->dpi_scale = std::isfinite(dpi_scale) && dpi_scale > 0.0F ? dpi_scale : 1.0F;

  std::unordered_set<std::uint64_t> retained;
  std::vector<std::pair<std::uint64_t, std::unique_ptr<Impl::HostedView>>> pending;
  std::vector<const PlatformViewPlacement*> placements;
  for (const RenderCompositionLayer& layer : composition.layers) {
    if (const auto* placement = std::get_if<PlatformViewPlacement>(&layer)) {
      const PlacePlatformViewCommand& command = *placement->command;
      retained.insert(command.Identity());
      placements.push_back(placement);
      const auto found = impl_->hosted_views.find(command.Identity());
      if (found == impl_->hosted_views.end() || found->second->type != command.Type()) {
        pending.emplace_back(command.Identity(), impl_->Create(*placement));
      } else {
        impl_->Update(*found->second, command);
      }
    }
  }

  const std::optional<std::uint64_t> platform_view_focus = impl_->IdentityForWindow(GetFocus());
  const bool focused_instance_removed =
      platform_view_focus.has_value() &&
      (!retained.contains(*platform_view_focus) ||
       std::ranges::any_of(
           pending,
           [platform_view_focus](const auto& entry) { return entry.first == *platform_view_focus; }
       ));
  if (focused_instance_removed) {
    SetFocus(impl_->platform_window);
    impl_->platform_view_focus_identity.reset();
  }
  for (auto& [identity, hosted] : pending) {
    const auto found = impl_->hosted_views.find(identity);
    if (found != impl_->hosted_views.end()) {
      impl_->Retire(std::move(found->second));
      impl_->hosted_views.erase(found);
    }
    impl_->hosted_views.emplace(identity, std::move(hosted));
  }
  for (auto iterator = impl_->hosted_views.begin(); iterator != impl_->hosted_views.end();) {
    if (retained.contains(iterator->first)) {
      ++iterator;
      continue;
    }
    impl_->Retire(std::move(iterator->second));
    iterator = impl_->hosted_views.erase(iterator);
  }
  for (const PlatformViewPlacement* placement : placements) {
    const PlacePlatformViewCommand& command = *placement->command;
    const auto found = impl_->hosted_views.find(command.Identity());
    impl_->Place(*found->second, *placement, impl_->dpi_scale);
  }

  for (auto& [identity, hosted] : impl_->hosted_views) {
    hosted->event_route->active = true;
  }
  impl_->ResizeOverlay();

  const std::optional<std::uint64_t> focused = InternalAccess::FocusedPlatformView(*impl_->ui_window);
  const std::optional<std::uint64_t> current = impl_->IdentityForWindow(GetFocus());
  if (focused.has_value()) {
    const auto found = impl_->hosted_views.find(*focused);
    if (found == impl_->hosted_views.end() || !found->second->visible) {
      if (current == focused) {
        SetFocus(impl_->platform_window);
      }
      InternalAccess::SynchronizePlatformViewFocus(*impl_->ui_window, std::nullopt, false);
    } else if (current != focused) {
      SetFocus(impl_->FocusTarget(*found->second));
      if (impl_->IdentityForWindow(GetFocus()) != focused) {
        InternalAccess::SynchronizePlatformViewFocus(*impl_->ui_window, std::nullopt, false);
      }
    }
  } else if (current.has_value()) {
    SetFocus(impl_->platform_window);
  }
  impl_->platform_view_focus_identity = impl_->IdentityForWindow(GetFocus());
  return !impl_->hosted_views.empty() || !impl_->retired.empty();
}

void Win32PlatformViews::DidPresent() {
  if (!impl_->retired.empty()) {
    // Present queues the composition buffer; wait until DWM has consumed it before exposing retired apertures.
    HUXERUI_DISCARD(DwmFlush());
  }
  impl_->retired.clear();
  impl_->ResizeOverlay();
}

void Win32PlatformViews::Resize() {
  impl_->ResizeOverlay();
}

bool Win32PlatformViews::HandleFocusTraversal(const MSG& message) {
  if (message.message != WM_KEYDOWN || message.wParam != VK_TAB) {
    return false;
  }
  const HWND focused = GetFocus();
  const std::optional<std::uint64_t> identity = impl_->IdentityForWindow(focused);
  if (!identity.has_value()) {
    return false;
  }
  const auto found = impl_->hosted_views.find(*identity);
  if (found == impl_->hosted_views.end()) {
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
  impl_->pending_focus_visible = true;
  if (leaves_view) {
    HUXERUI_DISCARD(InternalAccess::MoveFocusFromPlatformView(*impl_->ui_window, *identity, reverse));
    return true;
  }
  SetFocus(reverse ? *std::prev(current) : *std::next(current));
  return true;
}

void Win32PlatformViews::SynchronizeFocus(HWND focused) {
  const std::optional<std::uint64_t> identity = impl_->IdentityForWindow(focused);
  if (identity == impl_->platform_view_focus_identity) {
    return;
  }
  const std::optional<std::uint64_t> previous = impl_->platform_view_focus_identity;
  impl_->platform_view_focus_identity = identity;
  if (identity.has_value()) {
    InternalAccess::SynchronizePlatformViewFocus(*impl_->ui_window, identity, impl_->pending_focus_visible);
    impl_->pending_focus_visible = false;
  } else if (previous.has_value() && InternalAccess::FocusedPlatformView(*impl_->ui_window) == previous) {
    InternalAccess::SynchronizePlatformViewFocus(*impl_->ui_window, std::nullopt, false);
  }
}

HWND Win32PlatformViews::AccessibilityView(std::uint64_t identity) const noexcept {
  const auto found = impl_->hosted_views.find(identity);
  return found == impl_->hosted_views.end() || !found->second->visible ? nullptr : found->second->view;
}

void Win32PlatformViews::Shutdown() noexcept {
  if (!impl_) {
    return;
  }
  for (auto& [identity, hosted] : impl_->hosted_views) {
    hosted->event_route->active = false;
    hosted->event_route->ui_window = nullptr;
  }
  impl_->hosted_views.clear();
  impl_->retired.clear();
  if (impl_->overlay != nullptr && IsWindow(impl_->overlay)) {
    DestroyWindow(impl_->overlay);
  }
  impl_->overlay = nullptr;
  impl_->ui_window = nullptr;
  if (impl_->container_atom != 0) {
    UnregisterClassW(kContainerClassName, impl_->instance);
    impl_->container_atom = 0;
  }
  if (impl_->overlay_atom != 0) {
    UnregisterClassW(kOverlayClassName, impl_->instance);
    impl_->overlay_atom = 0;
  }
}

} // namespace huxerui::detail
