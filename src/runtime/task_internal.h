#pragma once

#include <cstddef>
#include <coroutine>
#include <memory>

#include <huxerui/app.h>
#include <huxerui/task.h>

namespace huxerui {

namespace detail {

struct ExecutionContext;

[[nodiscard]] std::size_t WorkerConcurrency() noexcept;

/// Creates ordinary task ownership with explicit dispatch and callback provenance.
/// @param dispatcher Queued application-thread posting function, weakly bound to the original application.
/// @param execution Original application context or a window/composition context; retained for resumed callbacks.
/// @return Shared scope state whose closure invalidates pending resumes and externally posted callbacks.
std::shared_ptr<TaskScopeState>
MakeTaskScopeState(UiThreadDispatcher dispatcher, std::shared_ptr<ExecutionContext> execution);
/// Idempotently closes a scope and cancels its retained executions before its owner retires.
/// @param scope Scope to close on its owning application thread; a null scope is harmless.
void CloseTaskScope(const std::shared_ptr<TaskScopeState>& scope) noexcept;

template <class Promise>
  requires std::derived_from<Promise, TaskPromiseBase>
std::weak_ptr<TaskExecution> TaskExecutionFor(std::coroutine_handle<Promise> coroutine) noexcept {
  return coroutine.promise().Execution();
}

} // namespace detail

} // namespace huxerui
