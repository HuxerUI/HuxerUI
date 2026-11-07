#pragma once

#include <cstddef>
#include <memory>
#include <stop_token>

#include <huxerui/app.h>
#include <huxerui/task.h>

namespace huxerui {

namespace detail {

struct ExecutionContext;
class SystemTaskGrant;

/// Cooperative stop state for one admitted system execution, shared with its coroutine chain.
struct SystemTaskStopState {
  std::stop_source source;
  std::weak_ptr<TaskExecution> execution;
  /// Observes platform revocation before its queued Runtime event reaches the execution.
  std::weak_ptr<SystemTaskGrant> grant;
  bool cleaning_up = false;
};

/// Queues the current participating wait so it can observe a requested stop.
void WakeStoppedTask(const SystemTaskStopState& stop) noexcept;
/// Checks whether this exact system execution is running on the current thread.
bool IsCurrentSystemTask(const SystemTaskStopState& stop) noexcept;
/// Indicates that the current system task is executing its bounded cleanup.
bool IsSystemTaskCleaningUp() noexcept;
/// Launches admitted work under the application context with cooperative stop enabled.
TaskHandle LaunchSystemTask(const std::shared_ptr<TaskScopeState>& scope, Task<void> task,
                            std::shared_ptr<SystemTaskStopState> stop);

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

} // namespace detail

} // namespace huxerui
