#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <utility>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif

#include "macros_internal.h"

namespace huxerui::test {

template <class Predicate>
bool WaitForTestCondition(std::condition_variable& condition, std::unique_lock<std::mutex>& lock,
                          std::chrono::steady_clock::time_point deadline, Predicate&& ready) {
#if defined(__EMSCRIPTEN__)
  HUXERUI_UNUSED(condition);
  while (!ready()) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    // Web file operations run on this event loop and must be able to acquire the queue lock.
    lock.unlock();
    emscripten_sleep(1);
    lock.lock();
  }
  return true;
#else
  return condition.wait_until(lock, deadline, std::forward<Predicate>(ready));
#endif
}

} // namespace huxerui::test
