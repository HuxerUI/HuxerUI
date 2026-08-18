#include "web_platform_view.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <emscripten.h>

#include <huxerui/web/platform_view.h>

#include "internal.h"
#include "web_renderer.h"

namespace huxerui::detail {

namespace {

using emscripten::val;

// clang-format off
EM_JS(bool, IsDetachedWebPlatformElement, (emscripten::EM_VAL handle), {
  const element = Emval.toValue(handle);
  return element instanceof HTMLElement && element.parentNode === null;
});

EM_JS(bool, FocusWebPlatformElement, (emscripten::EM_VAL handle), {
  const element = Emval.toValue(handle);
  if (!(element instanceof HTMLElement) || !element.isConnected) {
    return false;
  }
  const candidates = Module.huxerUIWebPlatformViewFocusables
      ? Module.huxerUIWebPlatformViewFocusables(element)
      : [];
  const target = candidates[0];
  if (!(target instanceof HTMLElement)) {
    return false;
  }
  target.focus({preventScroll : true});
  return element.contains(document.activeElement);
});

EM_JS(bool, WebPlatformElementContainsFocus, (emscripten::EM_VAL handle), {
  const element = Emval.toValue(handle);
  return element instanceof HTMLElement && element.contains(document.activeElement);
});
// clang-format on

struct SliceKey {
  std::optional<std::uint64_t> preceding;
  std::optional<std::uint64_t> following;

  bool operator==(const SliceKey&) const = default;
};

struct SliceKeyHash {
  std::size_t operator()(const SliceKey& key) const noexcept {
    const std::size_t preceding = key.preceding.has_value() ? std::hash<std::uint64_t>{}(*key.preceding) : 0;
    const std::size_t following = key.following.has_value() ? std::hash<std::uint64_t>{}(*key.following) : 0;
    return preceding ^ (following + 0x9e3779b9U + (preceding << 6U) + (preceding >> 2U));
  }
};

using LayerKey = std::variant<SliceKey, std::uint64_t>;

struct EventRoute {
  Runtime* runtime = nullptr;
  std::uint64_t identity = 0;
  bool active = false;
};

void SetStyle(const val& element, const char* name, std::string value) {
  element["style"].set(name, std::move(value));
}

std::string CssPixels(float value) {
  return std::to_string(value) + "px";
}

Rect VisibleBounds(const PlatformViewPlacement& placement) {
  return placement.clip.has_value() ? placement.world_bounds.Intersection(*placement.clip) : placement.world_bounds;
}

void ConfigureCanvas(val& canvas, Size viewport, float display_scale) {
  canvas.set("width", static_cast<unsigned int>(std::ceil(viewport.width * display_scale)));
  canvas.set("height", static_cast<unsigned int>(std::ceil(viewport.height * display_scale)));
  canvas.call<void>("setAttribute", std::string("aria-hidden"), std::string("true"));
  SetStyle(canvas, "position", "absolute");
  SetStyle(canvas, "inset", "0");
  SetStyle(canvas, "width", "100%");
  SetStyle(canvas, "height", "100%");
  SetStyle(canvas, "pointerEvents", "none");
  SetStyle(canvas, "zIndex", "0");
}

struct CanvasSlice {
  explicit CanvasSlice(val canvas_value) : canvas(std::move(canvas_value)) {}

  val canvas;
  RenderSlice range;
};

struct CommittedPlacement {
  Rect world_bounds;
  Rect visible_bounds;
  bool hidden = false;

  bool operator==(const CommittedPlacement&) const = default;
};

struct HostedPlatformView {
  std::uint64_t properties_revision = 0;
  std::uint32_t token = 0;
  std::string type;
  std::shared_ptr<EventRoute> event_route;
  web::PlatformViewFactory factory;
  val element = val::undefined();
  val container = val::undefined();
  std::optional<CommittedPlacement> placement;

  ~HostedPlatformView() {
    if (event_route) {
      event_route->active = false;
    }
    if (!container.isUndefined() && !container.isNull()) {
      try {
        container.call<void>("remove");
      } catch (...) {
      }
    }
    if (factory.dispose && !element.isUndefined() && !element.isNull()) {
      try {
        factory.dispose(element);
      } catch (...) {
      }
    }
  }

  void Update(const PlatformPayload& properties) {
    if (!factory.update) {
      throw std::logic_error("HuxerUI Web PlatformView factory does not support property updates");
    }
    try {
      factory.update(element, properties);
    } catch (...) {
      throw std::logic_error("HuxerUI Web PlatformView factory failed while updating");
    }
  }
};

} // namespace

struct WebPlatformViews::State {
  State(
      WebRenderer& renderer_value,
      PlatformModules& modules_value,
      Runtime& runtime_value,
      UIThreadDispatcher dispatcher,
      val root_value,
      val base_canvas_value
  )
      : renderer(&renderer_value), modules(&modules_value), runtime(&runtime_value),
        dispatch_to_ui_thread(std::move(dispatcher)), root(std::move(root_value)),
        base_canvas(std::move(base_canvas_value)) {}

  std::uint32_t AllocateToken() {
    for (std::uint64_t attempt = 0; attempt < std::numeric_limits<std::uint32_t>::max(); ++attempt) {
      if (++next_token == 0) {
        ++next_token;
      }
      if (!identities_by_token.contains(next_token)) {
        return next_token;
      }
    }
    throw std::logic_error("HuxerUI Web PlatformView token space is exhausted");
  }

  std::unique_ptr<HostedPlatformView> Create(const PlatformViewPlacement& placement) {
    const PlacePlatformViewCommand& command = *placement.command;
    const auto* factory = modules->Find<web::PlatformViewFactory>(command.Type());
    if (factory == nullptr) {
      throw std::logic_error("HuxerUI Web PlatformView type is not registered: " + std::string(command.Type()));
    }
    if (!factory->create) {
      throw std::logic_error("HuxerUI Web PlatformView factory must provide create");
    }

    auto route = std::make_shared<EventRoute>(EventRoute{runtime, command.Identity(), false});
    const std::weak_ptr<EventRoute> weak_route = route;
    PlatformEventSink events = [weak_route,
                                dispatcher = dispatch_to_ui_thread](std::string name, PlatformPayload payload) mutable {
      dispatcher([weak_route, name = std::move(name), payload = std::move(payload)]() mutable {
        const std::shared_ptr<EventRoute> route = weak_route.lock();
        if (!route || !route->active || route->runtime == nullptr) {
          return;
        }
        static_cast<void>(RuntimeAccess::DispatchPlatformViewEvent(*route->runtime, route->identity, name, payload));
      });
    };

    auto hosted = std::make_unique<HostedPlatformView>();
    hosted->properties_revision = command.PropertiesRevision();
    hosted->type = command.Type();
    hosted->event_route = std::move(route);
    hosted->factory = *factory;
    try {
      hosted->element = factory->create(command.Properties(), std::move(events));
    } catch (...) {
      throw std::logic_error("HuxerUI Web PlatformView factory failed while creating");
    }
    if (!IsDetachedWebPlatformElement(hosted->element.as_handle())) {
      throw std::logic_error("HuxerUI Web PlatformView factory must return a detached HTMLElement");
    }

    const std::uint32_t token = AllocateToken();
    hosted->token = token;
    hosted->container = val::global("document").call<val>("createElement", std::string("div"));
    hosted->container["dataset"].set("huxeruiPlatformView", token);
    SetStyle(hosted->container, "position", "absolute");
    SetStyle(hosted->container, "boxSizing", "border-box");
    SetStyle(hosted->container, "overflow", "hidden");
    SetStyle(hosted->container, "isolation", "isolate");
    SetStyle(hosted->container, "contain", "layout paint");
    SetStyle(hosted->container, "pointerEvents", "auto");
    SetStyle(hosted->container, "zIndex", "0");
    SetStyle(hosted->element, "position", "absolute");
    SetStyle(hosted->element, "boxSizing", "border-box");
    SetStyle(hosted->element, "margin", "0");
    hosted->container.call<void>("appendChild", hosted->element);
    identities_by_token.emplace(token, command.Identity());
    return hosted;
  }

  void Place(HostedPlatformView& hosted, const PlatformViewPlacement& placement) {
    const Rect visible_bounds = VisibleBounds(placement);
    const bool hidden = !placement.visible || visible_bounds.IsEmpty();
    const CommittedPlacement next{placement.world_bounds, visible_bounds, hidden};
    if (hosted.placement == next) {
      return;
    }
    if (!hosted.placement.has_value() || hosted.placement->hidden != hidden) {
      SetStyle(hosted.container, "display", hidden ? "none" : "block");
    }
    if (hidden) {
      hosted.placement = next;
      return;
    }
    SetStyle(hosted.container, "left", CssPixels(visible_bounds.x));
    SetStyle(hosted.container, "top", CssPixels(visible_bounds.y));
    SetStyle(hosted.container, "width", CssPixels(std::max(0.0F, visible_bounds.width)));
    SetStyle(hosted.container, "height", CssPixels(std::max(0.0F, visible_bounds.height)));
    SetStyle(hosted.element, "left", CssPixels(placement.world_bounds.x - visible_bounds.x));
    SetStyle(hosted.element, "top", CssPixels(placement.world_bounds.y - visible_bounds.y));
    SetStyle(hosted.element, "width", CssPixels(std::max(0.0F, placement.world_bounds.width)));
    SetStyle(hosted.element, "height", CssPixels(std::max(0.0F, placement.world_bounds.height)));
    hosted.placement = next;
  }

  CanvasSlice& EnsureSlice(const SliceKey& key, const RenderSlice& range, bool& composition_changed) {
    const auto found = slices.find(key);
    if (found != slices.end()) {
      found->second.range = range;
      return found->second;
    }
    val canvas = val::global("document").call<val>("createElement", std::string("canvas"));
    ConfigureCanvas(canvas, viewport, display_scale);
    auto [inserted, was_inserted] = slices.emplace(key, CanvasSlice(std::move(canvas)));
    static_cast<void>(was_inserted);
    inserted->second.range = range;
    composition_changed = true;
    return inserted->second;
  }

  std::optional<std::uint64_t> IdentityForToken(std::uint32_t token) const {
    const auto found = identities_by_token.find(token);
    return found == identities_by_token.end() ? std::nullopt : std::optional{found->second};
  }

  WebRenderer* renderer;
  PlatformModules* modules;
  Runtime* runtime;
  UIThreadDispatcher dispatch_to_ui_thread;
  val root;
  val base_canvas;
  Size viewport;
  float display_scale = 1.0F;
  std::uint32_t next_token = 0;
  std::optional<RenderSlice> base_slice;
  std::unordered_map<std::uint64_t, std::unique_ptr<HostedPlatformView>> hosted;
  std::unordered_map<std::uint32_t, std::uint64_t> identities_by_token;
  std::unordered_map<SliceKey, CanvasSlice, SliceKeyHash> slices;
  std::vector<LayerKey> order;
};

WebPlatformViews::WebPlatformViews(
    WebRenderer& renderer,
    PlatformModules& modules,
    Runtime& runtime,
    UIThreadDispatcher dispatch_to_ui_thread,
    val root,
    val base_canvas
)
    : state_(std::make_unique<State>(
          renderer, modules, runtime, std::move(dispatch_to_ui_thread), std::move(root), std::move(base_canvas)
      )) {}

WebPlatformViews::~WebPlatformViews() {
  Shutdown();
}

void WebPlatformViews::SetViewport(Size viewport, float display_scale) {
  state_->viewport = viewport;
  state_->display_scale = std::max(1.0F, display_scale);
  for (auto& [key, slice] : state_->slices) {
    static_cast<void>(key);
    ConfigureCanvas(slice.canvas, state_->viewport, state_->display_scale);
  }
}

void WebPlatformViews::Commit(const RenderFrame& frame) {
  RenderComposition composition = BuildRenderComposition(frame.scene);
  const bool has_platform_views = std::ranges::any_of(composition.layers, [](const RenderCompositionLayer& layer) {
    return std::holds_alternative<PlatformViewPlacement>(layer);
  });
  if (!has_platform_views) {
    const bool composition_changed = !state_->hosted.empty() || !state_->slices.empty() || !state_->order.empty();
    state_->hosted.clear();
    state_->identities_by_token.clear();
    state_->slices.clear();
    state_->base_slice.reset();
    state_->order.clear();
    if (composition_changed) {
      state_->root.call<void>("replaceChildren", state_->base_canvas);
      state_->renderer->Invalidate();
    }
    state_->renderer->Draw(frame);
    return;
  }

  std::unordered_set<std::uint64_t> retained_identities;
  std::vector<std::pair<std::uint64_t, std::unique_ptr<HostedPlatformView>>> pending;
  bool composition_changed = false;
  try {
    for (const RenderCompositionLayer& layer : composition.layers) {
      const auto* placement = std::get_if<PlatformViewPlacement>(&layer);
      if (placement == nullptr) {
        continue;
      }
      const PlacePlatformViewCommand& command = *placement->command;
      retained_identities.insert(command.Identity());
      const auto found = state_->hosted.find(command.Identity());
      if (found == state_->hosted.end() || found->second->type != command.Type()) {
        pending.emplace_back(command.Identity(), state_->Create(*placement));
        composition_changed = true;
        continue;
      }
      if (found->second->properties_revision != command.PropertiesRevision()) {
        found->second->Update(command.Properties());
        found->second->properties_revision = command.PropertiesRevision();
      }
    }
  } catch (...) {
    for (const auto& [identity, hosted] : pending) {
      static_cast<void>(identity);
      state_->identities_by_token.erase(hosted->token);
    }
    throw;
  }

  for (auto& [identity, hosted] : pending) {
    const auto existing = state_->hosted.find(identity);
    if (existing != state_->hosted.end()) {
      state_->identities_by_token.erase(existing->second->token);
      state_->hosted.erase(existing);
    }
    state_->hosted.emplace(identity, std::move(hosted));
  }
  std::erase_if(state_->hosted, [&](const auto& entry) {
    if (retained_identities.contains(entry.first)) {
      return false;
    }
    state_->identities_by_token.erase(entry.second->token);
    composition_changed = true;
    return true;
  });

  std::unordered_set<SliceKey, SliceKeyHash> retained_slices;
  std::vector<LayerKey> next_order;
  std::vector<val> ordered_elements;
  state_->base_slice.reset();
  bool passed_platform_view = false;
  for (const RenderCompositionLayer& layer : composition.layers) {
    if (const auto* slice = std::get_if<RenderSlice>(&layer)) {
      if (!passed_platform_view && !state_->base_slice.has_value()) {
        state_->base_slice = *slice;
        continue;
      }
      const SliceKey key{slice->preceding_platform_view, slice->following_platform_view};
      retained_slices.insert(key);
      CanvasSlice& retained = state_->EnsureSlice(key, *slice, composition_changed);
      next_order.emplace_back(key);
      ordered_elements.push_back(retained.canvas);
      continue;
    }

    const auto& placement = std::get<PlatformViewPlacement>(layer);
    passed_platform_view = true;
    HostedPlatformView& hosted = *state_->hosted.at(placement.command->Identity());
    state_->Place(hosted, placement);
    next_order.emplace_back(placement.command->Identity());
    ordered_elements.push_back(hosted.container);
  }

  std::erase_if(state_->slices, [&](const auto& entry) {
    if (retained_slices.contains(entry.first)) {
      return false;
    }
    try {
      entry.second.canvas.template call<void>("remove");
    } catch (...) {
    }
    composition_changed = true;
    return true;
  });
  if (state_->order != next_order) {
    composition_changed = true;
  }
  if (composition_changed) {
    state_->root.call<void>("appendChild", state_->base_canvas);
    for (const val& element : ordered_elements) {
      state_->root.call<void>("appendChild", element);
    }
  }
  state_->order = std::move(next_order);
  for (auto& [identity, hosted] : state_->hosted) {
    static_cast<void>(identity);
    hosted->event_route->active = true;
  }

  const std::optional<std::uint64_t> focused_identity = RuntimeAccess::FocusedPlatformView(*state_->runtime);
  if (focused_identity.has_value()) {
    const auto focused = state_->hosted.find(*focused_identity);
    if (focused == state_->hosted.end() || !focused->second->placement.has_value() ||
        focused->second->placement->hidden) {
      RuntimeAccess::SynchronizePlatformViewFocus(*state_->runtime, std::nullopt, false);
    } else if (!WebPlatformElementContainsFocus(focused->second->element.as_handle()) &&
               !FocusWebPlatformElement(focused->second->element.as_handle())) {
      RuntimeAccess::SynchronizePlatformViewFocus(*state_->runtime, std::nullopt, false);
    }
  } else {
    const bool native_element_focused = std::ranges::any_of(state_->hosted, [](const auto& entry) {
      return WebPlatformElementContainsFocus(entry.second->element.as_handle());
    });
    if (native_element_focused) {
      state_->root.call<void>("focus", val::object());
    }
  }

  const bool force_redraw = state_->renderer->TakeInvalidation() || composition_changed;
  if (state_->base_slice.has_value()) {
    state_->renderer->DrawSlice(
        state_->base_canvas,
        frame,
        state_->base_slice->first_command,
        state_->base_slice->command_count,
        true,
        force_redraw
    );
  } else {
    state_->renderer->DrawSlice(state_->base_canvas, frame, 0, 0, true, force_redraw);
  }
  for (const auto& [key, slice] : state_->slices) {
    static_cast<void>(key);
    state_->renderer
        ->DrawSlice(slice.canvas, frame, slice.range.first_command, slice.range.command_count, false, force_redraw);
  }
}

void WebPlatformViews::SynchronizeFocus(std::uint32_t token, bool focus_visible) {
  if (!state_ || state_->runtime == nullptr) {
    return;
  }
  RuntimeAccess::SynchronizePlatformViewFocus(*state_->runtime, state_->IdentityForToken(token), focus_visible);
}

bool WebPlatformViews::HitTest(std::uint32_t token, Point point) const {
  if (!state_ || state_->runtime == nullptr) {
    return false;
  }
  const std::optional<std::uint64_t> identity = state_->IdentityForToken(token);
  return identity.has_value() && RuntimeAccess::HitTestPlatformView(*state_->runtime, point) == identity;
}

void WebPlatformViews::MoveFocus(std::uint32_t token, bool reverse) {
  if (!state_ || state_->runtime == nullptr) {
    return;
  }
  const std::optional<std::uint64_t> identity = state_->IdentityForToken(token);
  if (identity.has_value()) {
    RuntimeAccess::MoveFocusFromPlatformView(*state_->runtime, *identity, reverse);
  }
}

void WebPlatformViews::Shutdown() noexcept {
  if (!state_) {
    return;
  }
  for (auto& [identity, hosted] : state_->hosted) {
    static_cast<void>(identity);
    hosted->event_route->active = false;
    hosted->event_route->runtime = nullptr;
  }
  state_->hosted.clear();
  state_->identities_by_token.clear();
  for (auto& [key, slice] : state_->slices) {
    static_cast<void>(key);
    try {
      slice.canvas.call<void>("remove");
    } catch (...) {
    }
  }
  state_->slices.clear();
  state_->base_slice.reset();
  state_->order.clear();
  state_->runtime = nullptr;
}

} // namespace huxerui::detail
