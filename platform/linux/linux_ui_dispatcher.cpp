#include "linux_internal.h"

#include "linux_ui_dispatcher.h"

#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace huxerui::detail {

struct LinuxUIThreadDispatcher::State {
  State() : event_fd(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {
    if (event_fd < 0) {
      throw std::system_error(errno, std::generic_category(), "HuxerUI could not create the Linux UI event handle");
    }
  }

  ~State() {
    if (event_fd >= 0) {
      close(event_fd);
    }
  }

  void Post(std::function<void()> task) {
    std::lock_guard lock(mutex);
    tasks.push_back(std::move(task));

    constexpr std::uint64_t wake = 1;
    while (write(event_fd, &wake, sizeof(wake)) < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN) {
        return;
      }
      tasks.pop_back();
      throw std::system_error(errno, std::generic_category(), "HuxerUI could not wake the Linux UI thread");
    }
  }

  void DrainWakeCount() {
    std::uint64_t count = 0;
    while (read(event_fd, &count, sizeof(count)) < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN) {
        return;
      }
      throw std::system_error(errno, std::generic_category(), "HuxerUI could not read the Linux UI event handle");
    }
  }

  int event_fd = -1;
  std::mutex mutex;
  std::deque<std::function<void()>> tasks;
};

LinuxUIThreadDispatcher::LinuxUIThreadDispatcher() : state_(std::make_shared<State>()) {}

LinuxUIThreadDispatcher::~LinuxUIThreadDispatcher() = default;

UIThreadDispatcher LinuxUIThreadDispatcher::Bind() const {
  const std::shared_ptr<State> state = state_;
  return [state](std::function<void()> task) { state->Post(std::move(task)); };
}

int LinuxUIThreadDispatcher::FileDescriptor() const noexcept {
  return state_->event_fd;
}

void LinuxUIThreadDispatcher::RunPending() {
  state_->DrainWakeCount();

  std::deque<std::function<void()>> pending;
  {
    std::lock_guard lock(state_->mutex);
    pending.swap(state_->tasks);
  }
  for (const auto& task : pending) {
    try {
      task();
    } catch (...) {
    }
  }
}

} // namespace huxerui::detail
