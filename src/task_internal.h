#pragma once

#include <coroutine>
#include <memory>

#include <huxerui/platform_module.h>
#include <huxerui/task.h>

namespace huxerui::detail {

std::shared_ptr<TaskScopeState> MakeTaskScopeState(UIThreadDispatcher dispatcher);
void CloseTaskScope(const std::shared_ptr<TaskScopeState>& scope) noexcept;
void ResumeTask(const std::weak_ptr<TaskExecution>& execution, std::coroutine_handle<> coroutine) noexcept;

template <class Promise>
  requires std::derived_from<Promise, TaskPromiseBase>
std::weak_ptr<TaskExecution> TaskExecutionFor(std::coroutine_handle<Promise> coroutine) noexcept {
  return coroutine.promise().Execution();
}

} // namespace huxerui::detail
