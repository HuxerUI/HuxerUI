#include "win32_ui_dispatcher.h"

#include <deque>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace huxerui::detail {

struct Win32UIThreadDispatcher::State {
  void Post(std::function<void()> task) {
    std::lock_guard lock(mutex);
    if (closed) {
      return;
    }
    tasks.push_back(std::move(task));
    if (window == nullptr || wake_posted) {
      // Root hooks can submit module work before the application HWND exists; Attach wakes that retained batch.
      return;
    }
    if (!PostMessageW(window, Win32UIThreadDispatcher::task_message, 0, 0)) {
      tasks.pop_back();
      throw std::system_error(
          static_cast<int>(GetLastError()),
          std::system_category(),
          "HuxerUI could not wake the Windows UI thread"
      );
    }
    wake_posted = true;
  }

  void Attach(HWND attached_window) {
    if (attached_window == nullptr) {
      throw std::invalid_argument("HuxerUI Windows UI dispatcher requires a window");
    }
    std::lock_guard lock(mutex);
    if (closed) {
      throw std::logic_error("HuxerUI Windows UI dispatcher is closed");
    }
    if (window != nullptr) {
      throw std::logic_error("HuxerUI Windows UI dispatcher is already attached");
    }
    window = attached_window;
    if (tasks.empty()) {
      return;
    }
    if (!PostMessageW(window, Win32UIThreadDispatcher::task_message, 0, 0)) {
      window = nullptr;
      throw std::system_error(
          static_cast<int>(GetLastError()),
          std::system_category(),
          "HuxerUI could not attach the Windows UI dispatcher"
      );
    }
    wake_posted = true;
  }

  void RunPending() {
    std::deque<std::function<void()>> pending;
    {
      std::lock_guard lock(mutex);
      if (closed) {
        return;
      }
      wake_posted = false;
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
  }

  void Shutdown() noexcept {
    std::lock_guard lock(mutex);
    closed = true;
    wake_posted = false;
    window = nullptr;
    tasks.clear();
  }

  HWND window = nullptr;
  std::mutex mutex;
  std::deque<std::function<void()>> tasks;
  bool wake_posted = false;
  bool closed = false;
};

Win32UIThreadDispatcher::Win32UIThreadDispatcher() : state_(std::make_shared<State>()) {}

Win32UIThreadDispatcher::~Win32UIThreadDispatcher() {
  Shutdown();
}

UIThreadDispatcher Win32UIThreadDispatcher::Bind() const {
  const std::shared_ptr<State> state = state_;
  return [state](std::function<void()> task) { state->Post(std::move(task)); };
}

void Win32UIThreadDispatcher::Attach(HWND window) {
  state_->Attach(window);
}

void Win32UIThreadDispatcher::RunPending() {
  state_->RunPending();
}

void Win32UIThreadDispatcher::Shutdown() noexcept {
  state_->Shutdown();
}

} // namespace huxerui::detail
