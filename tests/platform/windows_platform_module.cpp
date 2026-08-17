#include "runtime_test_support.h"
#include "timer.h"
#include "win32_ui_dispatcher.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
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

class DispatcherWindow final {
public:
  explicit DispatcherWindow(detail::Win32UIThreadDispatcher& dispatcher) : dispatcher_(&dispatcher) {
    instance_ = GetModuleHandleW(nullptr);
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = WindowProcedure;
    window_class.hInstance = instance_;
    window_class.lpszClassName = class_name;
    class_atom_ = RegisterClassW(&window_class);
    if (class_atom_ == 0) {
      throw std::runtime_error("HuxerUI tests could not register the Windows dispatcher class");
    }
    window_ = CreateWindowExW(0, class_name, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance_, dispatcher_);
    if (window_ == nullptr) {
      UnregisterClassW(class_name, instance_);
      class_atom_ = 0;
      throw std::runtime_error("HuxerUI tests could not create the Windows dispatcher window");
    }
  }

  ~DispatcherWindow() {
    dispatcher_->Shutdown();
    if (window_ != nullptr) {
      DestroyWindow(window_);
    }
    if (class_atom_ != 0) {
      UnregisterClassW(class_name, instance_);
    }
  }

  DispatcherWindow(const DispatcherWindow&) = delete;
  DispatcherWindow& operator=(const DispatcherWindow&) = delete;

  HWND Handle() const noexcept {
    return window_;
  }

private:
  static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    auto* dispatcher = reinterpret_cast<detail::Win32UIThreadDispatcher*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
      const auto* create = reinterpret_cast<const CREATESTRUCTW*>(l_param);
      dispatcher = static_cast<detail::Win32UIThreadDispatcher*>(create->lpCreateParams);
      SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dispatcher));
    }
    if (dispatcher != nullptr && message == detail::Win32UIThreadDispatcher::task_message) {
      dispatcher->RunPending();
      return 0;
    }
    return DefWindowProcW(window, message, w_param, l_param);
  }

  static constexpr wchar_t class_name[] = L"HuxerUI.Tests.Win32UIThreadDispatcher";

  detail::Win32UIThreadDispatcher* dispatcher_;
  HINSTANCE instance_ = nullptr;
  ATOM class_atom_ = 0;
  HWND window_ = nullptr;
};

template <class Predicate> bool RunDispatcherUntil(Predicate&& predicate, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!std::invoke(predicate) && std::chrono::steady_clock::now() < deadline) {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    const DWORD wait = MsgWaitForMultipleObjectsEx(
        0,
        nullptr,
        static_cast<DWORD>(std::max<std::int64_t>(1, remaining.count())),
        QS_ALLINPUT,
        MWMO_INPUTAVAILABLE
    );
    if (wait == WAIT_FAILED) {
      return false;
    }
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
  return std::invoke(predicate);
}

void RunDispatcherFor(std::chrono::milliseconds duration) {
  static_cast<void>(RunDispatcherUntil([] { return false; }, duration));
}

std::weak_ptr<example::TimerService> windows_timer_service;

View WindowsPlatformModuleApp() {
  windows_timer_service = example::UseTimer();
  return Text("platform module");
}

AppOptions WindowsPlatformModuleOptions() {
  AppOptions options{.show_debug_overlay = false};
  options.root_hooks.push_back(example::InstallTimer);
  return options;
}

TEST_CASE("WindowsUIThreadDispatcherQueuesBeforeAttachAndPreservesOrder") {
  detail::Win32UIThreadDispatcher dispatcher;
  UIThreadDispatcher post = dispatcher.Bind();
  const std::thread::id ui_thread = std::this_thread::get_id();
  std::vector<int> order;
  bool ran_inline = false;
  bool inside_post = true;

  std::thread worker([&] {
    post([&] {
      ran_inline = inside_post;
      if (std::this_thread::get_id() == ui_thread) {
        order.push_back(1);
      }
    });
    post([&] { order.push_back(2); });
  });
  worker.join();
  inside_post = false;

  REQUIRE(order.empty());
  DispatcherWindow window(dispatcher);
  dispatcher.Attach(window.Handle());
  REQUIRE(RunDispatcherUntil([&] { return order.size() == 2; }, 1s));
  REQUIRE_FALSE(ran_inline);
  REQUIRE(order == std::vector<int>{1, 2});

  dispatcher.Shutdown();
  post([&] { order.push_back(3); });
  RunDispatcherFor(10ms);
  REQUIRE(order == std::vector<int>{1, 2});
}

TEST_CASE("WindowsPlatformModuleUsesUIThreadWithoutInlineReentry") {
  windows_timer_service.reset();
  detail::Win32UIThreadDispatcher dispatcher;
  TestPlatform platform(dispatcher.Bind());
  Runtime runtime(WindowsPlatformModuleApp, platform, WindowsPlatformModuleOptions());
  DispatcherWindow window(dispatcher);
  dispatcher.Attach(window.Handle());
  runtime.SetWindowMetrics({{320.0F, 200.0F}});
  static_cast<void>(runtime.BuildRenderFrame());
  const std::shared_ptr<example::TimerService> timer = windows_timer_service.lock();
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
  REQUIRE(RunDispatcherUntil([&] { return completed && ticked; }, 1s));
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
  REQUIRE(RunDispatcherUntil([&] { return stopped; }, 1s));
  RunDispatcherFor(30ms);
  REQUIRE(delivery_order.size() == delivered_events);
  dispatcher.Shutdown();
}

TEST_CASE("WindowsPlatformModuleReplacesCancelsAndDisposesThreadPoolTimer") {
  windows_timer_service.reset();
  detail::Win32UIThreadDispatcher dispatcher;
  TestPlatform platform(dispatcher.Bind());
  DispatcherWindow window(dispatcher);
  dispatcher.Attach(window.Handle());
  bool first_replaced = false;
  bool second_completed = false;
  bool ticked = false;
  bool cancelled_completed = false;
  bool cancelled_ticked = false;
  std::weak_ptr<example::TimerService> service_lifetime;
  {
    Runtime runtime(WindowsPlatformModuleApp, platform, WindowsPlatformModuleOptions());
    runtime.SetWindowMetrics({{320.0F, 200.0F}});
    static_cast<void>(runtime.BuildRenderFrame());
    std::shared_ptr<example::TimerService> timer = windows_timer_service.lock();
    REQUIRE(timer != nullptr);
    service_lifetime = timer;

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
    REQUIRE(RunDispatcherUntil([&] { return first_replaced && second_completed && ticked; }, 1s));

    static_cast<void>(timer->Stop([](PlatformResult<std::monostate>) {}));
    dispatcher.RunPending();
    const PlatformRequestId request = timer->Start(
        50ms,
        [&](std::uint64_t) { cancelled_ticked = true; },
        [&](PlatformResult<std::uint64_t>) { cancelled_completed = true; }
    );
    REQUIRE(timer->Cancel(request));
    REQUIRE_FALSE(timer->Cancel(request));
    RunDispatcherFor(100ms);
    REQUIRE_FALSE(cancelled_completed);
    REQUIRE_FALSE(cancelled_ticked);

    static_cast<void>(timer->Start(
        50ms,
        [&](std::uint64_t) { cancelled_ticked = true; },
        [&](PlatformResult<std::uint64_t>) { cancelled_completed = true; }
    ));
    timer.reset();
  }

  REQUIRE(service_lifetime.expired());
  RunDispatcherFor(100ms);
  REQUIRE_FALSE(cancelled_completed);
  REQUIRE_FALSE(cancelled_ticked);
}

} // namespace
} // namespace huxerui::test
