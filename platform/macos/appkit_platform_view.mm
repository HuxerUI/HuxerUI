#include "appkit_platform_view.h"

#import <QuartzCore/QuartzCore.h>
#import <dispatch/dispatch.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/macos/platform_view.h>

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
  std::string type;
  std::shared_ptr<EventRoute> event_route;
  macos::PlatformViewFactory factory;
  __strong NSView* view = nil;
  __strong HuxerUIPlatformViewContainer* container = nil;

  ~HostedPlatformView() {
    if (event_route) {
      event_route->active = false;
    }
    [container removeFromSuperview];
    if (view != nil && factory.dispose) {
      @try {
        try {
          factory.dispose(view);
        } catch (...) {
        }
      } @catch (NSException* exception) {
        static_cast<void>(exception);
      }
    }
  }

  void Update(const PlatformPayload& properties) {
    if (!factory.update) {
      throw std::logic_error("HuxerUI macOS PlatformView factory does not support property updates");
    }
    @try {
      factory.update(view, properties);
    } @catch (NSException* exception) {
      static_cast<void>(exception);
      throw std::logic_error("HuxerUI macOS PlatformView factory raised an Objective-C exception while updating");
    }
  }
};

Rect VisibleBounds(const PlatformViewPlacement& placement) {
  return placement.clip.has_value() ? placement.world_bounds.Intersection(*placement.clip) : placement.world_bounds;
}

NSRect NativeRect(Rect rect) {
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
    NSRect dirty_rect = [view convertRect:NativeRect(rect) fromView:view.superview];
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

} // namespace

struct AppKitPlatformViewHost::State {
  State(AppKitRenderer& renderer_value, PlatformModules& modules_value, Runtime& runtime_value)
      : renderer(&renderer_value), modules(&modules_value), runtime(&runtime_value) {}

  std::unique_ptr<HostedPlatformView> Create(const PlatformViewPlacement& placement) {
    const PlacePlatformViewCommand& command = *placement.command;
    const auto* factory = modules->Find<macos::PlatformViewFactory>(command.Type());
    if (factory == nullptr) {
      throw std::logic_error("HuxerUI macOS PlatformView type is not registered: " + std::string(command.Type()));
    }
    if (!factory->create) {
      throw std::logic_error("HuxerUI macOS PlatformView factory must provide create");
    }

    auto route = std::make_shared<EventRoute>(EventRoute{
        runtime,
        command.Identity(),
        false,
    });
    const std::weak_ptr<EventRoute> weak_route = route;
    PlatformEventSink event_sink = [weak_route](std::string name, PlatformPayload payload) mutable {
      dispatch_async(dispatch_get_main_queue(), ^{
        const std::shared_ptr<EventRoute> route = weak_route.lock();
        if (!route || !route->active || route->runtime == nullptr) {
          return;
        }
        static_cast<void>(RuntimeAccess::DispatchPlatformViewEvent(*route->runtime, route->identity, name, payload));
      });
    };

    NSView* view = nil;
    @try {
      view = factory->create(command.Properties(), std::move(event_sink));
    } @catch (NSException* exception) {
      static_cast<void>(exception);
      throw std::logic_error("HuxerUI macOS PlatformView factory raised an Objective-C exception while creating");
    }
    if (view == nil) {
      throw std::logic_error("HuxerUI macOS PlatformView factory returned a null NSView");
    }

    auto hosted = std::make_unique<HostedPlatformView>();
    hosted->properties_revision = command.PropertiesRevision();
    hosted->type = command.Type();
    hosted->event_route = std::move(route);
    hosted->factory = *factory;
    hosted->view = view;
    hosted->container = [[HuxerUIPlatformViewContainer alloc] initWithFrame:NSZeroRect];
    hosted->container.wantsLayer = YES;
    hosted->container.layer.masksToBounds = YES;
    [hosted->container addSubview:hosted->view];
    return hosted;
  }

  void Place(HostedPlatformView& hosted, const PlatformViewPlacement& placement) {
    const Rect visible_bounds = VisibleBounds(placement);
    const bool hidden = !placement.visible || visible_bounds.IsEmpty();
    const NSRect container_frame = NativeRect(visible_bounds);
    const NSRect view_frame = NativeRect({
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

  AppKitRenderer* renderer;
  PlatformModules* modules;
  Runtime* runtime;
  __weak NSView* root = nil;
  const RenderFrame* frame = nullptr;
  std::optional<RenderSlice> base_slice;
  std::unordered_map<std::uint64_t, std::unique_ptr<HostedPlatformView>> hosted;
  std::unordered_map<SliceKey, __strong HuxerUIPlatformSliceView*, SliceKeyHash> slices;
};

AppKitPlatformViewHost::AppKitPlatformViewHost(AppKitRenderer& renderer, PlatformModules& modules, Runtime& runtime)
    : state_(std::make_unique<State>(renderer, modules, runtime)) {}

AppKitPlatformViewHost::~AppKitPlatformViewHost() {
  Shutdown();
}

bool AppKitPlatformViewHost::Commit(NSView* root, const RenderFrame& frame) {
  NSArray<NSView*>* previous_subviews = [root.subviews copy];
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
    if (found->second->properties_revision != command.PropertiesRevision()) {
      found->second->Update(command.Properties());
      found->second->properties_revision = command.PropertiesRevision();
    }
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

void AppKitPlatformViewHost::DrawBase(CGContextRef context, CGRect dirty_rect) {
  if (state_->base_slice.has_value()) {
    const RenderSlice& slice = *state_->base_slice;
    state_->renderer->DrawSlice(context, dirty_rect, state_->frame, slice.first_command, slice.command_count, true);
    return;
  }
  state_->renderer->DrawSlice(context, dirty_rect, state_->frame, 0, 0, true);
}

NSView* AppKitPlatformViewHost::HitTest(Point point) const {
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

void AppKitPlatformViewHost::Shutdown() {
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
  state_->frame = nullptr;
  state_->root = nil;
  state_->runtime = nullptr;
}

} // namespace huxerui::detail
