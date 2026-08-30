#include "appkit_platform_view.h"

#import <QuartzCore/QuartzCore.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/macos/platform_registry.h>

#include "appkit_renderer.h"
#include "internal.h"

@interface HuxerUIPlatformSliceView : NSView {
@public
  huxerui::detail::AppKitRenderer* huxeruiRenderer;
  const huxerui::RenderFrame* huxeruiFrame;
  std::size_t huxeruiFirstCommand;
  std::size_t huxeruiCommandCount;
}
@end

@implementation HuxerUIPlatformSliceView

- (BOOL)isFlipped {
  return YES;
}

- (BOOL)isOpaque {
  return NO;
}

- (NSView*)hitTest:(NSPoint)point {
  static_cast<void>(point);
  return nil;
}

- (void)drawRect:(NSRect)dirtyRect {
  [super drawRect:dirtyRect];
  if (huxeruiRenderer == nullptr) {
    return;
  }
  huxeruiRenderer->DrawSlice(
      NSGraphicsContext.currentContext.CGContext,
      NSRectToCGRect(dirtyRect),
      huxeruiFrame,
      huxeruiFirstCommand,
      huxeruiCommandCount,
      false
  );
}

@end

@interface HuxerUIPlatformViewContainer : NSView
@end

@implementation HuxerUIPlatformViewContainer

- (BOOL)isFlipped {
  return YES;
}

@end

namespace huxerui::detail {

namespace {

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

struct EventRoute {
  Runtime* runtime = nullptr;
  std::uint64_t identity = 0;
  bool active = false;
};

struct HostedPlatformView {
  std::uint64_t properties_revision = 0;
  std::uint64_t controller_revision = 0;
  std::string type;
  std::shared_ptr<EventRoute> event_route;
  std::shared_ptr<const macos::detail::AppKitViewFactory> factory;
  std::shared_ptr<void> instance;
  PlatformValue controller;
  bool controller_connected = false;
  __strong NSView* view = nil;
  __strong HuxerUIPlatformViewContainer* container = nil;

  ~HostedPlatformView() {
    if (event_route) {
      event_route->active = false;
    }
    [container removeFromSuperview];
    if (instance && controller_connected && factory && factory->disconnect) {
      @try {
        try {
          factory->disconnect(instance, controller);
        } catch (...) {
        }
      } @catch (NSException* exception) {
        static_cast<void>(exception);
      }
    }
    if (instance && factory && factory->dispose) {
      @try {
        try {
          factory->dispose(instance);
        } catch (...) {
        }
      } @catch (NSException* exception) {
        static_cast<void>(exception);
      }
    }
  }

  void Update(const PlacePlatformViewCommand& command) {
    if (properties_revision != command.PropertiesRevision()) {
      if (!factory->update) {
        throw std::logic_error("HuxerUI macOS PlatformView factory does not support property updates");
      }
      @try {
        factory->update(instance, command.Properties());
      } @catch (NSException* exception) {
        static_cast<void>(exception);
        throw std::logic_error("HuxerUI macOS PlatformView factory raised an Objective-C exception while updating");
      }
      properties_revision = command.PropertiesRevision();
    }
    if (controller_revision == command.ControllerRevision()) {
      return;
    }
    if (controller_connected) {
      factory->disconnect(instance, controller);
      controller_connected = false;
    }
    controller = command.Controller();
    if (controller.HasValue()) {
      if (!factory->connect || !factory->disconnect) {
        throw std::logic_error("HuxerUI controlled macOS PlatformView factory must provide connect and disconnect");
      }
      factory->connect(instance, controller);
      controller_connected = true;
    }
    controller_revision = command.ControllerRevision();
  }
};

Rect VisibleBounds(const PlatformViewPlacement& placement) {
  return placement.clip.has_value() ? placement.world_bounds.Intersection(*placement.clip) : placement.world_bounds;
}

NSRect ToNSRect(Rect rect) {
  return NSMakeRect(rect.x, rect.y, std::max(0.0F, rect.width), std::max(0.0F, rect.height));
}

void InvalidateView(NSView* view, const DamageRegion& damage) {
  if (damage.full) {
    [view setNeedsDisplay:YES];
    return;
  }
  for (const Rect& rect : damage.rects) {
    if (!std::isfinite(rect.x) || !std::isfinite(rect.y) || !std::isfinite(rect.width) || !std::isfinite(rect.height)) {
      [view setNeedsDisplay:YES];
      return;
    }
    if (rect.IsEmpty()) {
      continue;
    }
    NSRect dirty_rect = [view convertRect:ToNSRect(rect) fromView:view.superview];
    dirty_rect = NSIntersectionRect(dirty_rect, view.bounds);
    if (NSIsEmptyRect(dirty_rect)) {
      continue;
    }
    NSRect backing_rect = [view convertRectToBacking:dirty_rect];
    const CGFloat left = std::floor(NSMinX(backing_rect));
    const CGFloat top = std::floor(NSMinY(backing_rect));
    const CGFloat right = std::ceil(NSMaxX(backing_rect));
    const CGFloat bottom = std::ceil(NSMaxY(backing_rect));
    backing_rect = NSMakeRect(left, top, right - left, bottom - top);
    [view setNeedsDisplayInRect:[view convertRectFromBacking:backing_rect]];
  }
}

NSView* ResponderView(NSResponder* responder) {
  if ([responder isKindOfClass:NSTextView.class] && static_cast<NSTextView*>(responder).isFieldEditor) {
    id delegate = static_cast<NSTextView*>(responder).delegate;
    return [delegate isKindOfClass:NSView.class] ? static_cast<NSView*>(delegate) : nil;
  }
  if ([responder isKindOfClass:NSView.class]) {
    return static_cast<NSView*>(responder);
  }
  return nil;
}

bool IsDescendant(NSView* view, NSView* ancestor) {
  for (NSView* current = view; current != nil; current = current.superview) {
    if (current == ancestor) {
      return true;
    }
  }
  return false;
}

} // namespace

struct AppKitPlatformViews::State {
  State(AppKitRenderer& renderer_value, PlatformRegistry& registry_value, Runtime& runtime_value,
        NSWindow* host_window_value)
      : renderer(&renderer_value), registry(&registry_value), runtime(&runtime_value), host_window(host_window_value) {
    if (host_window == nil) {
      throw std::logic_error("HuxerUI macOS PlatformView host window must not be nil");
    }
  }

  std::unique_ptr<HostedPlatformView> Create(const PlatformViewPlacement& placement) {
    const PlacePlatformViewCommand& command = *placement.command;
    std::shared_ptr<const macos::detail::AppKitViewFactory> factory =
        registry->FindView<macos::detail::AppKitViewFactory>(command.Type(), command.Properties().Type(),
                                                             command.Controller().Type());
    if (!factory->create) {
      throw std::logic_error("HuxerUI macOS PlatformView factory must provide create");
    }

    auto route = std::make_shared<EventRoute>(EventRoute{
        runtime,
        command.Identity(),
        false,
    });
    const std::weak_ptr<EventRoute> weak_route = route;
    PlatformEventEmitter events = MakePlatformEventEmitter(
        [weak_route](std::type_index key, PlatformValue value) -> std::optional<PlatformValue> {
          const std::shared_ptr<EventRoute> route = weak_route.lock();
          if (!route || ![NSThread isMainThread] || !route->active || route->runtime == nullptr) {
            return std::nullopt;
          }
          return RuntimeAccess::DispatchPlatformViewEvent(*route->runtime, route->identity, key, value);
        },
        [weak_route](std::string name, PlatformPayload payload) -> std::optional<PlatformPayload> {
          const std::shared_ptr<EventRoute> route = weak_route.lock();
          if (!route || ![NSThread isMainThread] || !route->active || route->runtime == nullptr) {
            return std::nullopt;
          }
          return RuntimeAccess::DispatchPlatformViewEvent(*route->runtime, route->identity, name, payload);
        });

    auto hosted = std::make_unique<HostedPlatformView>();
    hosted->properties_revision = command.PropertiesRevision();
    hosted->controller_revision = command.ControllerRevision();
    hosted->type = command.Type();
    hosted->event_route = std::move(route);
    hosted->factory = std::move(factory);
    hosted->controller = command.Controller();
    @try {
      NSWindow* owner_window = host_window;
      if (owner_window == nil) {
        throw std::logic_error("HuxerUI macOS PlatformView host window is unavailable");
      }
      hosted->instance = hosted->factory->create(owner_window, command.Properties(), std::move(events));
      if (!hosted->instance || !hosted->factory->view) {
        throw std::logic_error("HuxerUI macOS PlatformView factory returned an empty instance");
      }
      hosted->view = hosted->factory->view(hosted->instance);
    } @catch (NSException* exception) {
      static_cast<void>(exception);
      throw std::logic_error("HuxerUI macOS PlatformView factory raised an Objective-C exception while creating");
    }
    if (hosted->view == nil) {
      throw std::logic_error("HuxerUI macOS PlatformView factory returned a null NSView");
    }

    hosted->container = [[HuxerUIPlatformViewContainer alloc] initWithFrame:NSZeroRect];
    hosted->container.wantsLayer = YES;
    hosted->container.layer.masksToBounds = YES;
    [hosted->container addSubview:hosted->view];
    if (hosted->controller.HasValue()) {
      if (!hosted->factory->connect || !hosted->factory->disconnect) {
        throw std::logic_error("HuxerUI controlled macOS PlatformView factory must provide connect and disconnect");
      }
      hosted->factory->connect(hosted->instance, hosted->controller);
      hosted->controller_connected = true;
    }
    return hosted;
  }

  void Place(HostedPlatformView& hosted, const PlatformViewPlacement& placement) {
    const Rect visible_bounds = VisibleBounds(placement);
    const bool hidden = !placement.visible || visible_bounds.IsEmpty();
    const NSRect container_frame = ToNSRect(visible_bounds);
    const NSRect view_frame = ToNSRect({
        placement.world_bounds.x - visible_bounds.x,
        placement.world_bounds.y - visible_bounds.y,
        placement.world_bounds.width,
        placement.world_bounds.height,
    });
    if (hosted.container.hidden != hidden) {
      hosted.container.hidden = hidden;
    }
    if (!NSEqualRects(hosted.container.frame, container_frame)) {
      hosted.container.frame = container_frame;
    }
    if (!NSEqualRects(hosted.view.frame, view_frame)) {
      hosted.view.frame = view_frame;
    }
  }

  std::optional<std::uint64_t> IdentityForResponder(NSResponder* responder) const {
    NSView* responder_view = ResponderView(responder);
    if (responder_view == nil) {
      return std::nullopt;
    }
    for (const auto& [identity, hosted_view] : hosted) {
      if (IsDescendant(responder_view, hosted_view->view)) {
        return identity;
      }
    }
    return std::nullopt;
  }

  AppKitRenderer* renderer;
  PlatformRegistry* registry;
  Runtime* runtime;
  __weak NSWindow* host_window = nil;
  __weak NSView* root = nil;
  const RenderFrame* frame = nullptr;
  std::optional<RenderSlice> base_slice;
  std::optional<std::uint64_t> platform_view_focus_identity;
  std::optional<std::pair<std::uint64_t, bool>> pending_focus_traversal;
  std::unordered_map<std::uint64_t, std::unique_ptr<HostedPlatformView>> hosted;
  std::unordered_map<SliceKey, __strong HuxerUIPlatformSliceView*, SliceKeyHash> slices;
};

AppKitPlatformViews::AppKitPlatformViews(AppKitRenderer& renderer, PlatformRegistry& registry, Runtime& runtime,
                                         NSWindow* host_window)
    : state_(std::make_unique<State>(renderer, registry, runtime, host_window)) {}

AppKitPlatformViews::~AppKitPlatformViews() {
  Shutdown();
}

bool AppKitPlatformViews::Commit(NSView* root, const RenderFrame& frame) {
  NSArray<NSView*>* previous_subviews = [root.subviews copy];
  const std::optional<std::uint64_t> responder_identity = state_->IdentityForResponder(root.window.firstResponder);
  RenderComposition composition = BuildRenderComposition(frame.scene);
  std::unordered_set<std::uint64_t> retained_identities;
  std::vector<std::pair<std::uint64_t, std::unique_ptr<HostedPlatformView>>> pending;

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
      continue;
    }
    found->second->Update(command);
  }

  const bool focused_instance_removed =
      responder_identity.has_value() && (!retained_identities.contains(*responder_identity) ||
                                         std::ranges::any_of(pending, [responder_identity](const auto& entry) {
                                           return entry.first == *responder_identity;
                                         }));
  if (focused_instance_removed) {
    [root.window makeFirstResponder:root];
    state_->platform_view_focus_identity.reset();
  }
  for (auto& [identity, hosted] : pending) {
    state_->hosted.erase(identity);
    state_->hosted.emplace(identity, std::move(hosted));
  }
  std::erase_if(state_->hosted, [&retained_identities](const auto& entry) {
    return !retained_identities.contains(entry.first);
  });

  NSMutableArray<NSView*>* ordered_subviews = [NSMutableArray array];
  std::unordered_set<SliceKey, SliceKeyHash> retained_slices;
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
      HuxerUIPlatformSliceView* slice_view = state_->slices[key];
      if (slice_view == nil) {
        slice_view = [[HuxerUIPlatformSliceView alloc] initWithFrame:root.bounds];
        slice_view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        state_->slices[key] = slice_view;
      }
      slice_view->huxeruiRenderer = state_->renderer;
      slice_view->huxeruiFrame = &frame;
      slice_view->huxeruiFirstCommand = slice->first_command;
      slice_view->huxeruiCommandCount = slice->command_count;
      if (!NSEqualRects(slice_view.frame, root.bounds)) {
        slice_view.frame = root.bounds;
      }
      [ordered_subviews addObject:slice_view];
      continue;
    }

    const auto& placement = std::get<PlatformViewPlacement>(layer);
    passed_platform_view = true;
    HostedPlatformView& hosted = *state_->hosted.at(placement.command->Identity());
    state_->Place(hosted, placement);
    [ordered_subviews addObject:hosted.container];
  }

  std::erase_if(state_->slices, [&retained_slices](const auto& entry) {
    if (retained_slices.contains(entry.first)) {
      return false;
    }
    [entry.second removeFromSuperview];
    return true;
  });

  const bool composition_changed = ![previous_subviews isEqualToArray:ordered_subviews];
  if (composition_changed) {
    [root setSubviews:ordered_subviews];
  }
  state_->root = root;
  state_->frame = &frame;
  for (auto& [identity, hosted] : state_->hosted) {
    static_cast<void>(identity);
    hosted->event_route->active = true;
  }
  const std::optional<std::uint64_t> focused_identity = RuntimeAccess::FocusedPlatformView(*state_->runtime);
  const std::optional<std::uint64_t> current_responder_identity =
      state_->IdentityForResponder(root.window.firstResponder);
  if (focused_identity.has_value()) {
    const auto focused = state_->hosted.find(*focused_identity);
    if (focused == state_->hosted.end() || focused->second->container.hidden) {
      if (current_responder_identity == focused_identity) {
        [root.window makeFirstResponder:root];
      }
      state_->platform_view_focus_identity.reset();
      RuntimeAccess::SynchronizePlatformViewFocus(*state_->runtime, std::nullopt, false);
    } else if (current_responder_identity != focused_identity) {
      if ([root.window makeFirstResponder:focused->second->view]) {
        state_->platform_view_focus_identity = focused_identity;
      } else {
        state_->platform_view_focus_identity.reset();
        RuntimeAccess::SynchronizePlatformViewFocus(*state_->runtime, std::nullopt, false);
      }
    } else {
      state_->platform_view_focus_identity = focused_identity;
    }
  } else if (current_responder_identity.has_value()) {
    [root.window makeFirstResponder:root];
    state_->platform_view_focus_identity.reset();
  } else {
    state_->platform_view_focus_identity.reset();
  }
  for (const auto& [key, slice] : state_->slices) {
    static_cast<void>(key);
    if (composition_changed) {
      [slice setNeedsDisplay:YES];
    } else {
      InvalidateView(slice, frame.damage);
    }
  }
  return composition_changed;
}

void AppKitPlatformViews::DrawBase(CGContextRef context, CGRect dirty_rect) {
  if (state_->base_slice.has_value()) {
    const RenderSlice& slice = *state_->base_slice;
    state_->renderer->DrawSlice(context, dirty_rect, state_->frame, slice.first_command, slice.command_count, true);
    return;
  }
  state_->renderer->DrawSlice(context, dirty_rect, state_->frame, 0, 0, true);
}

NSView* AppKitPlatformViews::HitTest(Point point) const {
  if (state_->runtime == nullptr) {
    return nil;
  }
  const std::optional<std::uint64_t> identity = RuntimeAccess::HitTestPlatformView(*state_->runtime, point);
  if (!identity.has_value()) {
    return nil;
  }
  const auto found = state_->hosted.find(*identity);
  if (found == state_->hosted.end() || found->second->container.hidden || state_->root == nil) {
    return nil;
  }
  return [found->second->container hitTest:NSMakePoint(point.x, point.y)];
}

NSView* AppKitPlatformViews::AccessibilityView(std::uint64_t identity) const {
  const auto found = state_->hosted.find(identity);
  return found == state_->hosted.end() || found->second->container.hidden ? nil : found->second->view;
}

bool AppKitPlatformViews::BeginFocusTraversal(NSResponder* responder, bool reverse) {
  state_->pending_focus_traversal.reset();
  if (state_->runtime == nullptr) {
    return false;
  }
  const std::optional<std::uint64_t> identity = state_->IdentityForResponder(responder);
  if (!identity.has_value()) {
    return false;
  }
  state_->pending_focus_traversal = std::pair{*identity, reverse};
  return true;
}

void AppKitPlatformViews::EndFocusTraversal() {
  state_->pending_focus_traversal.reset();
}

void AppKitPlatformViews::SynchronizeFocus(NSResponder* responder) {
  if (state_->runtime == nullptr) {
    return;
  }
  const std::optional<std::uint64_t> identity = state_->IdentityForResponder(responder);
  if (identity.has_value()) {
    state_->platform_view_focus_identity = identity;
    if (RuntimeAccess::FocusedPlatformView(*state_->runtime) != identity) {
      const bool focus_visible = state_->pending_focus_traversal.has_value();
      RuntimeAccess::SynchronizePlatformViewFocus(*state_->runtime, identity, focus_visible);
    }
    return;
  }
  const std::optional<std::uint64_t> previous_platform_view_focus = state_->platform_view_focus_identity;
  state_->platform_view_focus_identity.reset();
  if (!previous_platform_view_focus.has_value()) {
    return;
  }
  const std::optional<std::uint64_t> focused = RuntimeAccess::FocusedPlatformView(*state_->runtime);
  if (focused != previous_platform_view_focus) {
    return;
  }
  if (state_->pending_focus_traversal.has_value() && state_->pending_focus_traversal->first == *focused) {
    const bool reverse = state_->pending_focus_traversal->second;
    state_->pending_focus_traversal.reset();
    RuntimeAccess::MoveFocusFromPlatformView(*state_->runtime, *focused, reverse);
    return;
  }
  RuntimeAccess::SynchronizePlatformViewFocus(*state_->runtime, std::nullopt, false);
}

void AppKitPlatformViews::Shutdown() {
  if (!state_) {
    return;
  }
  for (auto& [identity, hosted] : state_->hosted) {
    static_cast<void>(identity);
    hosted->event_route->active = false;
    hosted->event_route->runtime = nullptr;
  }
  for (auto& [key, slice] : state_->slices) {
    static_cast<void>(key);
    slice->huxeruiRenderer = nullptr;
    slice->huxeruiFrame = nullptr;
    [slice removeFromSuperview];
  }
  state_->slices.clear();
  state_->hosted.clear();
  state_->base_slice.reset();
  state_->platform_view_focus_identity.reset();
  state_->pending_focus_traversal.reset();
  state_->frame = nullptr;
  state_->root = nil;
  state_->runtime = nullptr;
}

} // namespace huxerui::detail
