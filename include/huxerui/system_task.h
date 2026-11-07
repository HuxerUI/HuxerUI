#pragma once

#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>

#include <huxerui/data.h>
#include <huxerui/resource.h>
#include <huxerui/task.h>

namespace huxerui {

namespace detail {
class SystemTasks;
struct SystemTaskDeclaration;
struct SystemTaskAttempt;
} // namespace detail

/// Expected admission, platform operation, or handler failures.
enum class SystemTaskErrorCode {
  /// The mechanism or a requested scheduling option is unsupported by this host.
  Unsupported,
  /// The original request or execution is unavailable, or platform reconciliation is incomplete.
  Unavailable,
  /// Platform permission, entitlement, or admission rules prohibit the operation.
  NotPermitted,
  /// The operation requires foreground eligibility that the application currently lacks.
  NotForeground,
  /// Required platform declarations or task presentation configuration are missing or invalid.
  Configuration,
  /// An active request conflicts with the requested submission, including a request still stopping.
  Busy,
  /// The platform host declined admission or could not honor the requested submission policy.
  Rejected,
  /// A platform operation failed; message contains the available platform diagnostic.
  Platform,
  /// An exception escaped the application handler.
  Handler,
};

/// A system task error and its diagnostic.
struct SystemTaskError {
  /// Category suitable for application decisions without parsing the diagnostic.
  SystemTaskErrorCode code;
  /// Human-readable diagnostic; its text is not a stable error identifier.
  std::string message;

  bool operator==(const SystemTaskError&) const = default;
};

/// Reports the result of a system task operation independently of the task's execution state.
/// @tparam T Operation payload, or void for an operation without a value.
/// Expected platform failures use this result. Invalid arguments and invalid calling contexts still throw exceptions.
/// A successful observation can contain a TaskState whose execution status is Failed or Interrupted.
template <class T> using SystemTaskResult = Result<T, SystemTaskError>;

/// Platform submission was accepted or the same work was already active.
/// Accepted establishes a new request; execution may remain Pending until the platform host grants it.
/// NoChange retains the existing request, handler execution, and presentation.
enum class SubmissionStatus { Accepted, NoChange };

/// A stop was requested or there was no additional work to stop.
/// Requested means withdrawal succeeded or cooperative stopping began; running work may still be Stopping.
/// NoChange means the target is absent, inactive, or already stopping. Use WaitAsync() to observe its outcome.
enum class StopRequestStatus { Requested, NoChange };

/// First known cause of cooperative stopping.
/// Requested comes from RequestStop(); System comes from the platform host; Shutdown comes from application shutdown.
/// Deadline means an execution or cleanup deadline elapsed before another cause was recorded.
enum class TaskStopReason { Requested, System, Shutdown, Deadline };

/// Lifecycle of the current platform request and its execution.
/// Unknown means platform state is indeterminate; Idle means there is no known request.
/// Pending awaits admission or a background retry; Running has an admitted handler; Stopping awaits cooperative exit.
/// Completed finishes background or continued work; Stopped covers withdrawal, cooperative exit, or an ended session.
/// Failed records a known submission or execution failure; Interrupted means execution rights or the stop budget ended.
enum class TaskStatus { Unknown, Idle, Pending, Running, Stopping, Completed, Stopped, Failed, Interrupted };

/// Presentation for user-started tasks. Strings are resolved with application resources.
/// Start() and UpdatePresentation() resolve both strings using the application's current resource configuration.
/// Resolved text must be valid UTF-8 without embedded nulls; invalid text throws std::invalid_argument.
struct TaskPresentation {
  /// Platform task title; its resolved value must be nonempty.
  StringVariant title;
  /// Additional platform text; its resolved value may be empty.
  StringVariant subtitle;

  bool operator==(const TaskPresentation&) const = default;
};

/// Completed units and an optional positive total of the work.
/// ReportProgress() validates each report within its execution. Units describe actual completed work.
struct TaskProgress {
  /// Completed units, which may start at zero and must not decrease within the execution.
  std::uint64_t completed = 0;
  /// Unknown when nullopt. A known total must be positive, at least completed, and may only increase once reported.
  std::optional<std::uint64_t> total;

  bool operator==(const TaskProgress&) const = default;
};

/// Capabilities of the configured execution mechanism, independent of current platform admission.
/// Support does not guarantee permissions, foreground eligibility, acceptance, or support for every scheduling option.
struct SystemTaskCapabilities {
  /// Whether this host implements the declaration's task kind.
  bool supported = false;
  /// Whether the platform can deliver the work without an existing application process.
  /// Platform setup and scheduling policy still apply; application State and service objects remain process-local.
  bool can_launch_process = false;
};

/// Signals cooperative system task stopping from a participating framework await or an explicit stop check.
/// A handler may catch it and await Context::Cleanup() after the catch block, within the remaining execution budget.
class TaskStopped final : public std::exception {
public:
  /// Returns the cooperative stop diagnostic.
  const char* what() const noexcept override;
};

/// An owned snapshot of a system task request, including identity for conditional removal.
/// Copies retain their values when live task state changes and can be inspected without a live Runtime.
/// References returned by accessors borrow from this snapshot and remain valid until it is assigned or destroyed.
/// Snapshots contain observation metadata, not business results or durable task history.
class TaskState final {
public:
  /// Reads the current lifecycle status. Unknown means the platform request could not be established.
  [[nodiscard]] TaskStatus Status() const noexcept { return status_; }
  /// Returns true for Pending, Running, and Stopping.
  [[nodiscard]] bool IsActive() const noexcept;
  /// Returns true for Completed, Stopped, Failed, and Interrupted.
  [[nodiscard]] bool IsTerminal() const noexcept;
  /// Borrows the last accepted progress from this snapshot, if any.
  [[nodiscard]] const std::optional<TaskProgress>& Progress() const noexcept { return progress_; }
  /// Borrows the resolved presentation from this snapshot, if any.
  [[nodiscard]] const std::optional<TaskPresentation>& Presentation() const noexcept { return presentation_; }
  /// Reads the first known stop cause.
  [[nodiscard]] std::optional<TaskStopReason> StopReason() const noexcept { return stop_reason_; }
  /// Borrows the latest recorded operation or execution error, if any; an update error need not make status Failed.
  [[nodiscard]] const std::optional<SystemTaskError>& Error() const noexcept { return error_; }

  bool operator==(const TaskState&) const = default;

private:
  std::uint64_t record_id_ = 0;
  TaskStatus status_ = TaskStatus::Idle;
  std::optional<TaskProgress> progress_;
  std::optional<TaskPresentation> presentation_;
  std::optional<TaskStopReason> stop_reason_;
  std::optional<SystemTaskError> error_;

  friend class detail::SystemTasks;
};

/// One declaration of finite work admitted by a system background scheduler.
/// Install this declaration or a copy in AppOptions::system_tasks before its Runtime starts. The handler runs on the
/// application thread after platform admission and may update application State without a UiWindow.
/// Except Identifier(), control and query methods require the current Runtime's application thread and context.
/// Calling them for an uninstalled declaration or an invalid Runtime/context throws std::logic_error.
///
/// With <huxerui/huxerui.h>, a declaration can be created before the application and installed through AppOptions:
/// @code
/// Task<BackgroundTask::Result> Refresh(BackgroundTask::Context context) {
///   context.ThrowIfStopRequested();
///   co_return BackgroundTask::Result::Completed;
/// }
/// const BackgroundTask refresh_task{"com.example.refresh", Refresh};
/// const AppOptions options{.system_tasks = {refresh_task}};
/// @endcode
class BackgroundTask final {
public:
  /// Completes this request or asks the system scheduler for another opportunity.
  /// Completed finishes the request. Retry preserves its scheduling requirements and returns it to Pending when
  /// rescheduling succeeds; it promises neither a delivery time nor eventual execution.
  enum class Result { Completed, Retry };

  /// Lower bound and requirements for one scheduling request.
  /// Requested requirements are honored or rejected as Unsupported; the framework does not silently weaken them.
  struct Schedule {
    /// Earliest eligible wall-clock time, or nullopt for no lower bound; actual delivery may be later.
    std::optional<std::chrono::system_clock::time_point> earliest_start;
    /// Whether system scheduling must require network availability.
    bool requires_network = false;
    /// Whether system scheduling must require an external power source.
    bool requires_external_power = false;

    bool operator==(const Schedule&) const = default;
  };

  /// Controls the current background attempt from its original application thread.
  /// Copies keep the original attempt identity without extending Runtime or execution lifetime.
  /// StopToken() and ThrowIfStopRequested() may be used on worker threads. Other methods require the original
  /// application's thread and context; an invalid live context throws std::logic_error.
  class Context final {
  public:
    /// Retains the attempt's cooperative stop signal. The copied token is safe to check on a worker thread.
    /// The token remains usable after this Context or its attempt ends.
    [[nodiscard]] std::stop_token StopToken() const noexcept { return stop_token_; }
    /// Reads the first stop cause on the application thread, if this attempt has one.
    /// @return The recorded cause while active, or nullopt before stopping or after the attempt ends.
    [[nodiscard]] std::optional<TaskStopReason> StopReason() const;
    /// Reads the earliest known platform or cleanup deadline on the application thread.
    /// @return An absolute steady-clock deadline, or nullopt when none is known or the attempt has ended.
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> Deadline() const;
    /// Throws TaskStopped if this attempt's stop token has been signaled.
    /// @throws TaskStopped If cooperative stop was requested, including during cleanup when checked explicitly.
    void ThrowIfStopRequested() const;
    /// Awaits one cleanup task from the handler's own Task chain within its remaining execution budget.
    /// Normal completion may also await cleanup. It preserves the stop token and shields framework stop exceptions.
    /// The earliest platform or AppOptions::system_task_stop_timeout deadline applies; expiration may cancel cleanup.
    /// @param task Lazy cleanup work, which may await multiple operations and retains the original stop token.
    /// @return A lazy Task that propagates exceptions from the cleanup work when awaited.
    /// @throws std::logic_error If awaited outside the active attempt's Task chain or cleanup was already used.
    Task<void> Cleanup(Task<void> task) const;

    /// Publishes the attempt's nondecreasing completed units on the application thread.
    /// @param progress Completed units and, when known, a positive total at least that large.
    /// @return Success when accepted, Unavailable after the attempt ends, or a platform update error.
    /// A returned error is also recorded in TaskState; application code decides whether to end its handler.
    /// @throws std::invalid_argument If completed or total decreases, total is invalid, or a known total is removed.
    SystemTaskResult<void> ReportProgress(TaskProgress progress) const;

  private:
    explicit Context(const std::shared_ptr<detail::SystemTaskAttempt>& attempt);
    std::weak_ptr<detail::SystemTaskAttempt> attempt_;
    std::stop_token stop_token_;
    friend class detail::SystemTasks;
  };

  /// Runs finite work on the application thread and selects completion or system retry.
  /// The returned Task and its awaited children belong to this attempt; durable inputs and checkpoints are app-owned.
  using Handler = std::function<Task<Result>(Context)>;

  BackgroundTask(std::string identifier, Handler handler);

  /// Borrows the stable identifier used in AppOptions and platform configuration.
  /// The returned view borrows declaration-owned storage shared by its copies; no Runtime is needed.
  [[nodiscard]] std::string_view Identifier() const noexcept;
  /// Queries platform support and cold activation on the application thread.
  [[nodiscard]] SystemTaskCapabilities GetCapabilities() const;
  /// Creates a lazy submission bound to the current Runtime.
  /// Platform submission starts when consumed by a running Task. Cancellation before that skips submission;
  /// cancellation after submission starts does not withdraw accepted work. Use RequestStop() for explicit withdrawal.
  /// @param schedule Requested lower bound, connectivity, and external power constraints.
  /// @return A Task yielding Accepted, NoChange for an identical active schedule, or an operational error.
  /// A different active schedule or a stopping request returns Busy; indeterminate platform state returns Unavailable.
  /// @throws std::logic_error For an invalid calling context, or if consumed during installation, composition, cleanup,
  /// shutdown, or in a different or retired Runtime.
  Task<SystemTaskResult<SubmissionStatus>> ScheduleAsync(Schedule schedule) const;
  /// Creates a lazy submission with no additional scheduling constraints.
  /// Has the same Runtime, result, and cancellation rules as ScheduleAsync(Schedule).
  Task<SystemTaskResult<SubmissionStatus>> ScheduleAsync() const;
  /// Withdraws pending work or signals cooperative stop for the current attempt.
  /// A running handler may remain Stopping while it performs bounded cleanup. Failed withdrawal keeps its active state.
  /// @return Requested, NoChange when no further stop is needed, or an operational error.
  /// @throws std::logic_error For an invalid calling context or a call during composition.
  SystemTaskResult<StopRequestStatus> RequestStop() const;
  /// Copies the current request state and subscribes the active composition, if any.
  /// @return An owned snapshot, including Unknown during platform reconciliation or Idle when no request is known.
  /// Outside composition this is an immediate read without a retained subscription.
  [[nodiscard]] TaskState GetState() const;
  /// Selects the current request now and observes its terminal snapshot when awaited, including system retries.
  /// Later submissions never replace this selection. Canceling the observer leaves the platform request running.
  /// @return A Task yielding the selected snapshot, Idle when there was no request, or Unsupported/Unavailable when
  /// observation cannot be established. A successful result can contain a Failed or Interrupted execution.
  /// @throws std::logic_error For an invalid calling context, consumption in a different or retired Runtime, or an
  /// attempt waiting for its own completion.
  Task<SystemTaskResult<TaskState>> WaitAsync() const;

private:
  std::shared_ptr<const detail::SystemTaskDeclaration> declaration_;
  friend class detail::SystemTasks;
};

/// One declaration of user-started finite work, with independently identified instances.
/// Install the declaration in AppOptions::system_tasks. Its handler runs on the application thread after admission.
/// Except Identifier(), control and query methods require the current Runtime's application thread and context;
/// uninstalled declarations and invalid Runtime/context calls throw std::logic_error.
/// Each instance retains its terminal snapshot until it is started again or removed by Forget().
///
/// From a foreground user action, given an installed export_task, TaskScope tasks, and State<TaskStatus> status:
/// @code
/// auto submitted = export_task.Start("report", {"Export report", "Preparing"});
/// if (submitted.Succeeded()) {
///   auto selected = export_task.WaitAsync("report");
///   tasks.Launch([selected = std::move(selected), status]() mutable -> Task<void> {
///     auto result = co_await std::move(selected);
///     if (result.Succeeded()) status = result.Value().Status();
///   });
/// }
/// @endcode
class ContinuedTask final {
public:
  /// Controls whether platform admission may defer execution.
  /// RequireImmediate rejects admission that needs queueing. AllowQueue permits a system queue when available;
  /// neither policy creates durable framework job storage or guarantees when the handler starts.
  enum class SubmissionPolicy { RequireImmediate, AllowQueue };

  /// Controls the current continued attempt from its original application thread.
  /// Copies keep the original attempt identity without extending its lifetime. StopToken(), ThrowIfStopRequested(),
  /// and InstanceId() use retained values and may be called on worker threads. Other methods require the original
  /// application's thread and context; an invalid live context throws std::logic_error.
  class Context final {
  public:
    /// Retains the attempt's cooperative stop signal. The copied token is safe to check on a worker thread.
    /// The token remains usable after this Context or its attempt ends.
    [[nodiscard]] std::stop_token StopToken() const noexcept { return stop_token_; }
    /// Reads the first stop cause on the application thread, if this attempt has one.
    /// @return The recorded cause while active, or nullopt before stopping or after the attempt ends.
    [[nodiscard]] std::optional<TaskStopReason> StopReason() const;
    /// Reads the earliest known platform or cleanup deadline on the application thread.
    /// @return An absolute steady-clock deadline, or nullopt when none is known or the attempt has ended.
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> Deadline() const;
    /// Throws TaskStopped if this attempt's stop token has been signaled.
    /// @throws TaskStopped If cooperative stop was requested, including during cleanup when checked explicitly.
    void ThrowIfStopRequested() const;
    /// Awaits one cleanup task from the handler's own task chain within its remaining execution budget.
    /// Shares the single-use, stop-shielding, and deadline rules of BackgroundTask::Context::Cleanup().
    /// @param task Lazy cleanup work, which may await multiple operations and retains the original stop token.
    /// @return A lazy Task that propagates exceptions from the cleanup work when awaited.
    /// @throws std::logic_error If awaited outside the active attempt's Task chain or cleanup was already used.
    Task<void> Cleanup(Task<void> task) const;

    /// Copies the business instance identifier supplied to Start.
    /// This value remains available after the attempt ends and never selects a replacement execution.
    [[nodiscard]] std::string InstanceId() const { return instance_id_; }
    /// Publishes the attempt's nondecreasing completed units on the application thread.
    /// @param progress Completed units and, when known, a positive total at least that large.
    /// @return Success when accepted, Unavailable after the attempt ends, or a platform update error.
    /// A returned error is also recorded in TaskState; application code decides whether to end its handler.
    /// @throws std::invalid_argument If completed or total decreases, total is invalid, or a known total is removed.
    SystemTaskResult<void> ReportProgress(TaskProgress progress) const;
    /// Updates this attempt's platform presentation on the application thread.
    /// @param presentation Application-localized title and subtitle. The resolved title must be nonempty.
    /// @return Success when updated, Unavailable after the attempt ends, or a platform update error.
    /// A returned error is also recorded in TaskState; application code decides whether to end its handler.
    /// @throws std::invalid_argument If resolved text is invalid UTF-8, contains nulls, or has an empty title.
    SystemTaskResult<void> UpdatePresentation(TaskPresentation presentation) const;

  private:
    explicit Context(const std::shared_ptr<detail::SystemTaskAttempt>& attempt);
    std::weak_ptr<detail::SystemTaskAttempt> attempt_;
    std::stop_token stop_token_;
    std::string instance_id_;
    friend class detail::SystemTasks;
  };

  /// Runs one instance to completion on the application thread.
  /// Normal return completes an instance with valid execution rights; handler failures appear in TaskState.
  using Handler = std::function<Task<void>(Context)>;

  ContinuedTask(std::string identifier, Handler handler);

  /// Borrows the stable identifier used in AppOptions and platform configuration.
  /// The returned view borrows declaration-owned storage shared by its copies; no Runtime is needed.
  [[nodiscard]] std::string_view Identifier() const noexcept;
  /// Queries platform continued processing support on the application thread.
  [[nodiscard]] SystemTaskCapabilities GetCapabilities() const;
  /// Submits finite work directly from a foreground user action.
  /// Platform submission occurs before return; the handler is dispatched only after platform admission.
  /// @param instance_id Nonempty UTF-8 business identifier, unique among active instances of this declaration.
  /// @param presentation Initial application-localized title and subtitle.
  /// @param policy Whether system queueing is acceptable when immediate execution is unavailable.
  /// @return Accepted for a new request, NoChange for the same active instance, or an operational error.
  /// A duplicate retains its original presentation; an instance still stopping returns Busy.
  /// @throws std::invalid_argument If instance_id, resolved presentation text, or policy is invalid.
  /// @throws std::logic_error For an invalid context, or during installation, composition, cleanup, or shutdown.
  SystemTaskResult<SubmissionStatus> Start(
      std::string instance_id, TaskPresentation presentation,
      SubmissionPolicy policy = SubmissionPolicy::RequireImmediate) const;
  /// Withdraws or stops the current request for the given business instance identifier.
  /// @param instance_id Identifier originally passed to Start.
  /// @return Requested, NoChange for an absent/inactive/already-stopping instance, or an operational error.
  /// Requested does not mean a running handler has finished; WaitAsync() observes its eventual snapshot.
  /// @throws std::invalid_argument If instance_id is empty, invalid UTF-8, or contains a null character.
  /// @throws std::logic_error For an invalid calling context or a call during composition.
  SystemTaskResult<StopRequestStatus> RequestStop(std::string_view instance_id) const;
  /// Copies the current instance state, if present, and observes its presence during composition.
  /// @param instance_id Identifier originally passed to Start.
  /// @return An owned snapshot or nullopt when absent; composition also observes later appearance and removal.
  /// Outside composition this is an immediate read without a retained subscription.
  /// @throws std::invalid_argument If instance_id is empty, invalid UTF-8, or contains a null character.
  [[nodiscard]] std::optional<TaskState> GetState(std::string_view instance_id) const;
  /// Selects the current instance request now and observes its terminal state when awaited.
  /// @param instance_id Identifier originally passed to Start.
  /// @return A lazy observer yielding the selected request's terminal snapshot, or Unsupported/Unavailable.
  /// Selection occurs now, even if a later Start() replaces the instance before this Task runs. Cancellation only
  /// detaches the observer. As with BackgroundTask::WaitAsync(), successful observation can contain a Failed execution.
  /// @throws std::invalid_argument If instance_id is empty, invalid UTF-8, or contains a null character.
  /// @throws std::logic_error For an invalid context or Runtime, or an attempt waiting for its own completion.
  Task<SystemTaskResult<TaskState>> WaitAsync(std::string_view instance_id) const;
  /// Removes a terminal record only if it still matches the observed request identity.
  /// @param instance_id Identifier originally passed to Start.
  /// @param observed_state Terminal snapshot previously returned by GetState or WaitAsync.
  /// @return True when the exact terminal record was removed; false for a missing, active, or different request.
  /// Removal releases only the snapshot record; business data and platform requests are unaffected.
  /// @throws std::invalid_argument If instance_id is empty, invalid UTF-8, or contains a null character.
  /// @throws std::logic_error For an invalid calling context or a call during composition.
  bool Forget(std::string_view instance_id, const TaskState& observed_state) const;

private:
  std::shared_ptr<const detail::SystemTaskDeclaration> declaration_;
  friend class detail::SystemTasks;
};

/// One declaration of a user-started session that runs until its work or allowance ends.
/// Install the declaration in AppOptions::system_tasks. One request is retained at a time; its handler runs on the
/// application thread after platform admission. Except Identifier(), control and query methods require that Runtime's
/// application thread and context; uninstalled declarations and invalid Runtime/context calls throw std::logic_error.
/// Application shutdown bounds stopping and cleanup even while a session is active.
///
/// For an application service Session with RunAsync(stop_token) and CloseAsync(), release can follow either outcome:
/// @code
/// Task<void> RunSession(OngoingTask::Context context) {
///   auto session = UseService<Session>();
///   std::exception_ptr failure;
///   try {
///     co_await session->RunAsync(context.StopToken());
///   } catch (...) {
///     failure = std::current_exception();
///   }
///   co_await context.Cleanup(session->CloseAsync());
///   if (failure) std::rethrow_exception(failure);
/// }
/// @endcode
class OngoingTask final {
public:
  /// Controls the current ongoing attempt from its original application thread.
  /// Copies keep the original attempt identity without extending Runtime or execution lifetime.
  /// StopToken() and ThrowIfStopRequested() may be used on worker threads. Other methods require the original
  /// application's thread and context; an invalid live context throws std::logic_error.
  class Context final {
  public:
    /// Retains the attempt's cooperative stop signal. The copied token is safe to check on a worker thread.
    /// The token remains usable after this Context or its attempt ends.
    [[nodiscard]] std::stop_token StopToken() const noexcept { return stop_token_; }
    /// Reads the first stop cause on the application thread, if this attempt has one.
    /// @return The recorded cause while active, or nullopt before stopping or after the attempt ends.
    [[nodiscard]] std::optional<TaskStopReason> StopReason() const;
    /// Reads the earliest known platform or cleanup deadline on the application thread.
    /// @return An absolute steady-clock deadline, or nullopt when none is known or the attempt has ended.
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> Deadline() const;
    /// Throws TaskStopped if this attempt's stop token has been signaled.
    /// @throws TaskStopped If cooperative stop was requested, including during cleanup when checked explicitly.
    void ThrowIfStopRequested() const;
    /// Awaits one cleanup task from the handler's own task chain within its remaining execution budget.
    /// Shares the single-use, stop-shielding, and deadline rules of BackgroundTask::Context::Cleanup().
    /// @param task Lazy cleanup work, which may await multiple operations and retains the original stop token.
    /// @return A lazy Task that propagates exceptions from the cleanup work when awaited.
    /// @throws std::logic_error If awaited outside the active attempt's Task chain or cleanup was already used.
    Task<void> Cleanup(Task<void> task) const;

    /// Updates this attempt's platform presentation on the application thread.
    /// @param presentation Application-localized title and subtitle. The resolved title must be nonempty.
    /// @return Success when updated, Unavailable after the attempt ends, or a platform update error.
    /// A returned error is also recorded in TaskState; application code decides whether to end its handler.
    /// @throws std::invalid_argument If resolved text is invalid UTF-8, contains nulls, or has an empty title.
    SystemTaskResult<void> UpdatePresentation(TaskPresentation presentation) const;

  private:
    explicit Context(const std::shared_ptr<detail::SystemTaskAttempt>& attempt);
    std::weak_ptr<detail::SystemTaskAttempt> attempt_;
    std::stop_token stop_token_;
    friend class detail::SystemTasks;
  };

  /// Runs the session on the application thread. Returning ends the session.
  /// Normal return publishes Stopped while execution remains valid; handler failures appear in TaskState.
  using Handler = std::function<Task<void>(Context)>;

  OngoingTask(std::string identifier, Handler handler);

  /// Borrows the stable identifier used in AppOptions and platform configuration.
  /// The returned view borrows declaration-owned storage shared by its copies; no Runtime is needed.
  [[nodiscard]] std::string_view Identifier() const noexcept;
  /// Queries platform ongoing execution support on the application thread.
  [[nodiscard]] SystemTaskCapabilities GetCapabilities() const;
  /// Submits the session directly from a foreground user action.
  /// Platform submission occurs before return; the handler is dispatched only after platform admission.
  /// @param presentation Initial application-localized title and subtitle.
  /// @return Accepted for a new session, NoChange while an existing request is active, or an operational error.
  /// A duplicate retains its original presentation; a request still stopping returns Busy.
  /// @throws std::invalid_argument If resolved text is invalid UTF-8, contains nulls, or has an empty title.
  /// @throws std::logic_error For an invalid context, or during installation, composition, cleanup, or shutdown.
  SystemTaskResult<SubmissionStatus> Start(TaskPresentation presentation) const;
  /// Withdraws the pending session or signals cooperative stop.
  /// @return Requested, NoChange when absent/inactive/already stopping, or an operational error.
  /// A handler may remain Stopping; failed withdrawal keeps the request active. Use WaitAsync() for the outcome.
  /// @throws std::logic_error For an invalid calling context or a call during composition.
  SystemTaskResult<StopRequestStatus> RequestStop() const;
  /// Copies the current session state and subscribes the active composition, if any.
  /// @return An owned snapshot, or Idle when no request is known. Outside composition no subscription is retained.
  [[nodiscard]] TaskState GetState() const;
  /// Selects the current session request now and observes its terminal state when awaited.
  /// @return A lazy observer yielding the selected snapshot, Idle for no request, or Unsupported/Unavailable.
  /// Uses BackgroundTask::WaitAsync()'s request-selection, observer-cancellation, and execution-status rules.
  /// @throws std::logic_error For an invalid context or Runtime, or an attempt waiting for its own completion.
  Task<SystemTaskResult<TaskState>> WaitAsync() const;

private:
  std::shared_ptr<const detail::SystemTaskDeclaration> declaration_;
  friend class detail::SystemTasks;
};

/// One entry in AppOptions::system_tasks. Handlers and operations retain their declaration-specific types.
/// Declarations own immutable identifiers and handlers; copies share the same declaration identity. Construction needs
/// no Runtime. Identifiers must be nonempty UTF-8 without embedded nulls and unique across the installed list.
/// Malformed identifiers and empty handlers throw std::invalid_argument; duplicate identifiers fail installation.
/// Copying or destroying a declaration does not start or stop work. Platform setup belongs to the platform shell;
/// see the system task guide and example_system_tasks for configuration and complete application examples.
using SystemTask = std::variant<BackgroundTask, ContinuedTask, OngoingTask>;

} // namespace huxerui
