#import <AppKit/AppKit.h>
#import <dispatch/dispatch.h>

#include "appkit_platform_view.h"
#include "appkit_renderer.h"
#include "runtime_test_support.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <huxerui/macos/platform_view.h>

namespace {

int mac_platform_view_frame_sets = 0;

} // namespace

@interface HuxerUITestPlatformView : NSButton
@end

@interface HuxerUITestPlatformRootView : NSView {
@public
  huxerui::detail::AppKitPlatformViews* huxeruiPlatformViews;
  huxerui::Runtime* huxeruiRuntime;
}
@end

@interface HuxerUITestPlatformWindow : NSWindow {
@public
  huxerui::detail::AppKitPlatformViews* huxeruiPlatformViews;
}
@end

@implementation HuxerUITestPlatformView

- (BOOL)acceptsFirstResponder {
  return YES;
}

- (void)setFrame:(NSRect)frame {
  ++mac_platform_view_frame_sets;
  [super setFrame:frame];
}

@end

@implementation HuxerUITestPlatformRootView

- (BOOL)acceptsFirstResponder {
  return YES;
}

- (BOOL)becomeFirstResponder {
  const BOOL became_first_responder = [super becomeFirstResponder];
  if (became_first_responder && huxeruiPlatformViews != nullptr) {
    huxeruiPlatformViews->SynchronizeFocus(self);
  }
  return became_first_responder;
}

- (void)keyDown:(NSEvent*)event {
  if (huxeruiRuntime == nullptr || event.keyCode != 48) {
    [super keyDown:event];
    return;
  }
  huxerui::KeyModifiers modifiers;
  modifiers.shift = (event.modifierFlags & NSEventModifierFlagShift) != 0;
  huxeruiRuntime->HandleKeyEvent({
      huxerui::KeyEventType::Down,
      huxerui::Key::Tab,
      {},
      modifiers,
  });
}

@end

@implementation HuxerUITestPlatformWindow

- (void)sendEvent:(NSEvent*)event {
  const bool traversal = huxeruiPlatformViews != nullptr && event.type == NSEventTypeKeyDown && !event.isARepeat &&
                         event.keyCode == 48 &&
                         huxeruiPlatformViews->BeginFocusTraversal(
                             self.firstResponder,
                             (event.modifierFlags & NSEventModifierFlagShift) != 0
                         );
  @try {
    [super sendEvent:event];
  } @finally {
    if (traversal) {
      huxeruiPlatformViews->EndFocusTraversal();
    }
  }
}

@end

namespace huxerui::test {
namespace {

State<int> mac_platform_view_value;
State<bool> mac_platform_view_visible;
int mac_platform_view_creates = 0;
int mac_platform_view_updates = 0;
int mac_platform_view_disposals = 0;
int mac_platform_view_event_value = 0;
PlatformEventSink mac_platform_view_event_sink;
NSView* mac_created_platform_view = nil;
NSTextField* mac_created_focus_text_field = nil;

struct MacPlatformViewEvents {
  struct Changed : Event<int> {
    static constexpr std::string_view Name = "changed";

    static int Decode(const PlatformPayload& payload) {
      return static_cast<int>(payload.AsInteger());
    }
  };
};

PlatformPayload MacPlatformProperties(int value) {
  return PlatformPayload::Object{{"value", value}};
}

View MacPlatformViewApp() {
  auto value = UseState(1);
  auto visible = UseState(true);
  mac_platform_view_value = value;
  mac_platform_view_visible = visible;
  if (!visible.Get()) {
    return Text("without platform view");
  }
  return Column {
    Button("before").With(Frame{80.0F, 20.0F}),
    PlatformView("test/MacView", MacPlatformProperties(value.Get()))
        .Events<MacPlatformViewEvents::Changed>()
        .On<MacPlatformViewEvents::Changed>([](int next) { mac_platform_view_event_value = next; })
        .With(Frame{80.0F, 40.0F}),
    Button("after").With(Frame{80.0F, 20.0F}),
  }.With(CrossAlign{CrossAxisAlignment::Start});
}

macos::PlatformViewFactory MacTestFactory() {
  return {
      .create = [](const PlatformPayload& properties, PlatformEventSink event_sink) -> NSView* {
        ++mac_platform_view_creates;
        mac_platform_view_event_sink = std::move(event_sink);
        NSButton* button = [[HuxerUITestPlatformView alloc] initWithFrame:NSZeroRect];
        button.title = [NSString stringWithFormat:@"%lld", properties.AsObject().at("value").AsInteger()];
        mac_created_platform_view = button;
        return button;
      },
      .update = [](NSView* view, const PlatformPayload& properties) {
        ++mac_platform_view_updates;
        static_cast<NSButton*>(view).title =
            [NSString stringWithFormat:@"%lld", properties.AsObject().at("value").AsInteger()];
      },
      .dispose = [](NSView* view) {
        static_cast<void>(view);
        ++mac_platform_view_disposals;
        mac_created_platform_view = nil;
      },
  };
}

View MacPlatformTextFieldFocusApp() {
  return Column {
    Button("before").With(Frame{80.0F, 20.0F}),
    PlatformView("test/MacTextField", MacPlatformProperties(1)).With(Frame{80.0F, 40.0F}),
    Button("after").With(Frame{80.0F, 20.0F}),
  }.With(CrossAlign{CrossAxisAlignment::Start});
}

macos::PlatformViewFactory MacTextFieldFactory() {
  return {
      .create = [](const PlatformPayload& properties, PlatformEventSink event_sink) -> NSView* {
        static_cast<void>(properties);
        static_cast<void>(event_sink);
        mac_created_focus_text_field = [[NSTextField alloc] initWithFrame:NSZeroRect];
        return mac_created_focus_text_field;
      },
      .update = [](NSView* view, const PlatformPayload& properties) {
        static_cast<void>(view);
        static_cast<void>(properties);
      },
      .dispose = [](NSView* view) {
        static_cast<void>(view);
        mac_created_focus_text_field = nil;
      },
  };
}

NSEvent* TabKeyEvent(NSWindow* window, bool reverse) {
  NSString* characters = reverse ? [NSString stringWithFormat:@"%C", static_cast<unichar>(NSBackTabCharacter)] : @"\t";
  return [NSEvent keyEventWithType:NSEventTypeKeyDown
                          location:NSZeroPoint
                     modifierFlags:reverse ? NSEventModifierFlagShift : 0
                         timestamp:0.0
                      windowNumber:window.windowNumber
                           context:nil
                        characters:characters
       charactersIgnoringModifiers:@"\t"
                         isARepeat:NO
                           keyCode:48];
}

bool DrainMainQueue() {
  __block bool drained = false;
  dispatch_async(dispatch_get_main_queue(), ^{
    drained = true;
  });
  NSDate* deadline = [NSDate dateWithTimeIntervalSinceNow:1.0];
  while (!drained && deadline.timeIntervalSinceNow > 0.0) {
    [NSRunLoop.currentRunLoop runMode:NSDefaultRunLoopMode beforeDate:deadline];
  }
  return drained;
}

TEST_CASE("MacPlatformViewsRetainUpdateOrderAndDisposeNativeViews") {
  @autoreleasepool {
    mac_platform_view_creates = 0;
    mac_platform_view_updates = 0;
    mac_platform_view_disposals = 0;
    mac_platform_view_event_value = 0;
    mac_platform_view_frame_sets = 0;
    mac_platform_view_event_sink = {};
    mac_created_platform_view = nil;

    TestPlatform platform;
    PlatformModules* modules = nullptr;
    AppOptions options{.show_debug_overlay = false};
    options.root_hooks.push_back([&](RootContext& root) {
      modules = &root.Modules();
      root.Modules().Register("test/MacView", MacTestFactory());
    });
    Runtime runtime(MacPlatformViewApp, platform, std::move(options));
    runtime.SetWindowMetrics({{200.0F, 120.0F}});
    REQUIRE(modules != nullptr);

    detail::AppKitRenderer renderer;
    detail::AppKitPlatformViews platform_views(renderer, *modules, runtime.NativeRuntime());
    NSView* root = [[NSView alloc] initWithFrame:NSMakeRect(0.0, 0.0, 200.0, 120.0)];

    REQUIRE(platform_views.Commit(root, runtime.BuildRenderFrame()));
    REQUIRE(mac_platform_view_creates == 1);
    REQUIRE(mac_platform_view_updates == 0);
    REQUIRE(mac_created_platform_view != nil);
    REQUIRE(root.subviews.count == 2);
    REQUIRE(mac_created_platform_view.superview == root.subviews.firstObject);
    REQUIRE(NSEqualRects(mac_created_platform_view.superview.frame, NSMakeRect(0.0, 20.0, 80.0, 40.0)));
    REQUIRE(platform_views.HitTest({20.0F, 30.0F}) == mac_created_platform_view);
    REQUIRE(platform_views.HitTest({120.0F, 30.0F}) == nil);

    const int placed_frame_sets = mac_platform_view_frame_sets;
    NSView* foreground_slice = root.subviews.lastObject;
    [root setNeedsDisplay:NO];
    [foreground_slice setNeedsDisplay:NO];
    REQUIRE_FALSE(platform_views.Commit(root, runtime.BuildRenderFrame()));
    REQUIRE(mac_platform_view_frame_sets == placed_frame_sets);
    REQUIRE_FALSE(root.needsDisplay);
    REQUIRE_FALSE(foreground_slice.needsDisplay);

    mac_platform_view_event_sink("changed", PlatformPayload(7));
    REQUIRE(DrainMainQueue());
    REQUIRE(mac_platform_view_event_value == 7);

    mac_platform_view_value = 2;
    REQUIRE_FALSE(platform_views.Commit(root, runtime.BuildRenderFrame()));
    REQUIRE(mac_platform_view_creates == 1);
    REQUIRE(mac_platform_view_updates == 1);
    REQUIRE(static_cast<NSButton*>(mac_created_platform_view).title.integerValue == 2);
    REQUIRE(root.subviews.count == 2);
    REQUIRE(mac_platform_view_frame_sets == placed_frame_sets);
    REQUIRE_FALSE(root.needsDisplay);

    mac_platform_view_visible = false;
    REQUIRE(platform_views.Commit(root, runtime.BuildRenderFrame()));
    REQUIRE(mac_platform_view_disposals == 1);
    REQUIRE(mac_created_platform_view == nil);
    REQUIRE(root.subviews.count == 0);

    mac_platform_view_event_sink("changed", PlatformPayload(9));
    REQUIRE(DrainMainQueue());
    REQUIRE(mac_platform_view_event_value == 7);

    platform_views.Shutdown();
    mac_platform_view_event_sink = {};
  }
}

TEST_CASE("MacPlatformViewsBridgeFocusAndAccessibilityIdentity") {
  @autoreleasepool {
    mac_platform_view_event_sink = {};
    mac_created_platform_view = nil;

    TestPlatform platform;
    PlatformModules* modules = nullptr;
    AppOptions options{.show_debug_overlay = false};
    options.root_hooks.push_back([&](RootContext& root) {
      modules = &root.Modules();
      root.Modules().Register("test/MacView", MacTestFactory());
    });
    Runtime runtime(MacPlatformViewApp, platform, std::move(options));
    runtime.SetWindowMetrics({{200.0F, 120.0F}});
    REQUIRE(modules != nullptr);

    detail::AppKitRenderer renderer;
    detail::AppKitPlatformViews platform_views(renderer, *modules, runtime.NativeRuntime());
    NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0.0, 0.0, 200.0, 120.0)
                                                   styleMask:NSWindowStyleMaskBorderless
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
    NSView* root = [[HuxerUITestPlatformRootView alloc] initWithFrame:NSMakeRect(0.0, 0.0, 200.0, 120.0)];
    window.contentView = root;
    REQUIRE([window makeFirstResponder:root]);

    const FrameCommit& initial = runtime.BuildCommit();
    REQUIRE(platform_views.Commit(root, initial.render_frame));
    const auto anchor = std::ranges::find_if(initial.semantic_frame->nodes, [](const SemanticNode& node) {
      return node.platform_view_identity.has_value();
    });
    REQUIRE(anchor != initial.semantic_frame->nodes.end());
    const std::uint64_t platform_view_identity = *anchor->platform_view_identity;
    REQUIRE(platform_views.AccessibilityView(platform_view_identity) == mac_created_platform_view);

    REQUIRE([window makeFirstResponder:mac_created_platform_view]);
    platform_views.SynchronizeFocus(window.firstResponder);
    REQUIRE(detail::RuntimeAccess::FocusedPlatformView(runtime.NativeRuntime()) == platform_view_identity);
    REQUIRE_FALSE(platform_views.Commit(root, runtime.BuildRenderFrame()));
    REQUIRE(window.firstResponder == mac_created_platform_view);

    mac_platform_view_visible = false;
    REQUIRE(platform_views.Commit(root, runtime.BuildRenderFrame()));
    REQUIRE(window.firstResponder == root);
    REQUIRE(platform_views.AccessibilityView(platform_view_identity) == nil);

    platform_views.Shutdown();
    mac_platform_view_event_sink = {};
  }
}

TEST_CASE("MacPlatformViewsTraverseBetweenNativeTextFieldAndRuntimeFocus") {
  @autoreleasepool {
    mac_created_focus_text_field = nil;

    TestPlatform platform;
    PlatformModules* modules = nullptr;
    AppOptions options{.show_debug_overlay = false};
    options.root_hooks.push_back([&](RootContext& root) {
      modules = &root.Modules();
      root.Modules().Register("test/MacTextField", MacTextFieldFactory());
    });
    Runtime runtime(MacPlatformTextFieldFocusApp, platform, std::move(options));
    runtime.SetWindowMetrics({{200.0F, 120.0F}});
    REQUIRE(modules != nullptr);

    detail::AppKitRenderer renderer;
    detail::AppKitPlatformViews platform_views(renderer, *modules, runtime.NativeRuntime());
    HuxerUITestPlatformWindow* window =
        [[HuxerUITestPlatformWindow alloc] initWithContentRect:NSMakeRect(0.0, 0.0, 200.0, 120.0)
                                                     styleMask:NSWindowStyleMaskBorderless
                                                       backing:NSBackingStoreBuffered
                                                         defer:NO];
    HuxerUITestPlatformRootView* root =
        [[HuxerUITestPlatformRootView alloc] initWithFrame:NSMakeRect(0.0, 0.0, 200.0, 120.0)];
    window->huxeruiPlatformViews = &platform_views;
    root->huxeruiPlatformViews = &platform_views;
    root->huxeruiRuntime = &runtime.NativeRuntime();
    window.contentView = root;
    window.initialFirstResponder = root;
    REQUIRE([window makeFirstResponder:root]);

    const FrameCommit& initial = runtime.BuildCommit();
    REQUIRE(platform_views.Commit(root, initial.render_frame));
    REQUIRE(mac_created_focus_text_field != nil);
    [window recalculateKeyViewLoop];
    const auto anchor = std::ranges::find_if(initial.semantic_frame->nodes, [](const SemanticNode& node) {
      return node.platform_view_identity.has_value();
    });
    REQUIRE(anchor != initial.semantic_frame->nodes.end());
    const SemanticNodeId anchor_id = anchor->id;

    REQUIRE(runtime.NativeRuntime().PerformSemanticAction(anchor_id, {SemanticActionKind::Focus, std::monostate{}}));
    REQUIRE_FALSE(platform_views.Commit(root, runtime.BuildRenderFrame()));
    REQUIRE([window.firstResponder isKindOfClass:NSTextView.class]);
    REQUIRE(
        static_cast<id>(static_cast<NSTextView*>(window.firstResponder).delegate) ==
        static_cast<id>(mac_created_focus_text_field)
    );

    [window sendEvent:TabKeyEvent(window, false)];
    REQUIRE(window.firstResponder == root);
    const FrameCommit& forward = runtime.BuildCommit();
    const auto after = std::ranges::find(forward.semantic_frame->nodes, std::string("after"), &SemanticNode::label);
    REQUIRE(after != forward.semantic_frame->nodes.end());
    REQUIRE(after->focused);
    REQUIRE_FALSE(platform_views.Commit(root, forward.render_frame));
    REQUIRE(window.firstResponder == root);

    REQUIRE(runtime.NativeRuntime().PerformSemanticAction(anchor_id, {SemanticActionKind::Focus, std::monostate{}}));
    REQUIRE_FALSE(platform_views.Commit(root, runtime.BuildRenderFrame()));
    REQUIRE([window.firstResponder isKindOfClass:NSTextView.class]);

    [window sendEvent:TabKeyEvent(window, true)];
    REQUIRE(window.firstResponder == root);
    const FrameCommit& reverse = runtime.BuildCommit();
    const auto before = std::ranges::find(reverse.semantic_frame->nodes, std::string("before"), &SemanticNode::label);
    REQUIRE(before != reverse.semantic_frame->nodes.end());
    REQUIRE(before->focused);

    window->huxeruiPlatformViews = nullptr;
    root->huxeruiPlatformViews = nullptr;
    root->huxeruiRuntime = nullptr;
    [window makeFirstResponder:nil];
    platform_views.Shutdown();
    window.contentView = nil;
    [window close];
  }
}

} // namespace
} // namespace huxerui::test
