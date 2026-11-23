#include "ohos_platform_view.h"

#include <arkui/native_interface_focus.h>
#include <arkui/native_key_event.h>
#include <arkui/ui_input_event.h>
#include <hilog/log.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

#include <huxerui/ohos/platform_registry.h>
#include <huxerui/semantics.h>

#include "internal_access.h"
#include "runtime/mounted_node_internal.h"

#include "ohos_renderer.h"

namespace huxerui::detail {
namespace {

using Node = std::unique_ptr<std::remove_pointer_t<ArkUI_NodeHandle>, void (*)(ArkUI_NodeHandle)>;

void Check(int status) {
  if (status != 0) {
    throw std::runtime_error("HuxerUI could not update OHOS PlatformView composition");
  }
}

Node CreateNode(ArkUI_NativeNodeAPI_1& api, ArkUI_NodeType type) {
  Node node(api.createNode(type), api.disposeNode);
  if (!node) {
    throw std::runtime_error("HuxerUI could not create an OHOS PlatformView node");
  }
  return node;
}

void Set(ArkUI_NativeNodeAPI_1& api, ArkUI_NodeHandle node, ArkUI_NodeAttributeType type,
         ArkUI_NumberValue value) {
  const ArkUI_AttributeItem item{.value = &value, .size = 1, .string = nullptr, .object = nullptr};
  Check(api.setAttribute(node, type, &item));
}

void Place(ArkUI_NativeNodeAPI_1& api, ArkUI_NodeHandle node, Rect bounds) {
  const ArkUI_NumberValue point[]{{.f32 = bounds.x}, {.f32 = bounds.y}};
  const ArkUI_AttributeItem position{.value = point, .size = 2, .string = nullptr, .object = nullptr};
  Check(api.setAttribute(node, NODE_POSITION, &position));
  Set(api, node, NODE_WIDTH, {.f32 = bounds.width});
  Set(api, node, NODE_HEIGHT, {.f32 = bounds.height});
}

void Report(const std::exception& error) noexcept {
  OH_LOG_Print(LOG_APP, LOG_ERROR, 0xCAFE, "HuxerUI", "%{public}s", error.what());
}

struct EventRoute {
  UiWindow& window;
  const std::thread::id application_thread = std::this_thread::get_id();
  std::uint64_t identity;
  bool active = false;
  std::optional<bool> tab_reverse{};
};

PlatformEventEmitter EventEmitter(const std::shared_ptr<EventRoute>& route) {
  const std::weak_ptr<EventRoute> weak = route;
  const auto active = [weak] {
    auto current = weak.lock();
    if (!current || current->application_thread != std::this_thread::get_id() || !current->active) {
      return std::shared_ptr<EventRoute>{};
    }
    return current;
  };
  return MakePlatformEventEmitter(
      [active](std::type_index key, PlatformValue value) -> std::optional<PlatformValue> {
        if (const auto route = active()) {
          return InternalAccess::DispatchPlatformViewEvent(route->window, route->identity, key, value);
        }
        return std::nullopt;
      },
      [active](std::string name, PlatformPayload value) -> std::optional<PlatformPayload> {
        if (const auto route = active()) {
          return InternalAccess::DispatchPlatformViewEvent(route->window, route->identity, name, value);
        }
        return std::nullopt;
      });
}

struct HostedView {
  HostedView(ArkUI_NativeNodeAPI_1& api, ArkUI_NodeHandle root, UiWindow& window, PlatformRegistry& registry,
             const PlacePlatformViewCommand& command, const OhosPlatformViews::ForwardTouch& forward_touch)
      : api(api), root(root), container(CreateNode(api, ARKUI_NODE_STACK)), type(command.Type()),
        route(std::make_shared<EventRoute>(EventRoute{.window = window, .identity = command.Identity()})),
        factory(registry.FindView<ohos::detail::ArkUiViewFactory>(window, command.Type(), command.Properties().Type(),
                                                                command.Controller().Type())),
        forward_touch(forward_touch) {
    if (!factory->create || !factory->view || (command.Properties().HasValue() && !factory->update)) {
      throw std::logic_error("HuxerUI OHOS PlatformView factory lacks required callbacks");
    }
    Set(api, container.get(), NODE_CLIP, {.i32 = 1});
    Set(api, container.get(), NODE_STACK_ALIGN_CONTENT, {.i32 = ARKUI_ALIGNMENT_TOP_START});
    instance = factory->create(api, command.Properties(), EventEmitter(route));
    if (!instance || !(view = factory->view(instance))) {
      throw std::logic_error("HuxerUI OHOS PlatformView factory returned no ArkUI node");
    }
    try {
      Check(api.addChild(container.get(), view));
      view_added = true;
      Check(api.addNodeEventReceiver(container.get(), OnContainerEvent));
      Check(api.registerNodeEvent(container.get(), NODE_ON_TOUCH_INTERCEPT, NODE_ON_TOUCH_INTERCEPT, this));
      Check(api.registerNodeEvent(container.get(), NODE_TOUCH_EVENT, NODE_TOUCH_EVENT, this));
      Check(api.addNodeEventReceiver(view, OnViewEvent));
      Check(api.registerNodeEvent(view, NODE_ON_FOCUS, NODE_ON_FOCUS, this));
      Check(api.registerNodeEvent(view, NODE_ON_BLUR, NODE_ON_BLUR, this));
      Check(api.registerNodeEvent(view, NODE_ON_KEY_EVENT, NODE_ON_KEY_EVENT, this));
      Check(api.addChild(root, container.get()));
      container_added = true;
      properties_revision = command.PropertiesRevision();
      Update(command);
    } catch (...) {
      Dispose();
      throw;
    }
  }

  ~HostedView() { Dispose(); }

  void Update(const PlacePlatformViewCommand& command) {
    if (properties_revision != command.PropertiesRevision()) {
      factory->update(instance, command.Properties());
      properties_revision = command.PropertiesRevision();
    }
    if (controller_revision != command.ControllerRevision() || !controller.Equivalent(command.Controller())) {
      Disconnect();
      controller_revision = command.ControllerRevision();
      if (command.Controller().HasValue()) {
        if (!factory->connect || !factory->disconnect) {
          throw std::logic_error("HuxerUI OHOS PlatformView factory lacks controller callbacks");
        }
        factory->connect(instance, command.Controller());
        controller = command.Controller();
      }
    }
  }

  void Position(const PlatformViewPlacement& placement, std::int32_t order) {
    const Rect visible = placement.clip ? placement.world_bounds.Intersection(*placement.clip) : placement.world_bounds;
    const bool shown = placement.visible && !visible.IsEmpty();
    if (order != z_index) {
      Set(api, container.get(), NODE_Z_INDEX, {.i32 = order});
      z_index = order;
    }
    if (shown != visible_now) {
      Set(api, container.get(), NODE_VISIBILITY, {.i32 = shown ? ARKUI_VISIBILITY_VISIBLE : ARKUI_VISIBILITY_NONE});
      visible_now = shown;
    }
    if (shown && (visible != viewport || world_bounds != placement.world_bounds)) {
      viewport = visible;
      world_bounds = placement.world_bounds;
      Place(api, container.get(), visible);
      Place(api, view, {world_bounds.x - visible.x, world_bounds.y - visible.y,
                        world_bounds.width, world_bounds.height});
    }
  }

  void SetAccessible(bool accessible) {
    if (accessible == accessible_now) return;
    Set(api, container.get(), NODE_ACCESSIBILITY_MODE,
        {.i32 = accessible ? ARKUI_ACCESSIBILITY_MODE_AUTO : ARKUI_ACCESSIBILITY_MODE_DISABLED_FOR_DESCENDANTS});
    accessible_now = accessible;
  }

  void Commit() {
    route->active = true;
    if (factory->commit) factory->commit(instance);
  }

  static void OnContainerEvent(ArkUI_NodeEvent* event) noexcept {
    const auto type = OH_ArkUI_NodeEvent_GetEventType(event);
    if (type != NODE_ON_TOUCH_INTERCEPT && type != NODE_TOUCH_EVENT) {
      return;
    }
    auto* hosted = static_cast<HostedView*>(OH_ArkUI_NodeEvent_GetUserData(event));
    const auto* input = OH_ArkUI_NodeEvent_GetInputEvent(event);
    if (!hosted || !hosted->route->active || !hosted->visible_now || !input) {
      return;
    }
    try {
      const Point origin{hosted->viewport.x, hosted->viewport.y};
      if (type == NODE_ON_TOUCH_INTERCEPT) {
        auto& route = *hosted->route;
        const Point position{origin.x + OH_ArkUI_PointerEvent_GetX(input), origin.y + OH_ArkUI_PointerEvent_GetY(input)};
        hosted->forwarding_touch = InternalAccess::HitTestPlatformView(route.window, position) != route.identity;
        // When HuxerUI content covers the embedded view, block its child tree and forward the touch to UiWindow.
        OH_ArkUI_PointerEvent_SetInterceptHitTestMode(input, hosted->forwarding_touch ? HTM_BLOCK : HTM_DEFAULT);
      } else if (hosted->forwarding_touch) {
        hosted->forward_touch(input, origin);
      }
    } catch (const std::exception& error) {
      Report(error);
    }
  }

  static void OnViewEvent(ArkUI_NodeEvent* event) noexcept {
    const auto type = OH_ArkUI_NodeEvent_GetEventType(event);
    if (type != NODE_ON_FOCUS && type != NODE_ON_BLUR && type != NODE_ON_KEY_EVENT) {
      return;
    }
    auto* hosted = static_cast<HostedView*>(OH_ArkUI_NodeEvent_GetUserData(event));
    if (!hosted || !hosted->route->active || !hosted->visible_now) {
      return;
    }
    try {
      auto& route = *hosted->route;
      if (type == NODE_ON_KEY_EVENT) {
        route.tab_reverse.reset();
        const auto* input = OH_ArkUI_NodeEvent_GetInputEvent(event);
        if (!input || OH_ArkUI_KeyEvent_GetType(input) != ARKUI_KEY_EVENT_DOWN ||
            OH_ArkUI_KeyEvent_GetKeyCode(input) != ARKUI_KEYCODE_TAB) {
          return;
        }
        std::uint64_t modifiers = 0;
        OH_ArkUI_UIInputEvent_GetModifierKeyStates(input, &modifiers);
        if ((modifiers & (ARKUI_MODIFIER_KEY_CTRL | ARKUI_MODIFIER_KEY_ALT)) != 0) return;
        // ArkUI first navigates its own descendants. Only a blur of the whole scope hands Tab to HuxerUI.
        route.tab_reverse = (modifiers & ARKUI_MODIFIER_KEY_SHIFT) != 0;
      } else if (type == NODE_ON_FOCUS) {
        InternalAccess::SynchronizePlatformViewFocus(route.window, route.identity, false);
      } else if (InternalAccess::FocusedPlatformView(route.window) == route.identity) {
        const auto reverse = std::exchange(route.tab_reverse, std::nullopt);
        if (!reverse || !InternalAccess::MoveFocusFromPlatformView(route.window, route.identity, *reverse)) {
          InternalAccess::SynchronizePlatformViewFocus(route.window, std::nullopt, false);
        }
      }
    } catch (const std::exception& error) {
      Report(error);
    }
  }

  void Disconnect() {
    if (controller.HasValue()) {
      factory->disconnect(instance, std::exchange(controller, {}));
    }
  }

  void Dispose() noexcept {
    route->active = false;
    try {
      Disconnect();
    } catch (const std::exception& error) {
      Report(error);
    }
    if (container_added) {
      api.removeChild(root, container.get());
    }
    api.unregisterNodeEvent(container.get(), NODE_ON_TOUCH_INTERCEPT);
    api.unregisterNodeEvent(container.get(), NODE_TOUCH_EVENT);
    api.removeNodeEventReceiver(container.get(), OnContainerEvent);
    api.unregisterNodeEvent(view, NODE_ON_FOCUS);
    api.unregisterNodeEvent(view, NODE_ON_BLUR);
    api.unregisterNodeEvent(view, NODE_ON_KEY_EVENT);
    api.removeNodeEventReceiver(view, OnViewEvent);
    if (view_added) {
      api.removeChild(container.get(), view);
    }
    try {
      if (factory->dispose) {
        factory->dispose(instance);
      }
    } catch (const std::exception& error) {
      Report(error);
    }
  }

  ArkUI_NativeNodeAPI_1& api;
  ArkUI_NodeHandle root;
  Node container;
  const std::string type;
  std::shared_ptr<EventRoute> route;
  std::shared_ptr<const ohos::detail::ArkUiViewFactory> factory;
  const OhosPlatformViews::ForwardTouch& forward_touch;
  std::shared_ptr<void> instance;
  ArkUI_NodeHandle view = nullptr;
  PlatformValue controller;
  std::uint64_t properties_revision = 0;
  std::uint64_t controller_revision = 0;
  std::int32_t z_index = -1;
  Rect viewport;
  Rect world_bounds;
  bool visible_now = true;
  bool accessible_now = true;
  bool view_added = false;
  bool container_added = false;
  bool forwarding_touch = false;
};

struct DrawingSlice {
  DrawingSlice(ArkUI_NativeNodeAPI_1& api, ArkUI_NodeHandle root, OhosRenderer& renderer, std::int32_t order)
      : api(api), root(root), renderer(renderer), node(CreateNode(api, ARKUI_NODE_CUSTOM)) {
    Set(api, node.get(), NODE_WIDTH_PERCENT, {.f32 = 1.0F});
    Set(api, node.get(), NODE_HEIGHT_PERCENT, {.f32 = 1.0F});
    Set(api, node.get(), NODE_HIT_TEST_BEHAVIOR, {.i32 = ARKUI_HIT_TEST_MODE_NONE});
    Set(api, node.get(), NODE_Z_INDEX, {.i32 = order});
    Check(api.addNodeCustomEventReceiver(node.get(), OnDraw));
    Check(api.registerNodeCustomEvent(node.get(), ARKUI_NODE_CUSTOM_EVENT_ON_DRAW, 0, this));
    Check(api.addChild(root, node.get()));
  }

  ~DrawingSlice() {
    api.unregisterNodeCustomEvent(node.get(), ARKUI_NODE_CUSTOM_EVENT_ON_DRAW);
    api.removeNodeCustomEventReceiver(node.get(), OnDraw);
    api.removeChild(root, node.get());
  }

  void Commit(const RenderScene& next_scene, std::optional<RenderSlice> next_slice, float next_scale, bool damaged) {
    const bool repaint = damaged || slice != next_slice || scale != next_scale;
    scene = next_scene;
    slice = next_slice;
    scale = next_scale;
    if (repaint) {
      api.markDirty(node.get(), NODE_NEED_RENDER);
    }
  }

  static void OnDraw(ArkUI_NodeCustomEvent* event) noexcept {
    auto* layer = static_cast<DrawingSlice*>(OH_ArkUI_NodeCustomEvent_GetUserData(event));
    auto* context = OH_ArkUI_NodeCustomEvent_GetDrawContextInDraw(event);
    auto* canvas = context ? static_cast<OH_Drawing_Canvas*>(OH_ArkUI_DrawContext_GetCanvas(context)) : nullptr;
    if (layer && canvas) {
      try {
        if (layer->slice) {
          layer->renderer.PaintSlice(canvas, layer->scene, layer->scale, layer->slice->first_command,
                                     layer->slice->command_count);
        } else {
          layer->renderer.Paint(canvas, layer->scene, layer->scale);
        }
      } catch (const std::exception& error) {
        Report(error);
      }
    }
  }

  ArkUI_NativeNodeAPI_1& api;
  ArkUI_NodeHandle root;
  OhosRenderer& renderer;
  Node node;
  RenderScene scene;
  std::optional<RenderSlice> slice;
  float scale = 1.0F;
};

} // namespace

struct OhosPlatformViews::Impl {
  ArkUI_NativeNodeAPI_1& api;
  ArkUI_NodeHandle root;
  UiWindow& window;
  PlatformRegistry& registry;
  OhosRenderer& renderer;
  ForwardTouch forward_touch;
  std::unique_ptr<DrawingSlice> base;
  std::map<std::uint64_t, std::unique_ptr<HostedView>> hosted;
  std::map<std::int32_t, std::unique_ptr<DrawingSlice>> slices;
};

OhosPlatformViews::OhosPlatformViews(ArkUI_NativeNodeAPI_1& api, ArkUI_NodeHandle root, UiWindow& window,
                                     PlatformRegistry& registry, OhosRenderer& renderer, ForwardTouch forward_touch)
    : impl_(new Impl{api, root, window, registry, renderer, std::move(forward_touch), {}, {}, {}}) {}

OhosPlatformViews::~OhosPlatformViews() = default;

void OhosPlatformViews::Commit(const RenderFrame& frame, const SemanticFrame* semantics, float scale) {
  auto& state = *impl_;
  const bool damaged = frame.damage.full || !frame.damage.rects.empty();
  if (!state.base) {
    // The XComponent below the drawing nodes owns input and accessibility.
    state.base = std::make_unique<DrawingSlice>(state.api, state.root, state.renderer, 1);
    Set(state.api, state.base->node.get(), NODE_BACKGROUND_COLOR, {.u32 = 0xFFF7F8FA});
  }
  if (!RenderSceneHasPlatformViews(frame.scene.root)) {
    state.base->Commit(frame.scene, std::nullopt, scale, damaged);
    state.slices.clear();
    state.hosted.clear();
    return;
  }
  const auto composition = BuildRenderComposition(frame.scene);
  RenderSlice base_slice{};
  std::set<std::uint64_t> views;
  std::set<std::int32_t> slices;
  std::int32_t z_index = 0;
  for (const auto& layer : composition.layers) {
    const auto order = z_index++;
    if (const auto* placement = std::get_if<PlatformViewPlacement>(&layer)) {
      const auto& command = *placement->command;
      auto& hosted = state.hosted[command.Identity()];
      if (!hosted || hosted->type != command.Type()) {
        hosted = std::make_unique<HostedView>(state.api, state.root, state.window, state.registry, command,
                                             state.forward_touch);
      } else {
        hosted->Update(command);
      }
      hosted->Position(*placement, order + 2);
      views.insert(command.Identity());
    } else if (const auto& slice = std::get<RenderSlice>(layer); order == 0) {
      base_slice = slice;
    } else {
      auto& drawing = state.slices[order];
      if (!drawing) {
        drawing = std::make_unique<DrawingSlice>(state.api, state.root, state.renderer, order + 2);
      }
      drawing->Commit(frame.scene, slice, scale, damaged);
      slices.insert(order);
    }
  }
  state.base->Commit(frame.scene, base_slice, scale, damaged);
  std::erase_if(state.hosted, [&views](const auto& entry) { return !views.contains(entry.first); });
  std::erase_if(state.slices, [&slices](const auto& entry) { return !slices.contains(entry.first); });
  for (const auto& [identity, hosted] : state.hosted) {
    // A modal or a Semantics modifier can exclude the anchor even while ArkUI still draws the view.
    hosted->SetAccessible(!semantics || std::ranges::any_of(semantics->nodes, [identity](const SemanticNode& node) {
      return node.platform_view_identity == identity && !node.offscreen;
    }));
    hosted->Commit();
  }
  const auto focus = InternalAccess::FocusedPlatformView(state.window);
  if (focus) {
    const auto found = state.hosted.find(*focus);
    if (found != state.hosted.end() && found->second->visible_now) {
      OH_ArkUI_FocusRequest(found->second->view);
    }
  }
}

void OhosPlatformViews::Clear() noexcept {
  impl_->base.reset();
  impl_->slices.clear();
  impl_->hosted.clear();
}

} // namespace huxerui::detail
