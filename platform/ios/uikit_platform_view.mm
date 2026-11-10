#include "uikit_platform_view.h"
#include "internal_access.h"

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

#include <huxerui/ios/platform_registry.h>

#include "application/platform_frame_internal.h"
#include "uikit_renderer.h"
#include "runtime/ui_window_internal.h"

@interface HuxerUIPlatformSliceView : UIView {
@public
  huxerui::detail::UIKitPlatformViews* huxeruiPlatformViews;
  std::size_t huxeruiFirstCommand;
  std::size_t huxeruiCommandCount;
}
@end

@implementation HuxerUIPlatformSliceView

- (instancetype)initWithFrame:(CGRect)frame {
  self = [super initWithFrame:frame];
  if (self != nil) {
    self.backgroundColor = UIColor.clearColor;
    self.opaque = NO;
    self.userInteractionEnabled = NO;
    self.isAccessibilityElement = NO;
    self.contentMode = UIViewContentModeRedraw;
  }
  return self;
}

- (void)drawRect:(CGRect)rect {
  [super drawRect:rect];
  if (huxeruiPlatformViews == nullptr) {
    return;
  }
  huxeruiPlatformViews->DrawSlice(UIGraphicsGetCurrentContext(), rect, huxeruiFirstCommand, huxeruiCommandCount);
}

@end

@interface HuxerUIPlatformViewContainer : UIView
@end

@implementation HuxerUIPlatformViewContainer

- (instancetype)initWithFrame:(CGRect)frame {
  self = [super initWithFrame:frame];
  if (self != nil) {
    self.backgroundColor = UIColor.clearColor;
    self.clipsToBounds = YES;
    self.isAccessibilityElement = NO;
  }
  return self;
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {}

- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {}

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
  UiWindow* ui_window = nullptr;
  std::uint64_t identity = 0;
  bool active = false;
};

struct HostedPlatformView {
  std::uint64_t properties_revision = 0;
  std::uint64_t controller_revision = 0;
  std::string type;
  std::shared_ptr<EventRoute> event_route;
  std::shared_ptr<const ios::detail::UIKitViewFactory> factory;
  std::shared_ptr<void> instance;
  PlatformValue controller;
  bool controller_connected = false;
  __strong UIView* view = nil;
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
      } @catch (NSException*) {}
    }
    if (instance && factory && factory->dispose) {
      @try {
        try {
          factory->dispose(instance);
        } catch (...) {
        }
      } @catch (NSException*) {}
    }
  }

  void Update(const PlacePlatformViewCommand& command) {
    if (properties_revision != command.PropertiesRevision()) {
      if (!factory->update) {
        throw std::logic_error("HuxerUI iOS PlatformView factory does not support property updates");
      }
      @try {
        factory->update(instance, command.Properties());
      } @catch (NSException*) {
        throw std::logic_error("HuxerUI iOS PlatformView factory raised an Objective-C exception while updating");
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
        throw std::logic_error("HuxerUI controlled iOS PlatformView factory must provide connect and disconnect");
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

CGRect ToCGRect(Rect rect) {
  return CGRectMake(rect.x, rect.y, std::max(0.0F, rect.width), std::max(0.0F, rect.height));
}

void InvalidateView(UIView* view, const DamageRegion& damage) {
  if (damage.full) {
    [view setNeedsDisplay];
    return;
  }
  const CGFloat scale = std::max<CGFloat>(1.0, view.contentScaleFactor);
  for (const Rect& rect : damage.rects) {
    if (!std::isfinite(rect.x) || !std::isfinite(rect.y) || !std::isfinite(rect.width) || !std::isfinite(rect.height)) {
      [view setNeedsDisplay];
      return;
    }
    if (rect.IsEmpty()) {
      continue;
    }
    CGRect dirty = CGRectIntersection(ToCGRect(rect), view.bounds);
    if (CGRectIsEmpty(dirty)) {
      continue;
    }
    const CGFloat left = std::floor(CGRectGetMinX(dirty) * scale) / scale;
    const CGFloat top = std::floor(CGRectGetMinY(dirty) * scale) / scale;
    const CGFloat right = std::ceil(CGRectGetMaxX(dirty) * scale) / scale;
    const CGFloat bottom = std::ceil(CGRectGetMaxY(dirty) * scale) / scale;
    [view setNeedsDisplayInRect:CGRectMake(left, top, right - left, bottom - top)];
  }
}

UIView* FindFirstResponder(UIView* root) {
  if (root.isFirstResponder) {
    return root;
  }
  for (UIView* subview in root.subviews) {
    if (UIView* responder = FindFirstResponder(subview)) {
      return responder;
    }
  }
  return nil;
}

void ResignFirstResponder(UIView* root) {
  if (UIView* responder = FindFirstResponder(root)) {
    [responder resignFirstResponder];
  }
}

bool IsDescendant(UIView* view, UIView* ancestor) {
  for (UIView* current = view; current != nil; current = current.superview) {
    if (current == ancestor) {
      return true;
    }
  }
  return false;
}

} // namespace

struct UIKitPlatformViews::Impl {
  Impl(UIKitRenderer& renderer_value, FrameCoordinator& frame_coordinator_value, PlatformRegistry& registry_value,
       UiWindow& ui_window_value)
      : renderer(&renderer_value), frame_coordinator(frame_coordinator_value), registry(&registry_value),
        ui_window(&ui_window_value) {}

  std::unique_ptr<HostedPlatformView> Create(UIView* root, const PlatformViewPlacement& placement) {
    const PlacePlatformViewCommand& command = *placement.command;
    std::shared_ptr<const ios::detail::UIKitViewFactory> factory = registry->FindView<ios::detail::UIKitViewFactory>(*ui_window,
        command.Type(), command.Properties().Type(), command.Controller().Type());
    if (!factory->create) {
      throw std::logic_error("HuxerUI iOS PlatformView factory must provide create");
    }

    auto route = std::make_shared<EventRoute>(EventRoute{
        ui_window,
        command.Identity(),
        false,
    });
    const std::weak_ptr<EventRoute> weak_route = route;
    PlatformEventEmitter events = MakePlatformEventEmitter(
        [weak_route](std::type_index key, PlatformValue value) -> std::optional<PlatformValue> {
          const std::shared_ptr<EventRoute> route = weak_route.lock();
          if (!route || ![NSThread isMainThread] || !route->active || route->ui_window == nullptr) {
            return std::nullopt;
          }
          return InternalAccess::DispatchPlatformViewEvent(*route->ui_window, route->identity, key, value);
        },
        [weak_route](std::string name, PlatformPayload payload) -> std::optional<PlatformPayload> {
          const std::shared_ptr<EventRoute> route = weak_route.lock();
          if (!route || ![NSThread isMainThread] || !route->active || route->ui_window == nullptr) {
            return std::nullopt;
          }
          return InternalAccess::DispatchPlatformViewEvent(*route->ui_window, route->identity, name, payload);
        });

    auto hosted = std::make_unique<HostedPlatformView>();
    hosted->properties_revision = command.PropertiesRevision();
    hosted->controller_revision = command.ControllerRevision();
    hosted->type = command.Type();
    hosted->event_route = std::move(route);
    hosted->factory = std::move(factory);
    hosted->controller = command.Controller();
    @try {
      hosted->instance =
          hosted->factory->create(root.window.rootViewController, command.Properties(), std::move(events));
      if (!hosted->instance || !hosted->factory->view) {
        throw std::logic_error("HuxerUI iOS PlatformView factory returned an empty instance");
      }
      hosted->view = hosted->factory->view(hosted->instance);
    } @catch (NSException*) {
      throw std::logic_error("HuxerUI iOS PlatformView factory raised an Objective-C exception while creating");
    }
    if (hosted->view == nil) {
      throw std::logic_error("HuxerUI iOS PlatformView factory returned a null UIView");
    }

    hosted->container = [[HuxerUIPlatformViewContainer alloc] initWithFrame:CGRectZero];
    [hosted->container addSubview:hosted->view];
    if (hosted->controller.HasValue()) {
      if (!hosted->factory->connect || !hosted->factory->disconnect) {
        throw std::logic_error("HuxerUI controlled iOS PlatformView factory must provide connect and disconnect");
      }
      hosted->factory->connect(hosted->instance, hosted->controller);
      hosted->controller_connected = true;
    }
    return hosted;
  }

  void Place(HostedPlatformView& hosted, const PlatformViewPlacement& placement) {
    const Rect visible_bounds = VisibleBounds(placement);
    const bool hidden = !placement.visible || visible_bounds.IsEmpty();
    const CGRect container_frame = ToCGRect(visible_bounds);
    const CGRect view_frame = ToCGRect({
        placement.world_bounds.x - visible_bounds.x,
        placement.world_bounds.y - visible_bounds.y,
        placement.world_bounds.width,
        placement.world_bounds.height,
    });
    if (hosted.container.hidden != hidden) {
      hosted.container.hidden = hidden;
    }
    if (!CGRectEqualToRect(hosted.container.frame, container_frame)) {
      hosted.container.frame = container_frame;
    }
    if (!CGRectEqualToRect(hosted.view.frame, view_frame)) {
      hosted.view.frame = view_frame;
    }
  }

  std::optional<std::uint64_t> IdentityForResponder(UIView* responder) const {
    if (responder == nil) {
      return std::nullopt;
    }
    for (const auto& [identity, hosted_view] : hosted) {
      if (IsDescendant(responder, hosted_view->view)) {
        return identity;
      }
    }
    return std::nullopt;
  }

  UIKitRenderer* renderer;
  FrameCoordinator& frame_coordinator;
  PlatformRegistry* registry;
  UiWindow* ui_window;
  __weak UIView* root = nil;
  const RenderFrame* frame = nullptr;
  std::optional<RenderSlice> base_slice;
  std::unordered_map<std::uint64_t, std::unique_ptr<HostedPlatformView>> hosted;
  std::unordered_map<SliceKey, __strong HuxerUIPlatformSliceView*, SliceKeyHash> slices;
};

UIKitPlatformViews::UIKitPlatformViews(UIKitRenderer& renderer, FrameCoordinator& frame_coordinator,
                                       PlatformRegistry& registry, UiWindow& ui_window)
    : impl_(std::make_unique<Impl>(renderer, frame_coordinator, registry, ui_window)) {}

UIKitPlatformViews::~UIKitPlatformViews() {
  Shutdown();
}

bool UIKitPlatformViews::Commit(UIView* root, const RenderFrame& frame) {
  NSArray<UIView*>* previous_subviews = [root.subviews copy];
  const std::optional<std::uint64_t> responder_identity = impl_->IdentityForResponder(FindFirstResponder(root));
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
    const auto found = impl_->hosted.find(command.Identity());
    if (found == impl_->hosted.end() || found->second->type != command.Type()) {
      pending.emplace_back(command.Identity(), impl_->Create(root, *placement));
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
    ResignFirstResponder(root);
  }
  for (auto& [identity, hosted] : pending) {
    impl_->hosted.erase(identity);
    impl_->hosted.emplace(identity, std::move(hosted));
  }
  std::erase_if(impl_->hosted, [&retained_identities](const auto& entry) {
    return !retained_identities.contains(entry.first);
  });

  NSMutableArray<UIView*>* ordered_subviews = [NSMutableArray array];
  std::unordered_set<SliceKey, SliceKeyHash> retained_slices;
  impl_->base_slice.reset();
  bool passed_platform_view = false;
  for (const RenderCompositionLayer& layer : composition.layers) {
    if (const auto* slice = std::get_if<RenderSlice>(&layer)) {
      if (!passed_platform_view && !impl_->base_slice.has_value()) {
        impl_->base_slice = *slice;
        continue;
      }
      const SliceKey key{slice->preceding_platform_view, slice->following_platform_view};
      retained_slices.insert(key);
      HuxerUIPlatformSliceView* slice_view = impl_->slices[key];
      if (slice_view == nil) {
        slice_view = [[HuxerUIPlatformSliceView alloc] initWithFrame:root.bounds];
        slice_view.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
        slice_view->huxeruiPlatformViews = this;
        impl_->slices[key] = slice_view;
      }
      slice_view->huxeruiFirstCommand = slice->first_command;
      slice_view->huxeruiCommandCount = slice->command_count;
      if (!CGRectEqualToRect(slice_view.frame, root.bounds)) {
        slice_view.frame = root.bounds;
      }
      [ordered_subviews addObject:slice_view];
      continue;
    }

    const auto& placement = std::get<PlatformViewPlacement>(layer);
    passed_platform_view = true;
    HostedPlatformView& hosted = *impl_->hosted.at(placement.command->Identity());
    impl_->Place(hosted, placement);
    [ordered_subviews addObject:hosted.container];
  }

  std::erase_if(impl_->slices, [&retained_slices](const auto& entry) {
    if (retained_slices.contains(entry.first)) {
      return false;
    }
    entry.second->huxeruiPlatformViews = nullptr;
    [entry.second removeFromSuperview];
    return true;
  });

  const bool composition_changed = ![previous_subviews isEqualToArray:ordered_subviews];
  if (composition_changed) {
    for (NSUInteger index = 0; index < ordered_subviews.count; ++index) {
      UIView* subview = ordered_subviews[index];
      if (subview.superview != root || index >= root.subviews.count || root.subviews[index] != subview) {
        [root insertSubview:subview atIndex:index];
      }
    }
  }
  impl_->root = root;
  impl_->frame = &frame;
  for (auto& [identity, hosted] : impl_->hosted) {
    hosted->event_route->active = true;
  }
  const std::optional<std::uint64_t> focused_identity = InternalAccess::FocusedPlatformView(*impl_->ui_window);
  const std::optional<std::uint64_t> current_responder_identity =
      impl_->IdentityForResponder(FindFirstResponder(root));
  if (focused_identity.has_value()) {
    const auto focused = impl_->hosted.find(*focused_identity);
    if (focused == impl_->hosted.end() || focused->second->container.hidden) {
      if (current_responder_identity == focused_identity) {
        ResignFirstResponder(root);
      }
      InternalAccess::SynchronizePlatformViewFocus(*impl_->ui_window, std::nullopt, false);
    } else if (current_responder_identity != focused_identity) {
      if (![focused->second->view becomeFirstResponder]) {
        InternalAccess::SynchronizePlatformViewFocus(*impl_->ui_window, std::nullopt, false);
      }
    }
  } else if (current_responder_identity.has_value()) {
    ResignFirstResponder(root);
  }
  for (const auto& [key, slice] : impl_->slices) {
    if (composition_changed) {
      [slice setNeedsDisplay];
    } else {
      InvalidateView(slice, frame.damage);
    }
  }
  return composition_changed;
}

std::optional<double> UIKitPlatformViews::DrawBase(CGContextRef context, CGRect dirty_rect, bool platform_ready) {
  if (impl_->frame_coordinator.DeferPaintIfCommitting()) {
    return std::nullopt;
  }
  impl_->frame_coordinator.BeginPaint();
  if (impl_->base_slice.has_value()) {
    const RenderSlice& slice = *impl_->base_slice;
    impl_->renderer->DrawSlice(context, dirty_rect, impl_->frame, slice.first_command, slice.command_count, true);
  } else {
    impl_->renderer->DrawSlice(context, dirty_rect, impl_->frame, 0, 0, true);
  }
  return impl_->frame_coordinator.EndPaint(platform_ready);
}

void UIKitPlatformViews::DrawSlice(CGContextRef context, CGRect dirty_rect, std::size_t first_command,
                                  std::size_t command_count) {
  if (!impl_->frame_coordinator.DeferPaintIfCommitting()) {
    impl_->renderer->DrawSlice(context, dirty_rect, impl_->frame, first_command, command_count, false);
  }
}

void UIKitPlatformViews::InvalidateSlices() {
  for (const auto& [key, slice] : impl_->slices) {
    [slice setNeedsDisplay];
  }
}

UIView* UIKitPlatformViews::HitTest(Point point, UIEvent* event) {
  if (impl_->ui_window == nullptr) {
    return nil;
  }
  const std::optional<std::uint64_t> identity = InternalAccess::HitTestPlatformView(*impl_->ui_window, point);
  if (!identity.has_value()) {
    return nil;
  }
  const auto found = impl_->hosted.find(*identity);
  if (found == impl_->hosted.end() || found->second->container.hidden || impl_->root == nil) {
    return nil;
  }
  const CGPoint root_point = CGPointMake(point.x, point.y);
  const CGPoint container_point = [found->second->container convertPoint:root_point fromView:impl_->root];
  UIView* target = [found->second->container hitTest:container_point withEvent:event];
  if (target != nil && event != nil && event.type == UIEventTypeTouches &&
      InternalAccess::FocusedPlatformView(*impl_->ui_window) != identity) {
    InternalAccess::SynchronizePlatformViewFocus(*impl_->ui_window, identity, false);
  }
  return target;
}

UIView* UIKitPlatformViews::AccessibilityView(std::uint64_t identity) const noexcept {
  if (!impl_) {
    return nil;
  }
  const auto found = impl_->hosted.find(identity);
  return found == impl_->hosted.end() ? nil : found->second->view;
}

void UIKitPlatformViews::ClearFocus() {
  if (!impl_ || impl_->ui_window == nullptr) {
    return;
  }
  UIView* responder = impl_->root == nil ? nil : FindFirstResponder(impl_->root);
  if (impl_->IdentityForResponder(responder).has_value()) {
    [responder resignFirstResponder];
  }
  if (InternalAccess::FocusedPlatformView(*impl_->ui_window).has_value()) {
    InternalAccess::SynchronizePlatformViewFocus(*impl_->ui_window, std::nullopt, false);
  }
}

void UIKitPlatformViews::Shutdown() {
  if (!impl_) {
    return;
  }
  for (auto& [identity, hosted] : impl_->hosted) {
    hosted->event_route->active = false;
    hosted->event_route->ui_window = nullptr;
  }
  for (auto& [key, slice] : impl_->slices) {
    slice->huxeruiPlatformViews = nullptr;
    [slice removeFromSuperview];
  }
  impl_->slices.clear();
  impl_->hosted.clear();
  impl_->base_slice.reset();
  impl_->frame = nullptr;
  impl_->root = nil;
  impl_->ui_window = nullptr;
}

} // namespace huxerui::detail
