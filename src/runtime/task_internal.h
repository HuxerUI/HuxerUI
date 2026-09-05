#pragma once

#include <cstddef>
#include <coroutine>
#include <memory>

#include <huxerui/platform_adapter.h>
#include <huxerui/task.h>

namespace huxerui {

namespace detail {

class TaskDelayScheduler;

std::shared_ptr<TaskDelayScheduler> MakeTaskDelayScheduler(PlatformAdapter& platform);
void AdvanceTaskDelays(const std::shared_ptr<TaskDelayScheduler>& scheduler, double timestamp);
[[nodiscard]] std::size_t WorkerConcurrency() noexcept;

std::shared_ptr<TaskScopeState>
MakeTaskScopeState(UIThreadDispatcher dispatcher, std::shared_ptr<TaskDelayScheduler> delay_scheduler);
void CloseTaskScope(const std::shared_ptr<TaskScopeState>& scope) noexcept;

template <class Promise>
  requires std::derived_from<Promise, TaskPromiseBase>
std::weak_ptr<TaskExecution> TaskExecutionFor(std::coroutine_handle<Promise> coroutine) noexcept {
  return coroutine.promise().Execution();
}

} // namespace detail

} // namespace huxerui
