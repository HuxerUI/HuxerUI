#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/state.h>
#include <huxerui/system_task.h>

namespace huxerui {

struct AppOptions;

namespace detail {

struct ApplicationRuntimeState;
enum class SystemTaskKind { Background, Continued, Ongoing };
enum class SystemTaskOutcome { Completed, Retry, Stopped, Failed, Interrupted };

template <class T> SystemTaskResult<T> TaskFailure(SystemTaskErrorCode code, std::string message) {
  return SystemTaskResult<T>::Failure({code, std::move(message)});
}

struct SystemTaskRegistration {
  std::string identifier;
  SystemTaskKind kind;
};

struct ResolvedTaskPresentation {
  std::string title;
  std::string subtitle;
};

/// Correlates an accepted request across platform callbacks and retries.
struct SystemTaskRequest {
  std::string identifier;
  std::string instance_id{};
  std::string token{};
  SystemTaskKind kind = SystemTaskKind::Background;
  BackgroundTask::Schedule schedule{};
  bool schedule_known = true;
  std::optional<ResolvedTaskPresentation> presentation{};
  ContinuedTask::SubmissionPolicy policy = ContinuedTask::SubmissionPolicy::RequireImmediate;
};

/// One platform allowance, finalized once on the application thread even if queued delivery is discarded.
class SystemTaskGrant final {
public:
  using Completion = std::function<SystemTaskResult<void>(SystemTaskOutcome)>;
  explicit SystemTaskGrant(Completion completion = {});

#if defined(__OHOS__)
  /// Used by transports that publish the execution diagnostic outside the originating Runtime.
  using DetailedCompletion = std::function<SystemTaskResult<void>(SystemTaskOutcome,
                                                                 const std::optional<SystemTaskError>&)>;
  explicit SystemTaskGrant(DetailedCompletion completion);
#endif

  ~SystemTaskGrant();
  SystemTaskGrant(const SystemTaskGrant&) = delete;
  SystemTaskGrant& operator=(const SystemTaskGrant&) = delete;

  /// Finalizes the allowance and returns any platform completion or retry error.
#if defined(__OHOS__)
  /// The execution error accompanies the terminal snapshot sent to the observing Runtime.
  SystemTaskResult<void> Finish(SystemTaskOutcome outcome, const std::optional<SystemTaskError>& error = {});
#else
  SystemTaskResult<void> Finish(SystemTaskOutcome outcome);
#endif
  /// Marks a platform allowance that has already been revoked, suppressing its completion callback.
  void Revoke() noexcept;
  /// Indicates whether this exact allowance can admit a handler.
  bool IsLive() const noexcept { return live_; }
  /// Indicates that the host withdrew the allowance before local completion.
  bool WasRevoked() const noexcept { return revoked_; }

  /// Rechecks admission at handler entry when cancellation can come from another process.
  std::function<bool()> begin;
  std::function<SystemTaskResult<void>(TaskProgress)> update_progress;
  std::function<SystemTaskResult<void>(const ResolvedTaskPresentation&)> update_presentation;

private:
#if defined(__OHOS__)
  std::variant<Completion, DetailedCompletion> completion_;
#else
  Completion completion_;
#endif
  bool live_ = true;
  bool revoked_ = false;
};

#if defined(__OHOS__)
/// A snapshot of work owned by another Runtime. Observing it does not admit a local handler.
struct SystemTaskObservation {
  TaskStatus status;
  std::optional<TaskStopReason> stop_reason{};
  std::optional<TaskProgress> progress{};
};
#endif

struct SystemTaskEvent {
  enum class Kind {
    Reconciled, Granted, Rejected, StopRequested, Revoked,
#if defined(__OHOS__)
    Observed,
#endif
  };
  Kind kind;
  SystemTaskRequest request;
  std::shared_ptr<SystemTaskGrant> grant{};
  std::optional<std::chrono::steady_clock::time_point> deadline{};
  std::optional<SystemTaskError> error{};
  std::optional<TaskStopReason> stop_reason{};
#if defined(__OHOS__)
  std::optional<SystemTaskObservation> observation{};
#endif
};

/// System scheduling and platform admission. Calls and event publication use the original application thread.
/// Reconciled events describe each background declaration at startup. An empty token means no pending request,
/// and an error means the platform state is indeterminate. Granted events name the exact platform allowance.
class SystemTaskTransport {
public:
  using EventHandler = std::function<void(SystemTaskEvent)>;

  virtual ~SystemTaskTransport() = default;
  virtual SystemTaskCapabilities Capabilities(SystemTaskKind kind) const noexcept = 0;
  virtual void Install(std::span<const SystemTaskRegistration> registrations, EventHandler on_event) = 0;
  virtual SystemTaskResult<void> Submit(const SystemTaskRequest& request) = 0;
  virtual SystemTaskResult<void> Withdraw(const SystemTaskRequest& request) = 0;
#if defined(__OHOS__)
  /// Whether an accepted withdrawal will report its outcome through an observation from another process.
  virtual bool ObservesWithdrawal(const SystemTaskRequest&) const noexcept { return false; }
#endif
  /// Detaches callbacks and retires process-local work.
  /// Accepted background requests remain scheduled with the system.
  virtual void Disconnect() noexcept = 0;
};

/// User-started executions on hosts where an application may keep working while its process lives.
class ProcessLocalSystemTasks : public SystemTaskTransport {
public:
  SystemTaskCapabilities Capabilities(SystemTaskKind kind) const noexcept override;
  void Install(std::span<const SystemTaskRegistration> registrations, EventHandler on_event) override;
  SystemTaskResult<void> Submit(const SystemTaskRequest& request) override;
  SystemTaskResult<void> Withdraw(const SystemTaskRequest& request) override;
  void Disconnect() noexcept override;

protected:
  EventHandler on_event_;
};

struct SystemTaskRecord;

/// Runtime-owned catalog, request state, and cooperative shutdown for all system task declarations.
class SystemTasks final : public std::enable_shared_from_this<SystemTasks> {
public:
  SystemTasks(const std::shared_ptr<ApplicationRuntimeState>& application,
              std::shared_ptr<SystemTaskTransport> transport);
  void Install(const AppOptions& options);
  bool BeginShutdown(std::function<void()> resume_shutdown);
  void Retire() noexcept;

  std::shared_ptr<ApplicationRuntimeState> Application() const;
  SystemTaskCapabilities Capabilities(const std::shared_ptr<const SystemTaskDeclaration>& declaration);
  Task<SystemTaskResult<SubmissionStatus>> Schedule(
      const std::shared_ptr<const SystemTaskDeclaration>& declaration, BackgroundTask::Schedule schedule);
  SystemTaskResult<SubmissionStatus> Start(
      const std::shared_ptr<const SystemTaskDeclaration>& declaration, std::string instance_id,
      TaskPresentation presentation, ContinuedTask::SubmissionPolicy policy);
  SystemTaskResult<StopRequestStatus> RequestStop(
      const std::shared_ptr<const SystemTaskDeclaration>& declaration, std::string_view instance_id);
  std::optional<TaskState> GetState(
      const std::shared_ptr<const SystemTaskDeclaration>& declaration, std::string_view instance_id);
  Task<SystemTaskResult<TaskState>> Wait(
      const std::shared_ptr<const SystemTaskDeclaration>& declaration, std::string_view instance_id);
  bool Forget(
      const std::shared_ptr<const SystemTaskDeclaration>& declaration, std::string_view instance_id,
      const TaskState& observed);

  static Task<void> Cleanup(std::weak_ptr<SystemTaskAttempt> attempt, Task<void> task);
  SystemTaskResult<void> ReportProgress(const std::shared_ptr<SystemTaskAttempt>& attempt, TaskProgress progress);
  SystemTaskResult<void> UpdatePresentation(
      const std::shared_ptr<SystemTaskAttempt>& attempt, TaskPresentation presentation);

private:
  struct Slot {
    std::shared_ptr<const SystemTaskDeclaration> declaration;
    std::map<std::string, std::shared_ptr<SystemTaskRecord>, std::less<>> records;
    TaskState empty_state;
    std::shared_ptr<CompositionDependency> presence = std::make_shared<CompositionDependency>();
  };

  Slot& RequireSlot(const std::shared_ptr<const SystemTaskDeclaration>& declaration);
  void RequireSubmission() const;
  ResolvedTaskPresentation ResolvePresentation(TaskPresentation presentation) const;
  static Task<SystemTaskResult<SubmissionStatus>> ScheduleOwned(
      std::weak_ptr<SystemTasks> owner, std::shared_ptr<const SystemTaskDeclaration> declaration,
      BackgroundTask::Schedule schedule);
  static Task<SystemTaskResult<TaskState>> WaitOwned(
      std::weak_ptr<SystemTasks> owner, std::shared_ptr<SystemTaskRecord> record, SystemTaskResult<TaskState> fallback);
  static std::shared_ptr<SystemTaskRecord> Find(Slot& slot, std::string_view instance_id);
  std::shared_ptr<SystemTaskRecord> AddRecord(Slot& slot, SystemTaskRequest request);
  SystemTaskResult<SubmissionStatus> Submit(Slot& slot, SystemTaskRequest request);
  void OnEvent(SystemTaskEvent event);
  void Admit(Slot& slot, const std::shared_ptr<SystemTaskRecord>& record, SystemTaskEvent event);
  static Task<void> Run(
      std::shared_ptr<const SystemTaskDeclaration> declaration, std::shared_ptr<SystemTaskAttempt> attempt);
  void Stop(const std::shared_ptr<SystemTaskAttempt>& attempt, TaskStopReason reason);
  void SetDeadline(const std::shared_ptr<SystemTaskAttempt>& attempt, std::chrono::steady_clock::time_point deadline);
  void Finish(
      const std::shared_ptr<SystemTaskAttempt>& attempt, SystemTaskOutcome outcome,
      std::optional<SystemTaskError> error = {});
  void Publish(SystemTaskRecord& record, TaskState state);
  std::chrono::steady_clock::time_point StopDeadline() const;

  std::weak_ptr<ApplicationRuntimeState> application_;
  std::shared_ptr<SystemTaskTransport> transport_;
  std::shared_ptr<TaskScopeImpl> scope_;
  std::map<std::string, Slot, std::less<>> slots_;
  std::size_t active_ = 0;
  std::optional<std::chrono::steady_clock::time_point> shutdown_deadline_;
  std::function<void()> resume_shutdown_;
  bool retired_ = false;
};

} // namespace detail
} // namespace huxerui
