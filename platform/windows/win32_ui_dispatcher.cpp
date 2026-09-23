#include "win32_ui_dispatcher.h"

#include <atomic>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace huxerui::detail {

/// Shared dispatch gate and message-only HWND; no visible window owns this queue.
/// The application thread initializes and destroys the HWND, while the mutex protects foreign-thread posting.
struct Win32UiThreadDispatcher::State : std::enable_shared_from_this<State> {
  static constexpr UINT task_message = WM_APP + 4;

  /// Creates the message-only HWND on its owning thread before callbacks may be dispatched.
  void Initialize() {
    static std::atomic<unsigned long long> next_identity = 0;
    class_name = L"HuxerUI.ApplicationDispatcher." + std::to_wstring(next_identity.fetch_add(1));
    instance = GetModuleHandleW(nullptr);
    owner_thread = GetCurrentThreadId();
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = WindowProcedure;
    window_class.hInstance = instance;
    window_class.lpszClassName = class_name.c_str();
    if (RegisterClassW(&window_class) == 0) {
      throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                              "HuxerUI could not register its application dispatcher");
    }
    platform_window = CreateWindowExW(0, class_name.c_str(), L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, this);
    if (platform_window == nullptr) {
      const DWORD error = GetLastError();
      UnregisterClassW(class_name.c_str(), instance);
      throw std::system_error(static_cast<int>(error), std::system_category(),
                              "HuxerUI could not create its application dispatcher");
    }
  }

  /// Enqueues a callback from any thread and coalesces native wake messages.
  /// @param task Nonempty callback retained until delivery or closure; empty input throws std::invalid_argument.
  /// Native enqueue failure withdraws only this submission and propagates std::system_error.
  void Post(std::function<void()> task) {
    if (!task) {
      throw std::invalid_argument("HuxerUI application-thread task must not be empty");
    }
    DWORD error = ERROR_SUCCESS;
    {
      std::lock_guard lock(mutex);
      if (closed) {
        return;
      }
      tasks.push_back(std::move(task));
      if (!started || wake_posted) {
        return;
      }
      if (PostMessageW(platform_window, task_message, 0, 0)) {
        wake_posted = true;
        return;
      }
      error = GetLastError();
      task = std::move(tasks.back());
      tasks.pop_back();
    }
    throw std::system_error(static_cast<int>(error), std::system_category(),
                            "HuxerUI could not wake the Windows application thread");
  }

  /// Enables delivery after application initialization; requires the live owning thread and is idempotent.
  void Start() {
    std::lock_guard lock(mutex);
    if (closed || GetCurrentThreadId() != owner_thread) {
      throw std::logic_error("HuxerUI application dispatcher must start on its live owning thread");
    }
    if (started) {
      return;
    }
    if (!tasks.empty()) {
      if (!PostMessageW(platform_window, task_message, 0, 0)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "HuxerUI could not start its application dispatcher");
      }
      wake_posted = true;
    }
    started = true;
  }

  /// Runs one queued batch on the HWND thread without holding the queue lock during callbacks.
  void RunPending() {
    std::deque<std::function<void()>> pending;
    {
      std::lock_guard lock(mutex);
      if (closed) {
        return;
      }
      pending.swap(tasks);
    }
    for (auto& task : pending) {
      {
        std::lock_guard lock(mutex);
        if (closed) {
          return;
        }
      }
      try {
        task();
      } catch (...) {
      }
    }
    std::lock_guard lock(mutex);
    wake_posted = false;
    if (!closed && !tasks.empty()) {
      // Keep reentrant submissions behind this batch even when a callback pumps native messages.
      if (!PostMessageW(platform_window, task_message, 0, 0)) {
        std::terminate();
      }
      wake_posted = true;
    }
  }

  void Shutdown() noexcept {
    HWND retired_platform_window = nullptr;
    std::deque<std::function<void()>> discarded;
    {
      std::lock_guard lock(mutex);
      if (closed) {
        return;
      }
      if (GetCurrentThreadId() != owner_thread) {
        std::terminate();
      }
      closed = true;
      wake_posted = false;
      retired_platform_window = std::exchange(platform_window, nullptr);
      discarded.swap(tasks);
    }
    DestroyWindow(retired_platform_window);
    UnregisterClassW(class_name.c_str(), instance);
  }

  static LRESULT CALLBACK WindowProcedure(HWND platform_window, UINT message, WPARAM w_param, LPARAM l_param) {
    auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(platform_window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
      const auto* create = reinterpret_cast<const CREATESTRUCTW*>(l_param);
      state = static_cast<State*>(create->lpCreateParams);
      SetWindowLongPtrW(platform_window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state != nullptr && message == task_message) {
      const std::shared_ptr<State> retained = state->shared_from_this();
      retained->RunPending();
      return 0;
    }
    return DefWindowProcW(platform_window, message, w_param, l_param);
  }

  HWND platform_window = nullptr;
  HINSTANCE instance = nullptr;
  DWORD owner_thread = 0;
  std::wstring class_name;
  std::mutex mutex;
  std::deque<std::function<void()>> tasks;
  bool wake_posted = false;
  bool started = false;
  bool closed = false;
};

Win32UiThreadDispatcher::Win32UiThreadDispatcher() : state_(std::make_shared<State>()) {
  state_->Initialize();
}

Win32UiThreadDispatcher::~Win32UiThreadDispatcher() {
  Shutdown();
}

UiThreadDispatcher Win32UiThreadDispatcher::Bind() const {
  const std::shared_ptr<State> state = state_;
  return [state](std::function<void()> task) { state->Post(std::move(task)); };
}

void Win32UiThreadDispatcher::Start() {
  state_->Start();
}

void Win32UiThreadDispatcher::Shutdown() noexcept {
  state_->Shutdown();
}

} // namespace huxerui::detail
