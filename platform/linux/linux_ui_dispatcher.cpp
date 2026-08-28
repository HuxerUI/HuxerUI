#include "linux_internal.h"
#include "linux_ui_dispatcher.h"

#include <SDL3/SDL.h>

#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace huxerui::detail {

struct LinuxUIThreadDispatcher::State {
  State() : wake_event(SDL_RegisterEvents(1)) {}

  void Post(std::function<void()> task) {
    {
      std::lock_guard lock(mutex);
      if (!active) {
        return;
      }
      pending.push_back(std::move(task));
    }
    if (wake_event != 0) {
      SDL_Event event{};
      event.type = wake_event;
      static_cast<void>(SDL_PushEvent(&event));
    }
  }

  void Drain() {
    if (std::this_thread::get_id() != ui_thread) {
      return;
    }
    for (;;) {
      std::function<void()> task;
      {
        std::lock_guard lock(mutex);
        if (!active || pending.empty()) {
          return;
        }
        task = std::move(pending.front());
        pending.pop_front();
      }
      try {
        task();
      } catch (...) {
      }
    }
  }

  void Shutdown() noexcept {
    std::lock_guard lock(mutex);
    active = false;
    pending.clear();
  }

  std::mutex mutex;
  std::deque<std::function<void()>> pending;
  std::thread::id ui_thread = std::this_thread::get_id();
  Uint32 wake_event = 0;
  bool active = true;
};

LinuxUIThreadDispatcher::LinuxUIThreadDispatcher() : state_(std::make_shared<State>()) {}

LinuxUIThreadDispatcher::~LinuxUIThreadDispatcher() {
  Shutdown();
}

UIThreadDispatcher LinuxUIThreadDispatcher::Bind() const {
  const std::shared_ptr<State> state = state_;
  return [state](std::function<void()> task) { state->Post(std::move(task)); };
}

void LinuxUIThreadDispatcher::DrainPending() {
  state_->Drain();
}

void LinuxUIThreadDispatcher::Shutdown() noexcept {
  state_->Shutdown();
}

} // namespace huxerui::detail
