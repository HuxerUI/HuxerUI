#include <huxerui/system_task.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <random>
#include <stdexcept>
#include <utility>

#include "application_internal.h"
#include "macros_internal.h"
#include "platform_registry_internal.h"
#include "resources/resource_internal.h"
#include "runtime/task_internal.h"
#include "runtime/ui_window_internal.h"
#include "system_task_internal.h"

namespace huxerui::detail {

struct SystemTaskDeclaration {
  std::string identifier;
  std::variant<BackgroundTask::Handler, ContinuedTask::Handler, OngoingTask::Handler> handler;

  SystemTaskKind Kind() const noexcept { return static_cast<SystemTaskKind>(handler.index()); }
};

struct SystemTaskRecord {
  SystemTaskRequest request;
  State<TaskState> state;
  std::shared_ptr<SystemTaskAttempt> attempt;
  std::map<std::uint64_t, CallbackCompletion<TaskState>> waiters;
};

struct SystemTaskAttempt {
  std::weak_ptr<SystemTasks> owner;
  std::weak_ptr<SystemTaskRecord> record;
  std::shared_ptr<SystemTaskGrant> grant;
  std::shared_ptr<SystemTaskStopState> stop = std::make_shared<SystemTaskStopState>();
  TaskHandle execution;
  std::optional<TaskStopReason> stop_reason;
  std::optional<std::chrono::steady_clock::time_point> deadline;
  std::function<void()> cancel_deadline;
  bool cleanup_used = false;
  bool active = true;
};

namespace {

std::uint64_t NextIdentity() {
  static std::atomic<std::uint64_t> next{1};
  return next.fetch_add(1, std::memory_order_relaxed);
}

std::string NewRequestToken() {
  static const auto nonce = [] {
    std::random_device random;
    return std::to_string(random()) + "-" + std::to_string(random()) + "-" +
           std::to_string(random()) + "-" + std::to_string(random());
  }();
  return nonce + "-" + std::to_string(NextIdentity());
}

void ValidateIdentifier(std::string_view identifier) {
  if (identifier.empty() || identifier.find('\0') != std::string_view::npos || !IsValidUtf8(identifier)) {
    throw std::invalid_argument("HuxerUI system task identifiers must be nonempty UTF-8 without null characters");
  }
}

template <class Handler>
std::shared_ptr<const SystemTaskDeclaration> Declare(std::string identifier, Handler handler) {
  ValidateIdentifier(identifier);
  if (!handler) throw std::invalid_argument("HuxerUI system task handler must not be empty");
  return std::make_shared<const SystemTaskDeclaration>(SystemTaskDeclaration{std::move(identifier), std::move(handler)});
}

SystemTasks& CurrentTasks() {
  const auto application = CurrentApplicationRuntime();
  if (!application->system_tasks) throw std::logic_error("HuxerUI system tasks are not installed in this Runtime");
  return *application->system_tasks;
}

std::shared_ptr<SystemTaskAttempt> LiveAttempt(const std::weak_ptr<SystemTaskAttempt>& weak) {
  const auto attempt = weak.lock();
  if (auto owner = attempt ? attempt->owner.lock() : nullptr) {
    owner->Application();
    if (attempt->active) return attempt;
  }
  return {};
}

template <class T> SystemTaskResult<T> Unavailable() {
  return TaskFailure<T>(SystemTaskErrorCode::Unavailable, "HuxerUI system task request is unavailable");
}

template <class T> SystemTaskResult<T> Unsupported() {
  return TaskFailure<T>(SystemTaskErrorCode::Unsupported, "HuxerUI system task is unsupported by this host");
}

TaskPresentation SnapshotPresentation(const ResolvedTaskPresentation& presentation) {
  return {presentation.title, presentation.subtitle};
}

} // namespace

SystemTaskGrant::SystemTaskGrant(Completion completion) : completion_(std::move(completion)) {}

SystemTaskGrant::~SystemTaskGrant() {
  HUXERUI_DISCARD(Finish(SystemTaskOutcome::Interrupted));
}

SystemTaskResult<void> SystemTaskGrant::Finish(SystemTaskOutcome outcome) {
  live_ = false;
  auto completion = std::exchange(completion_, {});
  if (!completion) return SystemTaskResult<void>::Success();
  try {
    return completion(outcome);
  } catch (const std::exception& exception) {
    return TaskFailure<void>(SystemTaskErrorCode::Platform,
                             std::string("HuxerUI platform task completion failed: ") + exception.what());
  } catch (...) {
    return TaskFailure<void>(SystemTaskErrorCode::Platform, "HuxerUI platform task completion failed");
  }
}

void SystemTaskGrant::Revoke() noexcept {
  if (!live_) return;
  live_ = false;
  revoked_ = true;
  completion_ = {};
}

SystemTaskCapabilities ProcessLocalSystemTasks::Capabilities(SystemTaskKind kind) const noexcept {
  return {.supported = kind != SystemTaskKind::Background};
}

void ProcessLocalSystemTasks::Install(std::span<const SystemTaskRegistration>, EventHandler on_event) {
  on_event_ = std::move(on_event);
}

SystemTaskResult<void> ProcessLocalSystemTasks::Submit(const SystemTaskRequest& request) {
  if (request.kind == SystemTaskKind::Background) return Unsupported<void>();
  on_event_({SystemTaskEvent::Kind::Granted, request, std::make_shared<SystemTaskGrant>()});
  return SystemTaskResult<void>::Success();
}

SystemTaskResult<void> ProcessLocalSystemTasks::Withdraw(const SystemTaskRequest&) {
  return SystemTaskResult<void>::Success();
}

void ProcessLocalSystemTasks::Disconnect() noexcept {
  on_event_ = {};
}

SystemTasks::SystemTasks(const std::shared_ptr<ApplicationRuntimeState>& application,
                         std::shared_ptr<SystemTaskTransport> transport)
    : application_(application), transport_(std::move(transport)),
      scope_(MakeTaskScopeState(application->Dispatcher(), application->execution)) {}

std::shared_ptr<ApplicationRuntimeState> SystemTasks::Application() const {
  auto application = application_.lock();
  if (!application) throw std::logic_error("HuxerUI system task Runtime is retired");
  ValidateApplicationCall(application->execution);
  if (retired_ || application->phase == ApplicationRuntimeState::Phase::Stopped) {
    throw std::logic_error("HuxerUI system task Runtime is retired");
  }
  return application;
}

void SystemTasks::Install(const AppOptions& options) {
  std::vector<SystemTaskRegistration> registrations;
  registrations.reserve(options.system_tasks.size());
  for (const auto& entry : options.system_tasks) {
    const auto declaration = std::visit([](const auto& task) { return task.declaration_; }, entry);
    if (!declaration) throw std::invalid_argument("HuxerUI system task declaration is empty");
    auto [position, inserted] = slots_.try_emplace(declaration->identifier);
    if (!inserted) {
      throw std::invalid_argument("HuxerUI duplicate system task identifier: " + declaration->identifier);
    }
    const auto kind = declaration->Kind();
    auto& slot = position->second;
    slot.declaration = declaration;
    if (kind == SystemTaskKind::Background && transport_ && transport_->Capabilities(kind).supported) {
      slot.empty_state.status_ = TaskStatus::Unknown;
    }
    registrations.push_back({declaration->identifier, kind});
  }
  if (transport_) {
    transport_->Install(registrations, [weak = weak_from_this(), dispatch = Application()->Dispatcher()](
        SystemTaskEvent event) {
      dispatch([weak, event = std::move(event)]() mutable {
        if (auto tasks = weak.lock(); tasks && !tasks->retired_) tasks->OnEvent(std::move(event));
      });
    });
  }
}

SystemTasks::Slot& SystemTasks::RequireSlot(const std::shared_ptr<const SystemTaskDeclaration>& declaration) {
  Application();
  if (declaration) {
    const auto found = slots_.find(declaration->identifier);
    if (found != slots_.end() && found->second.declaration == declaration) return found->second;
  }
  throw std::logic_error("HuxerUI system task declaration is not installed in this Runtime");
}

SystemTaskCapabilities SystemTasks::Capabilities(const std::shared_ptr<const SystemTaskDeclaration>& declaration) {
  const auto kind = RequireSlot(declaration).declaration->Kind();
  return transport_ ? transport_->Capabilities(kind) : SystemTaskCapabilities{};
}

void SystemTasks::RequireSubmission() const {
  const auto application = Application();
  if (application->phase != ApplicationRuntimeState::Phase::Running || application->window_installations ||
      IsSystemTaskCleaningUp() || Composer::Current()) {
    throw std::logic_error("HuxerUI system task submission requires normal application execution");
  }
}

ResolvedTaskPresentation SystemTasks::ResolvePresentation(TaskPresentation presentation) const {
  const auto resources = Application()->resources;
  const auto locale = resources->EffectiveConfiguration(false).locale;
  auto title = ResolveString(std::move(presentation.title), *resources, locale);
  auto subtitle = ResolveString(std::move(presentation.subtitle), *resources, locale);
  const auto valid = [](std::string_view value) { return IsValidUtf8(value) && value.find('\0') == value.npos; };
  if (title.empty() || !valid(title) || !valid(subtitle)) {
    throw std::invalid_argument("HuxerUI task presentation requires a nonempty title and valid UTF-8 text");
  }
  return {std::move(title), std::move(subtitle)};
}

std::shared_ptr<SystemTaskRecord> SystemTasks::Find(Slot& slot, std::string_view instance_id) {
  const auto found = slot.records.find(instance_id);
  return found == slot.records.end() ? nullptr : found->second;
}

std::shared_ptr<SystemTaskRecord> SystemTasks::AddRecord(Slot& slot, SystemTaskRequest request) {
  TaskState state;
  state.record_id_ = NextIdentity();
  state.status_ = TaskStatus::Pending;
  if (request.presentation) state.presentation_ = SnapshotPresentation(*request.presentation);
  auto record = std::make_shared<SystemTaskRecord>(
      SystemTaskRecord{.request = std::move(request), .state = State<TaskState>(std::move(state)),
                       .attempt = {}, .waiters = {}});
  BeginDependencyChange(slot.presence);
  slot.records.insert_or_assign(record->request.instance_id, record);
  CommitDependencyChange(slot.presence);
  return record;
}

void SystemTasks::Publish(SystemTaskRecord& record, TaskState state) {
  record.state = state;
  if (state.IsTerminal()) {
    for (const auto& [identity, completion] : std::exchange(record.waiters, {})) {
      completion.Complete(state);
    }
  }
}

SystemTaskResult<SubmissionStatus> SystemTasks::Submit(Slot& slot, SystemTaskRequest request) {
  using Result = SystemTaskResult<SubmissionStatus>;
  if (const auto current = Find(slot, request.instance_id)) {
    const TaskState& state = current->state;
    if (state.Status() == TaskStatus::Unknown) return Unavailable<SubmissionStatus>();
    if (state.IsActive()) {
      if (state.Status() != TaskStatus::Stopping &&
          (request.kind != SystemTaskKind::Background ||
           (current->request.schedule_known && current->request.schedule == request.schedule))) {
        return Result::Success(SubmissionStatus::NoChange);
      }
      return TaskFailure<SubmissionStatus>(SystemTaskErrorCode::Busy, "HuxerUI system task request is active");
    }
  } else if (slot.empty_state.Status() == TaskStatus::Unknown) {
    return slot.empty_state.Error() ? Result::Failure(*slot.empty_state.Error()) : Unavailable<SubmissionStatus>();
  }
  const bool supported = transport_ && transport_->Capabilities(request.kind).supported;
  if (supported) request.token = NewRequestToken();
  const auto record = AddRecord(slot, std::move(request));
  auto result = supported ? transport_->Submit(record->request) : Unsupported<void>();
  if (!result.Succeeded()) {
    TaskState state = record->state;
    state.status_ = TaskStatus::Failed;
    state.error_ = result.Error();
    Publish(*record, std::move(state));
    return Result::Failure(std::move(result).Error());
  }
  return Result::Success(SubmissionStatus::Accepted);
}

Task<SystemTaskResult<SubmissionStatus>> SystemTasks::Schedule(
    const std::shared_ptr<const SystemTaskDeclaration>& declaration, BackgroundTask::Schedule schedule) {
  RequireSlot(declaration);
  return ScheduleOwned(weak_from_this(), declaration, schedule);
}

Task<SystemTaskResult<SubmissionStatus>> SystemTasks::ScheduleOwned(
    std::weak_ptr<SystemTasks> weak, std::shared_ptr<const SystemTaskDeclaration> declaration,
    BackgroundTask::Schedule schedule) {
  const auto owner = weak.lock();
  if (!owner || owner->Application() != CurrentApplicationRuntime()) {
    throw std::logic_error("HuxerUI task submission belongs to a retired or different Runtime");
  }
  owner->RequireSubmission();
  co_return owner->Submit(owner->RequireSlot(declaration), {.identifier = declaration->identifier,
                                                           .schedule = schedule});
}

SystemTaskResult<SubmissionStatus> SystemTasks::Start(const std::shared_ptr<const SystemTaskDeclaration>& declaration,
    std::string instance_id, TaskPresentation presentation, ContinuedTask::SubmissionPolicy policy) {
  auto& slot = RequireSlot(declaration);
  RequireSubmission();
  if (declaration->Kind() == SystemTaskKind::Continued) ValidateIdentifier(instance_id);
  if (policy != ContinuedTask::SubmissionPolicy::RequireImmediate &&
      policy != ContinuedTask::SubmissionPolicy::AllowQueue) {
    throw std::invalid_argument("HuxerUI invalid continued task submission policy");
  }
  return Submit(slot, {.identifier = declaration->identifier, .instance_id = std::move(instance_id),
                       .kind = declaration->Kind(), .presentation = ResolvePresentation(std::move(presentation)),
                       .policy = policy});
}

SystemTaskResult<StopRequestStatus> SystemTasks::RequestStop(
    const std::shared_ptr<const SystemTaskDeclaration>& declaration, std::string_view instance_id) {
  auto& slot = RequireSlot(declaration);
  if (Composer::Current()) throw std::logic_error("HuxerUI cannot stop a system task during composition");
  if (declaration->Kind() == SystemTaskKind::Continued) ValidateIdentifier(instance_id);
  using Result = SystemTaskResult<StopRequestStatus>;
  if (!transport_ || !transport_->Capabilities(declaration->Kind()).supported) return Unsupported<StopRequestStatus>();
  const auto record = Find(slot, instance_id);
  TaskState state = record ? record->state : slot.empty_state;
  if (state.Status() == TaskStatus::Unknown) return Unavailable<StopRequestStatus>();
  if (!state.IsActive() || state.Status() == TaskStatus::Stopping) return Result::Success(StopRequestStatus::NoChange);
  if (record->attempt) {
    Stop(record->attempt, TaskStopReason::Requested);
  } else {
    auto result = transport_->Withdraw(record->request);
    if (!result.Succeeded()) {
      state.error_ = result.Error();
      Publish(*record, std::move(state));
      return Result::Failure(std::move(result).Error());
    }
    state.status_ = TaskStatus::Stopped;
    state.stop_reason_ = TaskStopReason::Requested;
    state.error_.reset();
    Publish(*record, std::move(state));
  }
  return Result::Success(StopRequestStatus::Requested);
}

std::optional<TaskState> SystemTasks::GetState(const std::shared_ptr<const SystemTaskDeclaration>& declaration,
                                              std::string_view instance_id) {
  auto& slot = RequireSlot(declaration);
  const bool continued = declaration->Kind() == SystemTaskKind::Continued;
  if (continued) ValidateIdentifier(instance_id);
  ObserveDependency(slot.presence);
  if (const auto record = Find(slot, instance_id)) return record->state;
  if (continued) return std::nullopt;
  return slot.empty_state;
}

Task<SystemTaskResult<TaskState>> SystemTasks::Wait(const std::shared_ptr<const SystemTaskDeclaration>& declaration,
                                                   std::string_view instance_id) {
  auto& slot = RequireSlot(declaration);
  const auto kind = declaration->Kind();
  if (kind == SystemTaskKind::Continued) ValidateIdentifier(instance_id);
  if (!transport_ || !transport_->Capabilities(kind).supported) {
    return WaitOwned(weak_from_this(), {}, Unsupported<TaskState>());
  }
  auto record = Find(slot, instance_id);
  auto fallback = (kind == SystemTaskKind::Continued || slot.empty_state.Status() == TaskStatus::Unknown)
      ? Unavailable<TaskState>() : SystemTaskResult<TaskState>::Success(slot.empty_state);
  return WaitOwned(weak_from_this(), std::move(record), std::move(fallback));
}

Task<SystemTaskResult<TaskState>> SystemTasks::WaitOwned(std::weak_ptr<SystemTasks> weak,
    std::shared_ptr<SystemTaskRecord> record, SystemTaskResult<TaskState> fallback) {
  if (const auto owner = weak.lock(); !owner || owner->Application() != CurrentApplicationRuntime()) {
    throw std::logic_error("HuxerUI task observer belongs to a retired or different Runtime");
  }
  if (!record) co_return fallback;
  using Result = SystemTaskResult<TaskState>;
  const TaskState state = record->state;
  if (state.Status() == TaskStatus::Unknown) co_return Unavailable<TaskState>();
  if (state.IsTerminal()) co_return Result::Success(state);
  if (record->attempt && IsCurrentSystemTask(*record->attempt->stop)) {
    throw std::logic_error("HuxerUI a system task cannot wait for its own completion");
  }
  auto wait = AwaitCallback<TaskState>([record](CallbackCompletion<TaskState> completion) {
    const auto identity = NextIdentity();
    record->waiters.emplace(identity, std::move(completion));
    return [weak = std::weak_ptr(record), identity] {
      if (auto retained = weak.lock()) retained->waiters.erase(identity);
    };
  });
  co_return Result::Success(co_await std::move(wait));
}

bool SystemTasks::Forget(const std::shared_ptr<const SystemTaskDeclaration>& declaration,
                         std::string_view instance_id, const TaskState& observed) {
  auto& slot = RequireSlot(declaration);
  ValidateIdentifier(instance_id);
  if (Composer::Current()) throw std::logic_error("HuxerUI cannot forget a system task during composition");
  const auto found = slot.records.find(instance_id);
  if (found == slot.records.end() || !observed.IsTerminal()) return false;
  const TaskState& state = found->second->state;
  if (!state.IsTerminal() || state.record_id_ != observed.record_id_) return false;
  BeginDependencyChange(slot.presence);
  slot.records.erase(found);
  CommitDependencyChange(slot.presence);
  return true;
}

void SystemTasks::OnEvent(SystemTaskEvent event) {
  const auto found = slots_.find(event.request.identifier);
  if (found == slots_.end() || found->second.declaration->Kind() != event.request.kind) return;
  auto& slot = found->second;
  auto record = Find(slot, event.request.instance_id);
  if (event.kind == SystemTaskEvent::Kind::Reconciled) {
    if (record || event.request.kind != SystemTaskKind::Background) return;
    BeginDependencyChange(slot.presence);
    slot.empty_state.status_ = event.error ? TaskStatus::Unknown : TaskStatus::Idle;
    slot.empty_state.error_ = std::move(event.error);
    CommitDependencyChange(slot.presence);
    if (!event.request.token.empty()) AddRecord(slot, std::move(event.request));
    return;
  }
  if (!record && event.kind == SystemTaskEvent::Kind::Granted && event.grant &&
      event.request.kind == SystemTaskKind::Background && !event.request.token.empty() &&
      Application()->accepts_tasks) {
    record = AddRecord(slot, event.request);
  }
  if (!record || record->request.token != event.request.token || !record->state->IsActive()) return;
  switch (event.kind) {
    case SystemTaskEvent::Kind::Granted:
      Admit(slot, record, std::move(event));
      break;
    case SystemTaskEvent::Kind::Rejected:
      if (!record->attempt) {
        TaskState state = record->state;
        state.status_ = TaskStatus::Failed;
        state.error_ = event.error.value_or(SystemTaskError{
            SystemTaskErrorCode::Rejected, "HuxerUI system task admission failed"});
        Publish(*record, std::move(state));
      }
      break;
    case SystemTaskEvent::Kind::StopRequested:
    case SystemTaskEvent::Kind::Revoked:
      if (auto attempt = record->attempt; attempt && attempt->grant == event.grant) {
        if (event.kind == SystemTaskEvent::Kind::Revoked) {
          if (!attempt->stop_reason) attempt->stop_reason = TaskStopReason::System;
          Finish(attempt, SystemTaskOutcome::Interrupted, std::move(event.error));
        } else {
          if (event.deadline) SetDeadline(attempt, *event.deadline);
          Stop(attempt, TaskStopReason::System);
        }
      }
      break;
    case SystemTaskEvent::Kind::Reconciled:
      break;
  }
}

void SystemTasks::Admit(Slot& slot, const std::shared_ptr<SystemTaskRecord>& record, SystemTaskEvent event) {
  const auto application = Application();
  if (record->attempt || !event.grant || !application->accepts_tasks) return;
  if (event.grant->WasRevoked()) {
    TaskState state = record->state;
    state.status_ = TaskStatus::Interrupted;
    state.stop_reason_ = TaskStopReason::System;
    Publish(*record, std::move(state));
    return;
  }
  if (!event.grant->IsLive()) return;
  auto attempt = std::make_shared<SystemTaskAttempt>();
  attempt->owner = weak_from_this();
  attempt->record = record;
  attempt->grant = std::move(event.grant);
  attempt->stop->grant = attempt->grant;
  record->attempt = attempt;
  ++active_;
  TaskState state = record->state;
  state.status_ = TaskStatus::Running;
  Publish(*record, std::move(state));
  try {
    if (event.deadline) SetDeadline(attempt, *event.deadline);
    attempt->execution = LaunchSystemTask(scope_, Run(slot.declaration, attempt), attempt->stop);
  } catch (const std::exception& exception) {
    Finish(attempt, SystemTaskOutcome::Failed, SystemTaskError{SystemTaskErrorCode::Platform, exception.what()});
  }
}

Task<void> SystemTasks::Run(std::shared_ptr<const SystemTaskDeclaration> declaration,
                            std::shared_ptr<SystemTaskAttempt> attempt) {
  SystemTaskOutcome outcome = SystemTaskOutcome::Completed;
  std::optional<SystemTaskError> error;
  try {
    if (attempt->stop->source.stop_requested()) throw TaskStopped{};
    switch (declaration->Kind()) {
      case SystemTaskKind::Background: {
        const auto result = co_await std::get<BackgroundTask::Handler>(declaration->handler)(
            BackgroundTask::Context(attempt));
        if (result == BackgroundTask::Result::Retry) outcome = SystemTaskOutcome::Retry;
        else if (result != BackgroundTask::Result::Completed) {
          throw std::invalid_argument("HuxerUI invalid background task result");
        }
        break;
      }
      case SystemTaskKind::Continued:
        co_await std::get<ContinuedTask::Handler>(declaration->handler)(ContinuedTask::Context(attempt));
        break;
      case SystemTaskKind::Ongoing:
        co_await std::get<OngoingTask::Handler>(declaration->handler)(OngoingTask::Context(attempt));
        outcome = SystemTaskOutcome::Stopped;
        break;
    }
  } catch (const TaskStopped&) {
    outcome = SystemTaskOutcome::Stopped;
  } catch (const std::exception& exception) {
    outcome = SystemTaskOutcome::Failed;
    error = SystemTaskError{SystemTaskErrorCode::Handler,
                            std::string("HuxerUI system task handler failed: ") + exception.what()};
  } catch (...) {
    outcome = SystemTaskOutcome::Failed;
    error = SystemTaskError{SystemTaskErrorCode::Handler, "HuxerUI system task handler failed"};
  }
  if (auto owner = attempt->owner.lock()) owner->Finish(attempt, outcome, std::move(error));
}

std::chrono::steady_clock::time_point SystemTasks::StopDeadline() const {
  using Clock = std::chrono::steady_clock;
  const auto application = Application();
  const auto now = application->owner->TimerNow();
  const auto timeout = application->declaration->options.system_task_stop_timeout;
  if (timeout >= Clock::time_point::max() - now) return Clock::time_point::max();
  return now + std::chrono::duration_cast<Clock::duration>(timeout);
}

void SystemTasks::SetDeadline(const std::shared_ptr<SystemTaskAttempt>& attempt,
                             std::chrono::steady_clock::time_point deadline) {
  if (attempt->deadline && *attempt->deadline <= deadline) return;
  auto cancel = Application()->owner->ScheduleTimerAt(deadline, [weak = std::weak_ptr(attempt)] {
    if (auto active = weak.lock(); active && active->active) {
      if (!active->stop_reason) active->stop_reason = TaskStopReason::Deadline;
      if (auto owner = active->owner.lock()) owner->Finish(active, SystemTaskOutcome::Interrupted);
    }
  });
  if (attempt->cancel_deadline) attempt->cancel_deadline();
  attempt->cancel_deadline = std::move(cancel);
  attempt->deadline = deadline;
}

void SystemTasks::Stop(const std::shared_ptr<SystemTaskAttempt>& attempt, TaskStopReason reason) {
  if (!attempt->active) return;
  if (!attempt->stop_reason) attempt->stop_reason = reason;
  SetDeadline(attempt, shutdown_deadline_.value_or(StopDeadline()));
  const auto record = attempt->record.lock();
  TaskState state = record->state;
  state.status_ = TaskStatus::Stopping;
  state.stop_reason_ = attempt->stop_reason;
  Publish(*record, std::move(state));
  attempt->stop->source.request_stop();
  WakeStoppedTask(*attempt->stop);
}

void SystemTasks::Finish(const std::shared_ptr<SystemTaskAttempt>& attempt, SystemTaskOutcome outcome,
                         std::optional<SystemTaskError> error) {
  if (!attempt->active) return;
  const auto application = Application();
  const auto record = attempt->record.lock();
  if (attempt->grant->WasRevoked()) {
    outcome = SystemTaskOutcome::Interrupted;
    if (!attempt->stop_reason) attempt->stop_reason = TaskStopReason::System;
  } else if (attempt->deadline && application->owner->TimerNow() >= *attempt->deadline) {
    outcome = SystemTaskOutcome::Interrupted;
    if (!attempt->stop_reason) attempt->stop_reason = TaskStopReason::Deadline;
  } else if (attempt->stop->source.stop_requested() &&
             (outcome == SystemTaskOutcome::Completed || outcome == SystemTaskOutcome::Retry)) {
    outcome = SystemTaskOutcome::Stopped;
  }
  attempt->active = false;
  if (attempt->cancel_deadline) std::exchange(attempt->cancel_deadline, {})();
  if (outcome == SystemTaskOutcome::Interrupted) {
    attempt->stop->source.request_stop();
    attempt->execution.Cancel();
  }
  TaskState state = record->state;
  auto result = attempt->grant->Finish(outcome);
  switch (outcome) {
    case SystemTaskOutcome::Completed: state.status_ = TaskStatus::Completed; break;
    case SystemTaskOutcome::Retry:
      state.status_ = TaskStatus::Pending;
      state.progress_.reset();
      break;
    case SystemTaskOutcome::Stopped: state.status_ = TaskStatus::Stopped; break;
    case SystemTaskOutcome::Failed: state.status_ = TaskStatus::Failed; break;
    case SystemTaskOutcome::Interrupted: state.status_ = TaskStatus::Interrupted; break;
  }
  if (!result.Succeeded()) {
    if (outcome == SystemTaskOutcome::Retry) state.status_ = TaskStatus::Failed;
    state.error_ = result.Error();
  }
  if (error) state.error_ = std::move(error);
  state.stop_reason_ = attempt->stop_reason;
  record->attempt.reset();
  --active_;
  Publish(*record, std::move(state));
  if (!active_ && resume_shutdown_) application->Post(std::exchange(resume_shutdown_, {}));
}

Task<void> SystemTasks::Cleanup(std::weak_ptr<SystemTaskAttempt> weak, Task<void> task) {
  const auto attempt = LiveAttempt(weak);
  if (!attempt || !IsCurrentSystemTask(*attempt->stop) || attempt->cleanup_used) {
    throw std::logic_error("HuxerUI system task cleanup may run once within its original execution");
  }
  attempt->cleanup_used = true;
  if (const auto owner = attempt->owner.lock()) {
    owner->SetDeadline(attempt, owner->shutdown_deadline_.value_or(owner->StopDeadline()));
  }
  struct Shield {
    SystemTaskStopState& stop;
    ~Shield() { stop.cleaning_up = false; }
  } shield{*attempt->stop};
  shield.stop.cleaning_up = true;
  co_await std::move(task);
}

SystemTaskResult<void> SystemTasks::ReportProgress(const std::shared_ptr<SystemTaskAttempt>& attempt,
                                                  TaskProgress progress) {
  const auto record = attempt->record.lock();
  TaskState state = record->state;
  const auto& prior = state.Progress();
  if ((progress.total && (*progress.total == 0 || progress.completed > *progress.total)) ||
      (prior && (progress.completed < prior->completed ||
                 (prior->total && (!progress.total || *progress.total < *prior->total))))) {
    throw std::invalid_argument("HuxerUI task progress requires nondecreasing units and a positive total");
  }
  auto result = attempt->grant->update_progress ? attempt->grant->update_progress(progress)
                                                : SystemTaskResult<void>::Success();
  if (result.Succeeded()) state.progress_ = progress;
  else state.error_ = result.Error();
  Publish(*record, std::move(state));
  return result;
}

SystemTaskResult<void> SystemTasks::UpdatePresentation(const std::shared_ptr<SystemTaskAttempt>& attempt,
                                                       TaskPresentation presentation) {
  const auto resolved = ResolvePresentation(std::move(presentation));
  auto result = attempt->grant->update_presentation ? attempt->grant->update_presentation(resolved)
                                                    : SystemTaskResult<void>::Success();
  const auto record = attempt->record.lock();
  TaskState state = record->state;
  if (result.Succeeded()) state.presentation_ = SnapshotPresentation(resolved);
  else state.error_ = result.Error();
  Publish(*record, std::move(state));
  return result;
}

bool SystemTasks::BeginShutdown(std::function<void()> resume_shutdown) {
  if (!shutdown_deadline_) {
    shutdown_deadline_ = StopDeadline();
    for (auto& [identifier, slot] : slots_) {
      for (auto& [instance, record] : slot.records) {
        if (record->attempt) {
          Stop(record->attempt, TaskStopReason::Shutdown);
        } else if (record->request.kind != SystemTaskKind::Background && record->state->IsActive()) {
          HUXERUI_DISCARD(transport_->Withdraw(record->request));
        }
      }
    }
  }
  if (!active_) return true;
  resume_shutdown_ = std::move(resume_shutdown);
  return false;
}

void SystemTasks::Retire() noexcept {
  if (retired_) return;
  retired_ = true;
  resume_shutdown_ = {};
  for (auto& [identifier, slot] : slots_) {
    for (auto& [instance, record] : slot.records) {
      if (const auto attempt = record->attempt) {
        attempt->active = false;
        attempt->stop->source.request_stop();
      }
    }
  }
  CloseTaskScope(scope_);
  for (auto& [identifier, slot] : slots_) {
    for (auto& [instance, record] : slot.records) {
      if (auto attempt = std::exchange(record->attempt, {})) {
        if (attempt->cancel_deadline) attempt->cancel_deadline();
        HUXERUI_DISCARD(attempt->grant->Finish(SystemTaskOutcome::Interrupted));
      }
      record->waiters.clear();
    }
  }
  if (transport_) transport_->Disconnect();
  slots_.clear();
}

} // namespace huxerui::detail

namespace huxerui {

const char* TaskStopped::what() const noexcept {
  return "HuxerUI system task was stopped";
}

bool TaskState::IsActive() const noexcept {
  return status_ == TaskStatus::Pending || status_ == TaskStatus::Running || status_ == TaskStatus::Stopping;
}

bool TaskState::IsTerminal() const noexcept {
  return status_ == TaskStatus::Completed || status_ == TaskStatus::Stopped ||
         status_ == TaskStatus::Failed || status_ == TaskStatus::Interrupted;
}

BackgroundTask::BackgroundTask(std::string identifier, Handler handler)
    : declaration_(detail::Declare(std::move(identifier), std::move(handler))) {}

std::string_view BackgroundTask::Identifier() const noexcept {
  return declaration_ ? std::string_view(declaration_->identifier) : std::string_view{};
}

SystemTaskCapabilities BackgroundTask::GetCapabilities() const {
  return detail::CurrentTasks().Capabilities(declaration_);
}

BackgroundTask::Context::Context(const std::shared_ptr<detail::SystemTaskAttempt>& attempt)
    : attempt_(attempt), stop_token_(attempt->stop->source.get_token()) {}

std::optional<TaskStopReason> BackgroundTask::Context::StopReason() const {
  if (auto attempt = detail::LiveAttempt(attempt_)) return attempt->stop_reason;
  return std::nullopt;
}

std::optional<std::chrono::steady_clock::time_point> BackgroundTask::Context::Deadline() const {
  if (auto attempt = detail::LiveAttempt(attempt_)) return attempt->deadline;
  return std::nullopt;
}

void BackgroundTask::Context::ThrowIfStopRequested() const {
  if (stop_token_.stop_requested()) throw TaskStopped{};
}

Task<void> BackgroundTask::Context::Cleanup(Task<void> task) const {
  return detail::SystemTasks::Cleanup(attempt_, std::move(task));
}

SystemTaskResult<void> BackgroundTask::Context::ReportProgress(TaskProgress progress) const {
  if (auto attempt = detail::LiveAttempt(attempt_)) {
    return attempt->owner.lock()->ReportProgress(attempt, progress);
  }
  return detail::Unavailable<void>();
}

ContinuedTask::ContinuedTask(std::string identifier, Handler handler)
    : declaration_(detail::Declare(std::move(identifier), std::move(handler))) {}

std::string_view ContinuedTask::Identifier() const noexcept {
  return declaration_ ? std::string_view(declaration_->identifier) : std::string_view{};
}

SystemTaskCapabilities ContinuedTask::GetCapabilities() const {
  return detail::CurrentTasks().Capabilities(declaration_);
}

ContinuedTask::Context::Context(const std::shared_ptr<detail::SystemTaskAttempt>& attempt)
    : attempt_(attempt), stop_token_(attempt->stop->source.get_token()),
      instance_id_(attempt->record.lock()->request.instance_id) {}

std::optional<TaskStopReason> ContinuedTask::Context::StopReason() const {
  if (auto attempt = detail::LiveAttempt(attempt_)) return attempt->stop_reason;
  return std::nullopt;
}

std::optional<std::chrono::steady_clock::time_point> ContinuedTask::Context::Deadline() const {
  if (auto attempt = detail::LiveAttempt(attempt_)) return attempt->deadline;
  return std::nullopt;
}

void ContinuedTask::Context::ThrowIfStopRequested() const {
  if (stop_token_.stop_requested()) throw TaskStopped{};
}

Task<void> ContinuedTask::Context::Cleanup(Task<void> task) const {
  return detail::SystemTasks::Cleanup(attempt_, std::move(task));
}

SystemTaskResult<void> ContinuedTask::Context::ReportProgress(TaskProgress progress) const {
  if (auto attempt = detail::LiveAttempt(attempt_)) {
    return attempt->owner.lock()->ReportProgress(attempt, progress);
  }
  return detail::Unavailable<void>();
}

SystemTaskResult<void> ContinuedTask::Context::UpdatePresentation(TaskPresentation presentation) const {
  if (auto attempt = detail::LiveAttempt(attempt_)) {
    return attempt->owner.lock()->UpdatePresentation(attempt, std::move(presentation));
  }
  return detail::Unavailable<void>();
}

OngoingTask::OngoingTask(std::string identifier, Handler handler)
    : declaration_(detail::Declare(std::move(identifier), std::move(handler))) {}

std::string_view OngoingTask::Identifier() const noexcept {
  return declaration_ ? std::string_view(declaration_->identifier) : std::string_view{};
}

SystemTaskCapabilities OngoingTask::GetCapabilities() const {
  return detail::CurrentTasks().Capabilities(declaration_);
}

OngoingTask::Context::Context(const std::shared_ptr<detail::SystemTaskAttempt>& attempt)
    : attempt_(attempt), stop_token_(attempt->stop->source.get_token()) {}

std::optional<TaskStopReason> OngoingTask::Context::StopReason() const {
  if (auto attempt = detail::LiveAttempt(attempt_)) return attempt->stop_reason;
  return std::nullopt;
}

std::optional<std::chrono::steady_clock::time_point> OngoingTask::Context::Deadline() const {
  if (auto attempt = detail::LiveAttempt(attempt_)) return attempt->deadline;
  return std::nullopt;
}

void OngoingTask::Context::ThrowIfStopRequested() const {
  if (stop_token_.stop_requested()) throw TaskStopped{};
}

Task<void> OngoingTask::Context::Cleanup(Task<void> task) const {
  return detail::SystemTasks::Cleanup(attempt_, std::move(task));
}

SystemTaskResult<void> OngoingTask::Context::UpdatePresentation(TaskPresentation presentation) const {
  if (auto attempt = detail::LiveAttempt(attempt_)) {
    return attempt->owner.lock()->UpdatePresentation(attempt, std::move(presentation));
  }
  return detail::Unavailable<void>();
}

Task<SystemTaskResult<SubmissionStatus>> BackgroundTask::ScheduleAsync(Schedule schedule) const {
  return detail::CurrentTasks().Schedule(declaration_, schedule);
}

Task<SystemTaskResult<SubmissionStatus>> BackgroundTask::ScheduleAsync() const {
  return ScheduleAsync({});
}

SystemTaskResult<SubmissionStatus> ContinuedTask::Start(
    std::string instance_id, TaskPresentation presentation, SubmissionPolicy policy) const {
  return detail::CurrentTasks().Start(declaration_, std::move(instance_id), std::move(presentation), policy);
}

SystemTaskResult<SubmissionStatus> OngoingTask::Start(TaskPresentation presentation) const {
  return detail::CurrentTasks().Start(declaration_, {}, std::move(presentation),
                                     ContinuedTask::SubmissionPolicy::RequireImmediate);
}

SystemTaskResult<StopRequestStatus> BackgroundTask::RequestStop() const {
  return detail::CurrentTasks().RequestStop(declaration_, {});
}

TaskState BackgroundTask::GetState() const {
  return detail::CurrentTasks().GetState(declaration_, {}).value();
}

Task<SystemTaskResult<TaskState>> BackgroundTask::WaitAsync() const {
  return detail::CurrentTasks().Wait(declaration_, {});
}

SystemTaskResult<StopRequestStatus> ContinuedTask::RequestStop(std::string_view instance_id) const {
  return detail::CurrentTasks().RequestStop(declaration_, instance_id);
}

std::optional<TaskState> ContinuedTask::GetState(std::string_view instance_id) const {
  return detail::CurrentTasks().GetState(declaration_, instance_id);
}

Task<SystemTaskResult<TaskState>> ContinuedTask::WaitAsync(std::string_view instance_id) const {
  return detail::CurrentTasks().Wait(declaration_, instance_id);
}

SystemTaskResult<StopRequestStatus> OngoingTask::RequestStop() const {
  return detail::CurrentTasks().RequestStop(declaration_, {});
}

TaskState OngoingTask::GetState() const {
  return detail::CurrentTasks().GetState(declaration_, {}).value();
}

Task<SystemTaskResult<TaskState>> OngoingTask::WaitAsync() const {
  return detail::CurrentTasks().Wait(declaration_, {});
}

bool ContinuedTask::Forget(std::string_view instance_id, const TaskState& observed_state) const {
  return detail::CurrentTasks().Forget(declaration_, instance_id, observed_state);
}

} // namespace huxerui
