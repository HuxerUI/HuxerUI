#pragma once

#include <coroutine>
#include <memory>

#include <huxerui/platform_module.h>
#include <huxerui/task.h>

namespace huxerui {

class PlatformAdapter;

namespace detail {

class TaskDelayScheduler;

std::shared_ptr<TaskDelayScheduler> MakeTaskDelayScheduler(PlatformAdapter& platform);
void AdvanceTaskDelays(const std::shared_ptr<TaskDelayScheduler>& scheduler, double timestamp);

std::shared_ptr<TaskScopeState>
MakeTaskScopeState(UIThreadDispatcher dispatcher, std::shared_ptr<TaskDelayScheduler> delay_scheduler);
void CloseTaskScope(const std::shared_ptr<TaskScopeState>& scope) noexcept;
void ResumeTask(const std::weak_ptr<TaskExecution>& execution, std::coroutine_handle<> coroutine) noexcept;

template <class Promise>
  requires std::derived_from<Promise, TaskPromiseBase>
std::weak_ptr<TaskExecution> TaskExecutionFor(std::coroutine_handle<Promise> coroutine) noexcept {
  return coroutine.promise().Execution();
}

} // namespace detail

} // namespace huxerui
