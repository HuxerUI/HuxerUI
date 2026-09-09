#include "linux_internal.h"

#include "linux_ui_dispatcher.h"
#include "runtime_test_support.h"
#include "timer.h"

#include <glib.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace huxerui::test {
namespace {

using namespace std::chrono_literals;

std::weak_ptr<example::TimerService> linux_timer_service;

View LinuxPlatformModuleApp() {
  linux_timer_service = example::UseTimer();
  return Text("platform module");
}

AppOptions LinuxPlatformModuleOptions() {
  AppOptions options{.show_debug_overlay = false};
  options.root_hooks.push_back(example::InstallTimer);
  return options;
}

template <class Predicate>
bool RunDispatcherUntil(
    detail::LinuxUIThreadDispatcher& dispatcher, Predicate&& predicate, std::chrono::milliseconds timeout
) {
  static_cast<void>(dispatcher);
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!std::invoke(predicate) && std::chrono::steady_clock::now() < deadline) {
    while (g_main_context_iteration(nullptr, FALSE) != FALSE) {
    }
    std::this_thread::sleep_for(1ms);
  }
  while (g_main_context_iteration(nullptr, FALSE) != FALSE) {
  }
  return std::invoke(predicate);
}

void RunDispatcherFor(detail::LinuxUIThreadDispatcher& dispatcher, std::chrono::milliseconds duration) {
  static_cast<void>(RunDispatcherUntil(dispatcher, [] { return false; }, duration));
}

TEST_CASE("LinuxUIThreadDispatcherWakesMainContextAndPreservesOrder") {
  detail::LinuxUIThreadDispatcher dispatcher;
  UIThreadDispatcher post = dispatcher.Bind();
  const std::thread::id ui_thread = std::this_thread::get_id();
  std::vector<int> order;
  bool ran_inline = false;
  bool inside_post = true;

  std::thread worker([&] {
    post([&] {
      ran_inline = inside_post;
      REQUIRE(std::this_thread::get_id() == ui_thread);
      order.push_back(1);
    });
    post([&] { order.push_back(2); });
  });
  worker.join();
  inside_post = false;

  REQUIRE(order.empty());
  REQUIRE(RunDispatcherUntil(dispatcher, [&] { return order.size() == 2; }, 1s));
  REQUIRE_FALSE(ran_inline);
  REQUIRE(order == std::vector<int>{1, 2});
}

TEST_CASE("LinuxPlatformModuleUsesUIThreadWithoutInlineReentry") {
  linux_timer_service.reset();
  detail::LinuxUIThreadDispatcher dispatcher;
  TestPlatform platform(dispatcher.Bind());
  Runtime runtime(LinuxPlatformModuleApp, platform, LinuxPlatformModuleOptions());
  runtime.SetWindowMetrics({{320.0F, 200.0F}});
  static_cast<void>(runtime.BuildRenderFrame());
  const std::shared_ptr<example::TimerService> timer = linux_timer_service.lock();
  REQUIRE(timer != nullptr);

  const std::thread::id ui_thread = std::this_thread::get_id();
  bool inside_call = true;
  bool completed = false;
  bool ticked = false;
  bool completion_was_inline = false;
  bool completion_has_value = false;
  bool completion_on_ui = false;
  bool event_on_ui = false;
  std::uint64_t completed_tick = 0;
  std::vector<std::string> delivery_order;
  static_cast<void>(timer->Start(
      10ms,
      [&](std::uint64_t tick) {
        ticked = tick == 1;
        event_on_ui = std::this_thread::get_id() == ui_thread;
        delivery_order.push_back("event");
      },
      [&](PlatformResult<std::uint64_t> result) {
        completion_was_inline = inside_call;
        if (const auto* value = std::get_if<std::uint64_t>(&result)) {
          completion_has_value = true;
          completed_tick = *value;
        }
        completed = true;
        completion_on_ui = std::this_thread::get_id() == ui_thread;
        delivery_order.push_back("result");
      }
  ));
  inside_call = false;

  REQUIRE_FALSE(completed);
  REQUIRE_FALSE(ticked);
  REQUIRE(RunDispatcherUntil(dispatcher, [&] { return completed && ticked; }, 1s));
  REQUIRE_FALSE(completion_was_inline);
  REQUIRE(completion_has_value);
  REQUIRE(completed_tick == 1);
  REQUIRE(completion_on_ui);
  REQUIRE(event_on_ui);
  REQUIRE(delivery_order == std::vector<std::string>{"result", "event"});

  const std::size_t delivered_events = delivery_order.size();
  bool stopped = false;
  static_cast<void>(timer->Stop([&](PlatformResult<std::monostate> result) {
    stopped = std::holds_alternative<std::monostate>(result);
  }));
  REQUIRE_FALSE(stopped);
  REQUIRE(RunDispatcherUntil(dispatcher, [&] { return stopped; }, 1s));
  RunDispatcherFor(dispatcher, 30ms);
  REQUIRE(delivery_order.size() == delivered_events);
}

TEST_CASE("LinuxPlatformModuleReplacesCancelsAndDisposesTimer") {
  linux_timer_service.reset();
  detail::LinuxUIThreadDispatcher dispatcher;
  TestPlatform platform(dispatcher.Bind());
  bool first_replaced = false;
  bool second_completed = false;
  bool ticked = false;
  bool cancelled_completed = false;
  bool cancelled_ticked = false;
  std::weak_ptr<example::TimerService> service_lifetime;
  {
    Runtime runtime(LinuxPlatformModuleApp, platform, LinuxPlatformModuleOptions());
    runtime.SetWindowMetrics({{320.0F, 200.0F}});
    static_cast<void>(runtime.BuildRenderFrame());
    std::shared_ptr<example::TimerService> timer = linux_timer_service.lock();
    REQUIRE(timer != nullptr);
    service_lifetime = timer;

    REQUIRE_THROWS_AS(
        timer->Start(
            0ms,
            [](std::uint64_t) {},
            [](PlatformResult<std::uint64_t>) {}
        ),
        std::invalid_argument
    );

    static_cast<void>(timer->Start(
        200ms,
        [&](std::uint64_t) {},
        [&](PlatformResult<std::uint64_t> result) {
          const auto* error = std::get_if<PlatformError>(&result);
          first_replaced = error != nullptr && error->code == "example/timer-replaced";
        }
    ));
    static_cast<void>(timer->Start(
        10ms,
        [&](std::uint64_t) { ticked = true; },
        [&](PlatformResult<std::uint64_t> result) { second_completed = std::holds_alternative<std::uint64_t>(result); }
    ));
    REQUIRE(RunDispatcherUntil(dispatcher, [&] { return first_replaced && second_completed && ticked; }, 1s));

    static_cast<void>(timer->Stop([](PlatformResult<std::monostate>) {}));
    while (g_main_context_iteration(nullptr, FALSE) != FALSE) {
    }
    const PlatformRequestId cancelled = timer->Start(
        100ms,
        [&](std::uint64_t) { cancelled_ticked = true; },
        [&](PlatformResult<std::uint64_t>) { cancelled_completed = true; }
    );
    REQUIRE(timer->Cancel(cancelled));
    REQUIRE_FALSE(timer->Cancel(cancelled));
    RunDispatcherFor(dispatcher, 150ms);
    REQUIRE_FALSE(cancelled_completed);
    REQUIRE_FALSE(cancelled_ticked);

    static_cast<void>(timer->Start(
        100ms,
        [&](std::uint64_t) { cancelled_ticked = true; },
        [&](PlatformResult<std::uint64_t>) { cancelled_completed = true; }
    ));
    timer.reset();
  }

  REQUIRE(service_lifetime.expired());
  RunDispatcherFor(dispatcher, 150ms);
  REQUIRE_FALSE(cancelled_completed);
  REQUIRE_FALSE(cancelled_ticked);
}

} // namespace
} // namespace huxerui::test
