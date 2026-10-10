#include "linux_internal.h"

#include "linux_platform_view.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/linux/platform_registry.h>

#include "application/platform_registry_internal.h"
#include "internal_access.h"
#include "linux_renderer.h"
#include "runtime/mounted_node_internal.h"

namespace huxerui::detail {
namespace {

Rect VisibleBounds(const PlatformViewPlacement& placement) noexcept {
  if (!placement.visible) {
    return {};
  }
  return placement.clip.has_value() ? placement.world_bounds.Intersection(*placement.clip) : placement.world_bounds;
}

struct EventRoute {
  UiWindow* ui_window = nullptr;
  std::thread::id ui_thread;
  std::uint64_t identity = 0;
  bool active = false;
};

} // namespace

struct LinuxPlatformViews::State {
  struct FocusRoute {
    State* state = nullptr;
    std::uint64_t identity = 0;
  };

  struct HostedView {
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
      if (focus_controller != nullptr) {
        g_signal_handlers_disconnect_by_data(focus_controller, focus_enter_data);
        g_signal_handlers_disconnect_by_data(focus_controller, focus_leave_data);
      }
      delete focus_enter_data;
      delete focus_leave_data;
      if (container != nullptr) {
        if (gtk_widget_get_parent(container) != nullptr) {
          gtk_widget_unparent(container);
        }
        g_object_unref(container);
      }
    }

    std::uint64_t properties_revision = 0;
    std::uint64_t controller_revision = 0;
    std::string type;
    std::shared_ptr<EventRoute> event_route;
    std::shared_ptr<const linux::detail::LinuxViewFactory> factory;
    std::shared_ptr<void> instance;
    PlatformValue controller;
    GtkWidget* container = nullptr;
    GtkWidget* view = nullptr;
    GtkEventController* focus_controller = nullptr;
    FocusRoute* focus_enter_data = nullptr;
    FocusRoute* focus_leave_data = nullptr;
    Rect world_bounds;
    Rect visible_bounds;
    bool visible = false;
    bool controller_connected = false;
  };

  State(GtkWidget* host_value, LinuxRenderer& renderer_value, PlatformRegistry& registry_value, UiWindow& ui_window_value)
      : host(host_value), renderer(&renderer_value), registry(&registry_value), ui_window(&ui_window_value) {
    if (host == nullptr) {
      throw std::invalid_argument("HuxerUI Linux PlatformView host must not be null");
    }
  }

  static void DispatchFocusEnter(GtkEventControllerFocus*, gpointer data) {
    auto* route = static_cast<FocusRoute*>(data);
    if (route != nullptr && route->state != nullptr) {
      route->state->FocusChanged(route->identity, true);
    }
  }

  static void DispatchFocusLeave(GtkEventControllerFocus*, gpointer data) {
    auto* route = static_cast<FocusRoute*>(data);
    if (route != nullptr && route->state != nullptr) {
      route->state->FocusChanged(route->identity, false);
    }
  }

  void FocusChanged(std::uint64_t identity, bool focused) {
    if (ui_window == nullptr) {
      return;
    }
    if (focused) {
      focused_identity = identity;
      InternalAccess::SynchronizePlatformViewFocus(*ui_window, identity, true);
      if (focus_changed) {
        focus_changed(true);
      }
      return;
    }
    if (focused_identity == identity) {
      focused_identity.reset();
      InternalAccess::SynchronizePlatformViewFocus(*ui_window, std::nullopt, false);
      if (focus_changed) {
        focus_changed(false);
      }
    }
  }

  std::unique_ptr<HostedView> Create(const PlatformViewPlacement& placement) {
    const PlacePlatformViewCommand& command = *placement.command;
    auto hosted = std::make_unique<HostedView>();
    hosted->properties_revision = command.PropertiesRevision();
    hosted->controller_revision = command.ControllerRevision();
    hosted->type = command.Type();
    hosted->controller = command.Controller();
    hosted->event_route = std::make_shared<EventRoute>(EventRoute{
        ui_window,
        std::this_thread::get_id(),
        command.Identity(),
        false,
    });
    hosted->factory = registry->FindView<linux::detail::LinuxViewFactory>(
        *ui_window, command.Type(), command.Properties().Type(), command.Controller().Type());
    if (!hosted->factory->create || !hosted->factory->view) {
      throw std::logic_error("HuxerUI Linux PlatformView factory must provide create and view");
    }

    GtkWidget* container = gtk_fixed_new();
    gtk_widget_set_overflow(container, GTK_OVERFLOW_HIDDEN);
    gtk_widget_set_visible(container, TRUE);
    gtk_widget_set_parent(container, host);
    g_object_ref(container);
    hosted->container = container;

    const std::weak_ptr<EventRoute> weak_route = hosted->event_route;
    PlatformEventEmitter events = MakePlatformEventEmitter(
        [weak_route](std::type_index key, PlatformValue value) -> std::optional<PlatformValue> {
          const std::shared_ptr<EventRoute> route = weak_route.lock();
          if (!route || route->ui_thread != std::this_thread::get_id() || !route->active || route->ui_window == nullptr) {
            return std::nullopt;
          }
          return InternalAccess::DispatchPlatformViewEvent(*route->ui_window, route->identity, key, value);
        },
        [weak_route](std::string name, PlatformPayload payload) -> std::optional<PlatformPayload> {
          const std::shared_ptr<EventRoute> route = weak_route.lock();
          if (!route || route->ui_thread != std::this_thread::get_id() || !route->active || route->ui_window == nullptr) {
            return std::nullopt;
          }
          return InternalAccess::DispatchPlatformViewEvent(*route->ui_window, route->identity, name, payload);
        });

    hosted->instance = hosted->factory->create(container, command.Properties(), std::move(events));
    if (!hosted->instance) {
      throw std::logic_error("HuxerUI Linux PlatformView factory returned an empty instance");
    }
    hosted->view = hosted->factory->view(hosted->instance);
    if (hosted->view == nullptr || !GTK_IS_WIDGET(hosted->view) || gtk_widget_get_parent(hosted->view) != container) {
      hosted->view = nullptr;
      throw std::logic_error(
          "HuxerUI Linux PlatformView factory must return a GTK child of its supplied clipping container");
    }

    hosted->focus_controller = gtk_event_controller_focus_new();
    hosted->focus_enter_data = new FocusRoute{this, command.Identity()};
    hosted->focus_leave_data = new FocusRoute{this, command.Identity()};
    g_signal_connect(hosted->focus_controller, "enter", G_CALLBACK(DispatchFocusEnter), hosted->focus_enter_data);
    g_signal_connect(hosted->focus_controller, "leave", G_CALLBACK(DispatchFocusLeave), hosted->focus_leave_data);
    gtk_widget_add_controller(hosted->container, hosted->focus_controller);

    if (hosted->controller.HasValue()) {
      if (!hosted->factory->connect || !hosted->factory->disconnect) {
        throw std::logic_error("HuxerUI controlled Linux PlatformView factory must provide connect and disconnect");
      }
      hosted->factory->connect(hosted->instance, hosted->controller);
      hosted->controller_connected = true;
    }
    return hosted;
  }

  void Update(HostedView& hosted, const PlacePlatformViewCommand& command) {
    if (hosted.properties_revision != command.PropertiesRevision()) {
      if (!hosted.factory->update) {
        throw std::logic_error("HuxerUI Linux PlatformView factory does not support property updates");
      }
      hosted.factory->update(hosted.instance, command.Properties());
      hosted.properties_revision = command.PropertiesRevision();
    }
    if (hosted.controller_revision != command.ControllerRevision()) {
      if (hosted.controller_connected) {
        hosted.factory->disconnect(hosted.instance, hosted.controller);
        hosted.controller_connected = false;
      }
      hosted.controller = command.Controller();
      if (hosted.controller.HasValue()) {
        if (!hosted.factory->connect) {
          throw std::logic_error("HuxerUI controlled Linux PlatformView factory must provide connect");
        }
        hosted.factory->connect(hosted.instance, hosted.controller);
        hosted.controller_connected = true;
      }
      hosted.controller_revision = command.ControllerRevision();
    }
  }

  void Place(HostedView& hosted, const PlatformViewPlacement& placement) {
    hosted.world_bounds = placement.world_bounds;
    hosted.visible_bounds = VisibleBounds(placement);
    hosted.visible = placement.visible && !hosted.visible_bounds.IsEmpty();
    gtk_widget_set_child_visible(hosted.container, hosted.visible);
    if (!hosted.visible) {
      return;
    }

    const double container_x = std::floor(hosted.visible_bounds.x);
    const double container_y = std::floor(hosted.visible_bounds.y);
    const int view_width = std::max(1, static_cast<int>(std::ceil(hosted.world_bounds.width)));
    const int view_height = std::max(1, static_cast<int>(std::ceil(hosted.world_bounds.height)));
    gtk_widget_set_size_request(hosted.view, view_width, view_height);
    gtk_fixed_move(GTK_FIXED(hosted.container), hosted.view,
                   static_cast<double>(hosted.world_bounds.x) - container_x,
                   static_cast<double>(hosted.world_bounds.y) - container_y);
  }

  void Retire(std::unique_ptr<HostedView> hosted) {
    hosted->event_route->active = false;
    if (focused_identity == hosted->event_route->identity) {
      gtk_widget_grab_focus(host);
      focused_identity.reset();
      if (ui_window != nullptr) {
        InternalAccess::SynchronizePlatformViewFocus(*ui_window, std::nullopt, false);
      }
      if (focus_changed) {
        focus_changed(false);
      }
    }
  }

  GtkWidget* host = nullptr;
  LinuxRenderer* renderer = nullptr;
  PlatformRegistry* registry = nullptr;
  UiWindow* ui_window = nullptr;
  RenderComposition composition;
  std::unordered_map<std::uint64_t, std::unique_ptr<HostedView>> hosted_views;
  std::optional<std::uint64_t> focused_identity;
  bool native_pointer_sequence = false;
  std::function<void(bool)> focus_changed;
};

LinuxPlatformViews::LinuxPlatformViews(GtkWidget* host, LinuxRenderer& renderer, PlatformRegistry& registry,
                                       UiWindow& ui_window)
    : state_(std::make_unique<State>(host, renderer, registry, ui_window)) {}

LinuxPlatformViews::~LinuxPlatformViews() {
  Shutdown();
}

void LinuxPlatformViews::Commit(const RenderFrame& frame) {
  RenderComposition composition = BuildRenderComposition(frame.scene);
  std::unordered_set<std::uint64_t> retained;
  std::vector<std::pair<std::uint64_t, std::unique_ptr<State::HostedView>>> pending;
  std::vector<const PlatformViewPlacement*> placements;

  for (const RenderCompositionLayer& layer : composition.layers) {
    const auto* placement = std::get_if<PlatformViewPlacement>(&layer);
    if (placement == nullptr) {
      continue;
    }
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

  const bool focused_view_removed = state_->focused_identity.has_value() &&
      (!retained.contains(*state_->focused_identity) ||
       std::ranges::any_of(pending, [this](const auto& entry) { return entry.first == *state_->focused_identity; }));
  if (focused_view_removed) {
    gtk_widget_grab_focus(state_->host);
    state_->focused_identity.reset();
    InternalAccess::SynchronizePlatformViewFocus(*state_->ui_window, std::nullopt, false);
    if (state_->focus_changed) {
      state_->focus_changed(false);
    }
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
    const auto found = state_->hosted_views.find(placement->command->Identity());
    if (found == state_->hosted_views.end()) {
      throw std::logic_error("HuxerUI Linux PlatformView disappeared during composition commit");
    }
    state_->Place(*found->second, *placement);
    found->second->event_route->active = true;
  }
  state_->composition = std::move(composition);
  Allocate(gtk_widget_get_width(state_->host), gtk_widget_get_height(state_->host));
}

void LinuxPlatformViews::Allocate(int width, int height) {
  if (width <= 0 || height <= 0 || state_->host == nullptr) {
    return;
  }
  for (auto& [identity, hosted] : state_->hosted_views) {
    static_cast<void>(identity);
    if (!hosted->visible) {
      gtk_widget_set_child_visible(hosted->container, FALSE);
      continue;
    }
    const double x = std::floor(hosted->visible_bounds.x);
    const double y = std::floor(hosted->visible_bounds.y);
    const int right = static_cast<int>(std::ceil(hosted->visible_bounds.x + hosted->visible_bounds.width));
    const int bottom = static_cast<int>(std::ceil(hosted->visible_bounds.y + hosted->visible_bounds.height));
    const int child_width = std::max(1, right - static_cast<int>(x));
    const int child_height = std::max(1, bottom - static_cast<int>(y));
    graphene_point_t offset;
    graphene_point_init(&offset, static_cast<float>(x), static_cast<float>(y));
    gtk_widget_allocate(hosted->container, child_width, child_height, -1, gsk_transform_translate(nullptr, &offset));
  }
}

void LinuxPlatformViews::Snapshot(GtkSnapshot* snapshot, const RenderFrame& frame) {
  for (const RenderCompositionLayer& layer : state_->composition.layers) {
    if (const auto* slice = std::get_if<RenderSlice>(&layer)) {
      state_->renderer->SnapshotSlice(snapshot, frame, slice->first_command, slice->command_count);
      continue;
    }
    const auto& placement = std::get<PlatformViewPlacement>(layer);
    const auto found = state_->hosted_views.find(placement.command->Identity());
    if (found != state_->hosted_views.end() && found->second->visible) {
      gtk_widget_snapshot_child(state_->host, found->second->container, snapshot);
    }
  }
}

bool LinuxPlatformViews::RouteToHuxerUI(Point position, PointerEventType type, bool huxerui_pointer_capture) {
  if (state_->native_pointer_sequence) {
    if (type == PointerEventType::Up || type == PointerEventType::Cancel) {
      state_->native_pointer_sequence = false;
    }
    return false;
  }
  if (huxerui_pointer_capture) {
    return true;
  }
  const std::optional<std::uint64_t> identity = InternalAccess::HitTestPlatformView(*state_->ui_window, position);
  const auto found = identity.has_value() ? state_->hosted_views.find(*identity) : state_->hosted_views.end();
  if (found == state_->hosted_views.end() || !found->second->visible) {
    return true;
  }
  if (type == PointerEventType::Down) {
    state_->native_pointer_sequence = true;
  }
  return false;
}

bool LinuxPlatformViews::HasPlatformFocus() const noexcept {
  return state_->focused_identity.has_value();
}

void LinuxPlatformViews::SetFocusChangedHandler(std::function<void(bool)> handler) {
  state_->focus_changed = std::move(handler);
}

void LinuxPlatformViews::Shutdown() noexcept {
  if (!state_) {
    return;
  }
  for (auto& [identity, hosted] : state_->hosted_views) {
    static_cast<void>(identity);
    if (hosted->event_route) {
      hosted->event_route->active = false;
    }
  }
  state_->hosted_views.clear();
  state_->composition.layers.clear();
  state_->focused_identity.reset();
  state_->native_pointer_sequence = false;
  state_->host = nullptr;
  state_->ui_window = nullptr;
}

} // namespace huxerui::detail
