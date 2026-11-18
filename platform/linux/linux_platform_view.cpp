#include "linux_internal.h"
#include "linux_platform_view.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

#include <huxerui/linux/platform_registry.h>

#include "internal_access.h"
#include "runtime/mounted_node_internal.h"

namespace huxerui::detail {

namespace {

struct HuxerUIPlatformClip {
  GtkWidget parent_instance;
  Rect child_bounds;
};

struct HuxerUIPlatformClipClass {
  GtkWidgetClass parent_class;
};

G_DEFINE_TYPE(HuxerUIPlatformClip, huxerui_platform_clip, GTK_TYPE_WIDGET)

GskTransform* Translation(float x, float y) {
  const graphene_point_t point{x, y};
  return gsk_transform_translate(nullptr, &point);
}

int GtkExtent(float extent) {
  return static_cast<int>(std::min(std::ceil(static_cast<double>(extent)), static_cast<double>(G_MAXINT)));
}

void huxerui_platform_clip_class_init(HuxerUIPlatformClipClass* clip_class) {
  auto* widget_class = GTK_WIDGET_CLASS(clip_class);
  widget_class->size_allocate = [](GtkWidget* widget, int, int, int) {
    const Rect bounds = reinterpret_cast<HuxerUIPlatformClip*>(widget)->child_bounds;
    if (GtkWidget* child = gtk_widget_get_first_child(widget); child && gtk_widget_should_layout(child)) {
      gtk_widget_allocate(
          child, GtkExtent(bounds.width), GtkExtent(bounds.height), -1, Translation(bounds.x, bounds.y));
    }
  };
  widget_class->snapshot = [](GtkWidget* widget, GtkSnapshot* snapshot) {
    if (GtkWidget* child = gtk_widget_get_first_child(widget)) {
      gtk_widget_snapshot_child(widget, child, snapshot);
    }
  };
  G_OBJECT_CLASS(clip_class)->dispose = [](GObject* object) {
    if (GtkWidget* child = gtk_widget_get_first_child(GTK_WIDGET(object))) {
      gtk_widget_unparent(child);
    }
    G_OBJECT_CLASS(huxerui_platform_clip_parent_class)->dispose(object);
  };
  gtk_widget_class_set_accessible_role(widget_class, GTK_ACCESSIBLE_ROLE_PRESENTATION);
}

void huxerui_platform_clip_init(HuxerUIPlatformClip* clip) {
  gtk_widget_set_overflow(GTK_WIDGET(clip), GTK_OVERFLOW_HIDDEN);
}

struct EventRoute {
  UiWindow& ui;
  const std::thread::id thread = std::this_thread::get_id();
  std::uint64_t identity;
  bool active = false;
};

struct HostedView {
  std::string type;
  std::shared_ptr<const linux::detail::GtkViewFactory> factory;
  std::shared_ptr<EventRoute> route;
  std::shared_ptr<void> instance;
  GtkWidget* view = nullptr;
  GtkWidget* clip = nullptr;
  Rect bounds;
  Rect visible_bounds;
  std::uint64_t properties_revision = 0;
  std::uint64_t controller_revision = 0;
  PlatformValue controller;
  bool connected = false;

  ~HostedView() {
    route->active = false;
    if (clip) {
      if (gtk_widget_get_parent(clip)) {
        gtk_widget_unparent(clip);
      }
      if (gtk_widget_get_parent(view) == clip) {
        gtk_widget_unparent(view);
      }
      g_object_unref(clip);
    }
    if (instance) {
      TryDisconnect();
      if (factory->dispose) {
        try {
          factory->dispose(instance);
        } catch (...) {
        }
      }
    }
  }

  void TryDisconnect() noexcept {
    if (connected) {
      try {
        factory->disconnect(instance, controller);
      } catch (...) {
      }
    }
  }

  void UpdateController(const PlacePlatformViewCommand& command) {
    if (connected) {
      connected = false;
      factory->disconnect(instance, controller);
    }
    controller = command.Controller();
    if (controller.HasValue()) {
      if (!factory->connect || !factory->disconnect) {
        throw std::logic_error("HuxerUI GTK PlatformView factory must provide connect and disconnect");
      }
      factory->connect(instance, controller);
      connected = true;
    }
    controller_revision = command.ControllerRevision();
  }

  void Update(const PlacePlatformViewCommand& command) {
    if (properties_revision != command.PropertiesRevision()) {
      if (!factory->update) {
        throw std::logic_error("HuxerUI GTK PlatformView factory does not support property updates");
      }
      factory->update(instance, command.Properties());
      properties_revision = command.PropertiesRevision();
    }
    if (controller_revision != command.ControllerRevision()) {
      UpdateController(command);
    }
  }
};

bool IsWithin(GtkWidget* widget, GtkWidget* ancestor) {
  return widget && (widget == ancestor || gtk_widget_is_ancestor(widget, ancestor));
}

} // namespace

struct LinuxPlatformViews::Impl {
  using Views = std::unordered_map<std::uint64_t, std::unique_ptr<HostedView>>;

  Impl(UiWindow& ui_value, PlatformRegistry& registry_value, GtkWindow* window_value, GtkWidget* canvas_value,
       GtkWidget* input_value)
      : ui(ui_value), registry(registry_value), window(window_value), canvas(canvas_value), input(input_value) {}

  std::unique_ptr<HostedView> Create(const PlacePlatformViewCommand& command) {
    auto factory = registry.FindView<linux::detail::GtkViewFactory>(
        ui, command.Type(), command.Properties().Type(), command.Controller().Type());
    if (!factory->create || !factory->view || (command.Properties().HasValue() && !factory->update)) {
      throw std::logic_error("HuxerUI GTK PlatformView factory has missing callbacks: " + std::string(command.Type()));
    }
    auto route = std::make_shared<EventRoute>(EventRoute{.ui = ui, .identity = command.Identity()});
    const auto dispatch = [weak = std::weak_ptr(route)](auto key, auto value) -> std::optional<decltype(value)> {
      const auto route = weak.lock();
      if (!route || route->thread != std::this_thread::get_id() || !route->active) {
        return std::nullopt;
      }
      return InternalAccess::DispatchPlatformViewEvent(route->ui, route->identity, key, value);
    };

    auto hosted = std::make_unique<HostedView>();
    hosted->route = std::move(route);
    hosted->factory = std::move(factory);
    hosted->type = command.Type();
    hosted->properties_revision = command.PropertiesRevision();
    hosted->instance = hosted->factory->create(
        window, command.Properties(), MakePlatformEventEmitter(dispatch, dispatch));
    if (!hosted->instance) {
      throw std::logic_error("HuxerUI GTK PlatformView factory returned no instance: " + std::string(command.Type()));
    }
    hosted->view = hosted->factory->view(hosted->instance);
    if (!hosted->view || !GTK_IS_WIDGET(hosted->view) || gtk_widget_get_parent(hosted->view) ||
        GTK_IS_NATIVE(hosted->view) || g_object_is_floating(hosted->view)) {
      throw std::logic_error("HuxerUI GTK PlatformView factory must return an owned, unparented widget without a "
                             "GdkSurface: " + std::string(command.Type()));
    }

    hosted->clip = GTK_WIDGET(g_object_ref_sink(g_object_new(huxerui_platform_clip_get_type(), nullptr)));
    gtk_widget_set_child_visible(hosted->clip, FALSE);
    gtk_widget_set_parent(hosted->view, hosted->clip);
    hosted->UpdateController(command);
    return hosted;
  }

  void Place(HostedView& view, const PlatformViewPlacement& placement) {
    const Rect bounds = placement.world_bounds;
    const Rect visible = !placement.visible ? Rect{} :
        (placement.clip ? bounds.Intersection(*placement.clip) : bounds);
    if (view.bounds != bounds || view.visible_bounds != visible) {
      view.bounds = bounds;
      view.visible_bounds = visible;
      gtk_widget_set_child_visible(view.clip, !visible.IsEmpty());
      gtk_widget_queue_allocate(view.clip);
      // Only the canvas allocator knows the child's new position in the window.
      gtk_widget_queue_allocate(canvas);
    }
  }

  std::optional<std::uint64_t> FocusIdentity() const {
    GtkWidget* focused = gtk_window_get_focus(window);
    for (const auto& [identity, hosted] : views) {
      if (IsWithin(focused, hosted->clip)) {
        return identity;
      }
    }
    return std::nullopt;
  }

  struct FocusGuard {
    bool& changing;
    bool previous = std::exchange(changing, true);
    ~FocusGuard() { changing = previous; }
  };

  void ApplyFocusToGtk(bool reverse) {
    FocusGuard guard{changing};
    const auto desired = InternalAccess::FocusedPlatformView(ui);
    const auto current = FocusIdentity();
    if (desired == current && gtk_window_get_focus(window)) {
      return;
    }
    gtk_widget_grab_focus(input);
    if (desired) {
      const auto found = views.find(*desired);
      if (found != views.end() && !found->second->visible_bounds.IsEmpty()) {
        gtk_widget_child_focus(found->second->view, reverse ? GTK_DIR_TAB_BACKWARD : GTK_DIR_TAB_FORWARD);
      } else if (found != views.end()) {
        InternalAccess::SynchronizePlatformViewFocus(ui, std::nullopt, false);
      }
    }
  }

  UiWindow& ui;
  PlatformRegistry& registry;
  GtkWindow* window;
  GtkWidget* canvas;
  GtkWidget* input;
  bool changing = false;
  Views views;
};

LinuxPlatformViews::LinuxPlatformViews(UiWindow& ui, PlatformRegistry& registry, GtkWindow* window, GtkWidget* canvas,
                                       GtkWidget* input)
    : impl_(std::make_unique<Impl>(ui, registry, window, canvas, input)) {}

LinuxPlatformViews::~LinuxPlatformViews() {
  impl_->changing = true;
  impl_->views.clear();
}

void LinuxPlatformViews::Commit(const RenderFrame& frame) {
  Impl::FocusGuard guard{impl_->changing};
  const RenderComposition composition = BuildRenderComposition(frame.scene);
  Impl::Views next;
  for (const auto& layer : composition.layers) {
    const auto* placement = std::get_if<PlatformViewPlacement>(&layer);
    if (!placement) {
      continue;
    }
    const auto& command = *placement->command;
    const auto found = impl_->views.find(command.Identity());
    if (found == impl_->views.end() || found->second->type != command.Type()) {
      next.emplace(command.Identity(), impl_->Create(command));
    } else {
      found->second->Update(command);
    }
  }

  // Retain instances by identity while ordering the GTK children for native event picking.
  GtkWidget* sibling = nullptr;
  for (const auto& layer : composition.layers) {
    const auto* placement = std::get_if<PlatformViewPlacement>(&layer);
    if (!placement) {
      continue;
    }
    const auto identity = placement->command->Identity();
    auto found = next.find(identity);
    if (found == next.end()) {
      found = next.insert(impl_->views.extract(identity)).position;
    }
    auto& view = *found->second;
    if (gtk_widget_get_parent(view.clip) != impl_->canvas || gtk_widget_get_prev_sibling(view.clip) != sibling) {
      gtk_widget_insert_after(view.clip, impl_->canvas, sibling);
    }
    sibling = view.clip;
    impl_->Place(view, *placement);
  }
  impl_->views.swap(next);
  next.clear();
  for (const auto& [identity, hosted] : impl_->views) {
    hosted->route->active = true;
  }
  impl_->ApplyFocusToGtk(false);
}

void LinuxPlatformViews::Allocate(int width, int height) {
  const Rect viewport{0.0F, 0.0F, static_cast<float>(width), static_cast<float>(height)};
  for (const auto& [identity, view] : impl_->views) {
    const Rect visible = view->visible_bounds.Intersection(viewport);
    if (visible.IsEmpty()) {
      continue;
    }
    auto* clip = reinterpret_cast<HuxerUIPlatformClip*>(view->clip);
    clip->child_bounds = {view->bounds.x - visible.x, view->bounds.y - visible.y,
                          view->bounds.width, view->bounds.height};
    gtk_widget_allocate(view->clip, GtkExtent(visible.width), GtkExtent(visible.height),
                        -1, Translation(visible.x, visible.y));
  }
}

void LinuxPlatformViews::SnapshotView(GtkSnapshot* snapshot, const PlacePlatformViewCommand& command) const {
  const auto it = impl_->views.find(command.Identity());
  if (it == impl_->views.end() || it->second->visible_bounds.IsEmpty()) {
    return;
  }
  const auto& view = *it->second;
  GtkSnapshot* child_snapshot = gtk_snapshot_new();
  gtk_widget_snapshot_child(impl_->canvas, view.clip, child_snapshot);
  if (GskRenderNode* node = gtk_snapshot_free_to_node(child_snapshot)) {
    // GTK supplies the node in canvas coordinates. The painter is at the placement's local origin.
    const float dx = command.Bounds().x - view.bounds.x;
    const float dy = command.Bounds().y - view.bounds.y;
    const Rect visible = view.visible_bounds;
    const graphene_rect_t clip{{visible.x + dx, visible.y + dy}, {visible.width, visible.height}};
    const graphene_point_t offset{dx, dy};
    gtk_snapshot_push_clip(snapshot, &clip);
    gtk_snapshot_save(snapshot);
    gtk_snapshot_translate(snapshot, &offset);
    gtk_snapshot_append_node(snapshot, node);
    gtk_snapshot_restore(snapshot);
    gtk_snapshot_pop(snapshot);
    gsk_render_node_unref(node);
  }
}

bool LinuxPlatformViews::TargetsHuxerUI(Point point) const {
  if (impl_->views.empty() || impl_->changing) {
    return true;
  }
  const auto identity = InternalAccess::HitTestPlatformView(impl_->ui, point);
  if (!identity) {
    return true;
  }
  const auto found = impl_->views.find(*identity);
  return found == impl_->views.end() || found->second->visible_bounds.IsEmpty() ||
         !found->second->visible_bounds.Contains(point);
}

void LinuxPlatformViews::SynchronizeFocusFromGtk() {
  if (!impl_->changing) {
    InternalAccess::SynchronizePlatformViewFocus(
        impl_->ui, impl_->FocusIdentity(), gtk_window_get_focus_visible(impl_->window));
  }
}

void LinuxPlatformViews::ApplyFocusToGtk(bool reverse) {
  if (!impl_->changing) {
    impl_->ApplyFocusToGtk(reverse);
  }
}

bool LinuxPlatformViews::HandleTab(bool reverse) {
  if (impl_->changing) {
    return false;
  }
  const auto identity = impl_->FocusIdentity();
  if (!identity) {
    return false;
  }
  GtkWidget* child = impl_->views.at(*identity)->view;
  if (gtk_widget_child_focus(child, reverse ? GTK_DIR_TAB_BACKWARD : GTK_DIR_TAB_FORWARD)) {
    return true;
  }
  return InternalAccess::MoveFocusFromPlatformView(impl_->ui, *identity, reverse);
}

} // namespace huxerui::detail
