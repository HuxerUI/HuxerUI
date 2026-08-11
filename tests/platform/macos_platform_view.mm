#import <AppKit/AppKit.h>

#include "appkit_platform_view.h"
#include "appkit_renderer.h"
#include "runtime_test_support.h"

#include <string_view>
#include <utility>

#include <huxerui/macos/platform_view.h>

namespace {

int mac_platform_view_frame_sets = 0;

} // namespace

@interface HuxerUITestPlatformView : NSButton
@end

@implementation HuxerUITestPlatformView

- (void)setFrame:(NSRect)frame {
  ++mac_platform_view_frame_sets;
  [super setFrame:frame];
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

struct MacPlatformViewEvents {
  struct Changed : Event<int> {
    static constexpr std::string_view PlatformName = "changed";

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
    Text("before").With(Frame{80.0F, 20.0F}),
    PlatformView("test/MacView", MacPlatformProperties(value.Get()))
        .Events<MacPlatformViewEvents::Changed>()
        .On<MacPlatformViewEvents::Changed>([](int next) { mac_platform_view_event_value = next; })
        .With(Frame{80.0F, 40.0F}),
    Text("after").With(Frame{80.0F, 20.0F}),
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

TEST_CASE("MacPlatformViewHostRetainsUpdatesOrdersAndDisposesNativeViews") {
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
    detail::AppKitPlatformViewHost host(renderer, *modules, runtime.NativeRuntime());
    NSView* root = [[NSView alloc] initWithFrame:NSMakeRect(0.0, 0.0, 200.0, 120.0)];

    REQUIRE(host.Commit(root, runtime.BuildRenderFrame()));
    REQUIRE(mac_platform_view_creates == 1);
    REQUIRE(mac_platform_view_updates == 0);
    REQUIRE(mac_created_platform_view != nil);
    REQUIRE(root.subviews.count == 2);
    REQUIRE(mac_created_platform_view.superview == root.subviews.firstObject);
    REQUIRE(NSEqualRects(mac_created_platform_view.superview.frame, NSMakeRect(0.0, 20.0, 80.0, 40.0)));
    REQUIRE(host.HitTest({20.0F, 30.0F}) == mac_created_platform_view);
    REQUIRE(host.HitTest({120.0F, 30.0F}) == nil);

    const int placed_frame_sets = mac_platform_view_frame_sets;
    NSView* foreground_slice = root.subviews.lastObject;
    [root setNeedsDisplay:NO];
    [foreground_slice setNeedsDisplay:NO];
    REQUIRE_FALSE(host.Commit(root, runtime.BuildRenderFrame()));
    REQUIRE(mac_platform_view_frame_sets == placed_frame_sets);
    REQUIRE_FALSE(root.needsDisplay);
    REQUIRE_FALSE(foreground_slice.needsDisplay);

    mac_platform_view_event_sink("changed", PlatformPayload(7));
    [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    REQUIRE(mac_platform_view_event_value == 7);

    mac_platform_view_value = 2;
    REQUIRE_FALSE(host.Commit(root, runtime.BuildRenderFrame()));
    REQUIRE(mac_platform_view_creates == 1);
    REQUIRE(mac_platform_view_updates == 1);
    REQUIRE(static_cast<NSButton*>(mac_created_platform_view).title.integerValue == 2);
    REQUIRE(root.subviews.count == 2);
    REQUIRE(mac_platform_view_frame_sets == placed_frame_sets);
    REQUIRE_FALSE(root.needsDisplay);

    mac_platform_view_visible = false;
    REQUIRE(host.Commit(root, runtime.BuildRenderFrame()));
    REQUIRE(mac_platform_view_disposals == 1);
    REQUIRE(mac_created_platform_view == nil);
    REQUIRE(root.subviews.count == 0);

    mac_platform_view_event_sink("changed", PlatformPayload(9));
    [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    REQUIRE(mac_platform_view_event_value == 7);

    host.Shutdown();
    mac_platform_view_event_sink = {};
  }
}

} // namespace
} // namespace huxerui::test
