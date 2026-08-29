#import <AppKit/AppKit.h>
#import <dispatch/dispatch.h>

#include "apple_timer.h"
#include "runtime_test_support.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace huxerui::test {
namespace {

using namespace std::chrono_literals;

void DispatchToMainQueue(std::function<void()> task) {
  dispatch_async(dispatch_get_main_queue(), ^{
    try {
      task();
    } catch (...) {
    }
  });
}

template <class Predicate> bool RunMainLoopUntil(Predicate&& predicate, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!std::invoke(predicate) && std::chrono::steady_clock::now() < deadline) {
    [NSRunLoop.currentRunLoop runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
  }
  return std::invoke(predicate);
}

void RunMainLoopFor(std::chrono::milliseconds duration) {
  static_cast<void>(RunMainLoopUntil([] { return false; }, duration));
}

std::shared_ptr<example::TimerService> CreateTimer(TestPlatform& platform, NSWindow* window) {
  PlatformChannel channel = macos::detail::CreateObjectiveCPlatformModule(
      platform, window, example::CreateAppleTimerFactory(), {}
  );
  return example::CreateAppleTimerService(std::move(channel));
}

TEST_CASE("MacPlatformModuleUsesMainQueueWithoutInlineReentry") {
  @autoreleasepool {
    REQUIRE(NSThread.isMainThread);
    TestPlatform platform(DispatchToMainQueue);
    NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0.0, 0.0, 320.0, 200.0)
                                                   styleMask:NSWindowStyleMaskBorderless
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
    const std::shared_ptr<example::TimerService> timer = CreateTimer(platform, window);
    REQUIRE(timer != nullptr);

    bool inside_call = true;
    bool completed = false;
    bool ticked = false;
    bool completion_was_inline = false;
    bool completion_has_value = false;
    std::uint64_t completed_tick = 0;
    bool completion_on_main = false;
    bool event_on_main = false;
    std::vector<std::string> delivery_order;
    static_cast<void>(timer->Start(
        10ms,
        [&](std::uint64_t tick) {
          ticked = tick == 1;
          event_on_main = NSThread.isMainThread;
          delivery_order.push_back("event");
        },
        [&](PlatformResult<std::uint64_t> result) {
          completion_was_inline = inside_call;
          if (const auto* value = std::get_if<std::uint64_t>(&result)) {
            completion_has_value = true;
            completed_tick = *value;
          }
          completed = true;
          completion_on_main = NSThread.isMainThread;
          delivery_order.push_back("result");
        }
    ));
    inside_call = false;

    REQUIRE_FALSE(completed);
    REQUIRE_FALSE(ticked);
    REQUIRE(RunMainLoopUntil([&] { return completed && ticked; }, 1s));
    REQUIRE_FALSE(completion_was_inline);
    REQUIRE(completion_has_value);
    REQUIRE(completed_tick == 1);
    REQUIRE(completion_on_main);
    REQUIRE(event_on_main);
    REQUIRE(delivery_order == std::vector<std::string>{"result", "event"});

    const std::size_t delivered_events = delivery_order.size();
    bool stopped = false;
    bool stop_has_value = false;
    bool stop_on_main = false;
    static_cast<void>(timer->Stop([&](PlatformResult<std::monostate> result) {
      if (std::holds_alternative<std::monostate>(result)) {
        stop_has_value = true;
        stopped = true;
      }
      stop_on_main = NSThread.isMainThread;
    }));
    REQUIRE_FALSE(stopped);
    REQUIRE(RunMainLoopUntil([&] { return stopped; }, 1s));
    REQUIRE(stop_has_value);
    REQUIRE(stop_on_main);
    RunMainLoopFor(30ms);
    REQUIRE(delivery_order.size() == delivered_events);
  }
}

TEST_CASE("MacPlatformModuleCancelsAndDisposesFoundationTimer") {
  @autoreleasepool {
    bool completed = false;
    bool ticked = false;
    std::weak_ptr<example::TimerService> service_lifetime;
    {
      TestPlatform platform(DispatchToMainQueue);
      NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0.0, 0.0, 320.0, 200.0)
                                                     styleMask:NSWindowStyleMaskBorderless
                                                       backing:NSBackingStoreBuffered
                                                         defer:NO];
      std::shared_ptr<example::TimerService> timer = CreateTimer(platform, window);
      REQUIRE(timer != nullptr);
      service_lifetime = timer;

      const PlatformRequestId request = timer->Start(
          50ms,
          [&](std::uint64_t) { ticked = true; },
          [&](PlatformResult<std::uint64_t>) { completed = true; }
      );
      REQUIRE(timer->Cancel(request));
      REQUIRE_FALSE(timer->Cancel(request));
      RunMainLoopFor(100ms);
      REQUIRE_FALSE(completed);
      REQUIRE_FALSE(ticked);

      static_cast<void>(timer->Start(
          50ms,
          [&](std::uint64_t) { ticked = true; },
          [&](PlatformResult<std::uint64_t>) { completed = true; }
      ));
      timer.reset();
    }

    REQUIRE(service_lifetime.expired());
    RunMainLoopFor(100ms);
    REQUIRE_FALSE(completed);
    REQUIRE_FALSE(ticked);
  }
}

} // namespace
} // namespace huxerui::test
