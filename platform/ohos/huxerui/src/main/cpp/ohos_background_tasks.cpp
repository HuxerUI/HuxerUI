#include "ohos_background_tasks.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace huxerui::detail {
namespace {

using Clock = std::chrono::system_clock;
using Milliseconds = std::chrono::milliseconds;
constexpr auto work_budget = std::chrono::minutes(2);
constexpr int first_work_id = 0x40000000;

std::int64_t Now() {
  return std::chrono::duration_cast<Milliseconds>(Clock::now().time_since_epoch()).count();
}

[[noreturn]] void StoreError() { throw std::runtime_error("HuxerUI could not access the OHOS background task store"); }

void Check(napi_status status) {
  if (status != napi_ok) throw std::runtime_error("HuxerUI could not call the OHOS work scheduler host");
}

ReferenceDescriptor RequireDescriptor(int descriptor) {
  if (descriptor < 0) StoreError();
  return ReferenceDescriptor(descriptor);
}

struct HandleScope {
  explicit HandleScope(napi_env env) : env(env) { Check(napi_open_handle_scope(env, &scope)); }
  ~HandleScope() { napi_close_handle_scope(env, scope); }

  napi_env env;
  napi_handle_scope scope = nullptr;
};

struct Work {
  int id = 0;
  std::uint64_t revision = 1;
  SystemTaskRequest request;
  SystemTaskObservation snapshot{TaskStatus::Pending};
  std::optional<SystemTaskError> error;
  std::int64_t deadline = 0;

  bool Active() const noexcept {
    return snapshot.status == TaskStatus::Pending || snapshot.status == TaskStatus::Running ||
           snapshot.status == TaskStatus::Stopping;
  }
};

Work ReadWork(std::istream& input) {
  Work work;
  auto& request = work.request;
  int status, stop, code;
  bool has_earliest, has_progress, has_total;
  Clock::duration::rep earliest;
  std::uint64_t completed, total;
  std::string message;
  if (!(input >> work.id >> work.revision >> status >> stop >> work.deadline >> has_earliest >> earliest >>
        request.schedule.requires_network >> request.schedule.requires_external_power >> has_progress >> completed >>
        has_total >> total >> code >> std::quoted(request.identifier) >> std::quoted(request.token) >>
        std::quoted(message)) || work.id < first_work_id || request.identifier.empty() || request.token.empty() ||
      status < static_cast<int>(TaskStatus::Pending) || status > static_cast<int>(TaskStatus::Interrupted) ||
      stop < -1 || stop > static_cast<int>(TaskStopReason::Deadline) ||
      code < -1 || code > static_cast<int>(SystemTaskErrorCode::Handler)) StoreError();
  if (has_earliest) request.schedule.earliest_start = Clock::time_point(Clock::duration(earliest));
  work.snapshot.status = static_cast<TaskStatus>(status);
  if (stop >= 0) work.snapshot.stop_reason = static_cast<TaskStopReason>(stop);
  if (has_progress) work.snapshot.progress = TaskProgress{completed, has_total ? std::optional(total) : std::nullopt};
  if (code >= 0) work.error = SystemTaskError{static_cast<SystemTaskErrorCode>(code), std::move(message)};
  return work;
}

void WriteWork(std::ostream& output, const Work& work) {
  const auto& schedule = work.request.schedule;
  const auto& snapshot = work.snapshot;
  const auto progress = snapshot.progress.value_or(TaskProgress{});
  output << work.id << ' ' << work.revision << ' ' << static_cast<int>(snapshot.status) << ' '
         << (snapshot.stop_reason ? static_cast<int>(*snapshot.stop_reason) : -1) << ' ' << work.deadline << ' '
         << schedule.earliest_start.has_value() << ' '
         << schedule.earliest_start.value_or(Clock::time_point{}).time_since_epoch().count() << ' '
         << schedule.requires_network << ' ' << schedule.requires_external_power << ' '
         << snapshot.progress.has_value() << ' ' << progress.completed << ' ' << progress.total.has_value() << ' '
         << progress.total.value_or(0) << ' ' << (work.error ? static_cast<int>(work.error->code) : -1) << ' '
         << std::quoted(work.request.identifier) << ' ' << std::quoted(work.request.token) << ' '
         << std::quoted(work.error ? work.error->message : std::string{}) << '\n';
}

void WriteAll(int descriptor, std::string_view bytes) {
  while (!bytes.empty()) {
    const auto written = write(descriptor, bytes.data(), bytes.size());
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) StoreError();
    bytes.remove_prefix(static_cast<std::size_t>(written));
  }
  if (fsync(descriptor) != 0) StoreError();
}

/// The separate lock file stays locked across catalog replacement, so both processes compare the same record.
class Catalog {
public:
  explicit Catalog(std::string_view directory)
      : path_(std::string(directory) + "/.huxerui-background-tasks"),
        lock_(RequireDescriptor(open((path_ + ".lock").c_str(), O_CREAT | O_CLOEXEC | O_RDWR, 0600))) {
    while (flock(lock_.Get(), LOCK_EX) != 0) {
      if (errno != EINTR) StoreError();
    }
    std::ifstream input(path_, std::ios::binary);
    if (!input) {
      if (std::filesystem::exists(path_)) StoreError();
      return;
    }
    std::string version;
    if (!(input >> version >> last_id_) || version != "HUXERUI_WORK_1" || last_id_ < first_work_id - 1) StoreError();
    while (input >> std::ws && !input.eof()) {
      auto work = ReadWork(input);
      if (work.id > last_id_) StoreError();
      std::string identifier = work.request.identifier;
      if (!entries.emplace(std::move(identifier), std::move(work)).second) StoreError();
    }
    if (!input.eof()) StoreError();
  }

  Work* Find(int id) {
    for (auto& [identifier, work] : entries) if (work.id == id) return &work;
    return nullptr;
  }

  int Allocate() {
    if (last_id_ == std::numeric_limits<int>::max()) StoreError();
    return ++last_id_;
  }

  void Save() const {
    std::ostringstream output;
    output << "HUXERUI_WORK_1 " << last_id_ << '\n';
    for (const auto& entry : entries) WriteWork(output, entry.second);
    const auto temporary = path_ + ".tmp";
    const auto file = RequireDescriptor(open(temporary.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600));
    WriteAll(file.Get(), std::move(output).str());
    if (rename(temporary.c_str(), path_.c_str()) != 0) StoreError();
  }

  ReferenceDescriptor Observe(std::string_view token) const {
    const auto path = CompletionPath(token);
    return RequireDescriptor(open(path.c_str(), O_CREAT | O_RDONLY | O_CLOEXEC, 0600));
  }

  void PreserveCompletion(const Work& work) const {
    const auto path = CompletionPath(work.request.token);
    const int descriptor = open(path.c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (descriptor < 0 && errno == ENOENT) return;
    const auto file = RequireDescriptor(descriptor);
    std::ostringstream output;
    WriteWork(output, work);
    WriteAll(file.Get(), std::move(output).str());
    // Live observers retain the file through their descriptors. The file can be unlinked before replacing the row.
    if (unlink(path.c_str()) != 0) StoreError();
  }

  Work ReadCompletion(int descriptor) const {
    std::array<char, 4096> buffer;
    std::string bytes;
    for (;;) {
      const auto count = pread(descriptor, buffer.data(), buffer.size(), static_cast<off_t>(bytes.size()));
      if (count < 0 && errno == EINTR) continue;
      if (count < 0) StoreError();
      if (count == 0) break;
      bytes.append(buffer.data(), static_cast<std::size_t>(count));
    }
    std::istringstream input(std::move(bytes));
    auto work = ReadWork(input);
    if (work.Active()) StoreError();
    return work;
  }

  std::map<std::string, Work, std::less<>> entries;

private:
  std::string CompletionPath(std::string_view token) const {
    if (token.empty() || token.find_first_not_of("0123456789-") != token.npos) StoreError();
    return path_ + ".completion-" + std::string(token);
  }

  std::string path_;
  ReferenceDescriptor lock_;
  int last_id_ = first_work_id - 1;
};

void Interrupt(Work& work, TaskStopReason reason) {
  work.snapshot.status = TaskStatus::Interrupted;
  if (!work.snapshot.stop_reason) work.snapshot.stop_reason = reason;
  ++work.revision;
}

SystemTaskEvent Observed(const Work& work) {
  return {.kind = SystemTaskEvent::Kind::Observed, .request = work.request,
          .error = work.error, .observation = work.snapshot};
}

} // namespace

OhosBackgroundTasks::OhosBackgroundTasks(napi_env env, napi_value host, std::string directory)
    : env_(env), directory_(std::move(directory)) {
  Check(napi_create_reference(env_, host, 1, &host_));
}

OhosBackgroundTasks::~OhosBackgroundTasks() { Disconnect(); }

bool OhosBackgroundTasks::PrepareDelivery(std::string_view directory, int work_id) {
  if (work_id < first_work_id) return false;
  Catalog catalog(directory);
  auto* work = catalog.Find(work_id);
  if (!work || work->snapshot.status != TaskStatus::Pending) return false;
  const auto now = Now();
  if (!work->deadline) {
    work->deadline = now + std::chrono::duration_cast<Milliseconds>(work_budget).count();
    catalog.Save();
  }
  return now < work->deadline;
}

void OhosBackgroundTasks::Install(std::span<const SystemTaskRegistration> registrations,
                                  SystemTaskTransport::EventHandler on_event) {
  for (const auto& registration : registrations) {
    if (registration.kind == SystemTaskKind::Background) registrations_.insert(registration.identifier);
  }
  on_event_ = std::move(on_event);
  try {
    static_cast<void>(Refresh());
  } catch (const std::exception& error) {
    for (const auto& identifier : registrations_) {
      on_event_({.kind = SystemTaskEvent::Kind::Reconciled, .request = {.identifier = identifier},
                 .error = SystemTaskError{SystemTaskErrorCode::Platform, error.what()}});
    }
  }
}

SystemTaskResult<void> OhosBackgroundTasks::Submit(const SystemTaskRequest& request) {
  try {
    Catalog catalog(directory_);
    auto& work = catalog.entries[request.identifier];
    if (work.id && work.Active()) {
      return TaskFailure<void>(SystemTaskErrorCode::Busy, "HuxerUI OHOS background task is active");
    }
    if (work.id) catalog.PreserveCompletion(work);
    work = Work{.id = catalog.Allocate(), .request = request};
    // The callback can arrive as soon as startWork submits. It cannot claim this record until submission is settled.
    catalog.Save();
    auto& observation = observed_[request.identifier];
    Observation next{.token = request.token};
    auto result = SystemTaskResult<void>::Success();
    try {
      next.completion = catalog.Observe(next.token);
      result = StartWork(work.id, request.schedule);
    } catch (const std::exception& error) {
      result = TaskFailure<void>(SystemTaskErrorCode::Platform, error.what());
    }
    if (result.Succeeded()) {
      observation = std::move(next);
    } else {
      work.snapshot.status = TaskStatus::Failed;
      work.error = result.Error();
      ++work.revision;
      catalog.Save();
    }
    return result;
  } catch (const std::exception& error) {
    return TaskFailure<void>(SystemTaskErrorCode::Platform, error.what());
  }
}

SystemTaskResult<void> OhosBackgroundTasks::Withdraw(const SystemTaskRequest& request) {
  try {
    int pending_id = 0;
    {
      Catalog catalog(directory_);
      const auto found = catalog.entries.find(request.identifier);
      if (found == catalog.entries.end() || found->second.request.token != request.token) {
        return TaskFailure<void>(SystemTaskErrorCode::Unavailable, "HuxerUI OHOS background request is unavailable");
      }
      auto& work = found->second;
      if (work.Active()) {
        work.snapshot.stop_reason = TaskStopReason::Requested;
        if (work.snapshot.status == TaskStatus::Pending) {
          pending_id = work.id;
          work.snapshot.status = TaskStatus::Stopped;
        } else {
          work.snapshot.status = TaskStatus::Stopping;
        }
        ++work.revision;
        catalog.Save();
      }
    }
    // Once the cancellation is stored, a late callback cannot enter the handler even if the IPC fails.
    if (pending_id) StopWork(pending_id, request.schedule);
    static_cast<void>(Refresh());
    return SystemTaskResult<void>::Success();
  } catch (const std::exception& error) {
    return TaskFailure<void>(SystemTaskErrorCode::Platform, error.what());
  }
}

void OhosBackgroundTasks::Disconnect() noexcept {
  on_event_ = {};
  observed_.clear();
  executions_.clear();
  if (host_) napi_delete_reference(env_, std::exchange(host_, nullptr));
}

std::pair<std::vector<int>, bool> OhosBackgroundTasks::Refresh() {
  std::vector<int> pending;
  std::vector<std::pair<int, BackgroundTask::Schedule>> expired;
  bool active = false;
  if (registrations_.empty()) return {std::move(pending), active};
  {
    Catalog catalog(directory_);
    const auto now = Now();
    for (const auto& identifier : registrations_) {
      const auto found = catalog.entries.find(identifier);
      if (found == catalog.entries.end()) continue;
      auto& work = found->second;
      if (work.Active() && work.deadline && now >= work.deadline) {
        Interrupt(work, TaskStopReason::Deadline);
        expired.emplace_back(work.id, work.request.schedule);
      }
    }
    if (!expired.empty()) catalog.Save();

    // The OHOS dispatcher queues these events on the application thread. Handlers run after the catalog is unlocked.
    for (auto entry = executions_.begin(); entry != executions_.end();) {
      const auto* work = catalog.Find(entry->first);
      if (auto grant = entry->second.grant.lock()) {
        if (work && work->Active()) {
          if (work->snapshot.status == TaskStatus::Stopping && !entry->second.stop_sent) {
            on_event_({.kind = SystemTaskEvent::Kind::StopRequested, .request = work->request,
                       .grant = std::move(grant), .stop_reason = work->snapshot.stop_reason});
            entry->second.stop_sent = true;
          }
          ++entry;
          continue;
        }
        grant->Revoke();
        on_event_({.kind = SystemTaskEvent::Kind::Revoked, .request = entry->second.request,
                   .grant = std::move(grant), .stop_reason = work ? work->snapshot.stop_reason : std::nullopt});
      }
      entry = executions_.erase(entry);
    }
    for (const auto& identifier : registrations_) {
      const auto found = catalog.entries.find(identifier);
      if (found == catalog.entries.end()) {
        if (observed_.try_emplace(identifier).second) {
          on_event_({SystemTaskEvent::Kind::Reconciled, {.identifier = identifier}});
        }
        continue;
      }
      const auto& work = found->second;
      auto& observed = observed_[identifier];
      if (observed.completion.Get() >= 0 && observed.token != work.request.token) {
        const auto previous = catalog.ReadCompletion(observed.completion.Get());
        if (previous.request.identifier != identifier || previous.request.token != observed.token) StoreError();
        on_event_(Observed(previous));
        observed.completion.Close();
      }
      if (!executions_.contains(work.id)) {
        const auto version = std::pair(work.id, work.revision);
        if (observed.version != version) {
          if (work.Active() && observed.completion.Get() < 0) {
            auto token = work.request.token;
            observed.completion = catalog.Observe(token);
            observed.token = std::move(token);
          }
          on_event_(Observed(work));
          observed.version = version;
          if (!work.Active()) observed.completion.Close();
        }
      }
      active |= work.Active();
      if (work.snapshot.status == TaskStatus::Pending) pending.push_back(work.id);
    }
  }
  for (const auto& [id, schedule] : expired) StopWork(id, schedule);
  return {std::move(pending), active};
}

void OhosBackgroundTasks::Reconcile(std::span<const int> missing) {
  bool changed = false;
  {
    Catalog catalog(directory_);
    for (auto id : missing) {
      auto* work = catalog.Find(id);
      if (work && work->snapshot.status == TaskStatus::Pending) {
        Interrupt(*work, TaskStopReason::System);
        changed = true;
      }
    }
    if (changed) catalog.Save();
  }
  if (changed) static_cast<void>(Refresh());
}

void OhosBackgroundTasks::OnStart(int work_id) {
  if (executions_.contains(work_id)) return;
  static_cast<void>(Refresh());
  SystemTaskRequest request;
  Milliseconds remaining;
  {
    Catalog catalog(directory_);
    auto* work = catalog.Find(work_id);
    if (!work || work->snapshot.status != TaskStatus::Pending) return;
    request = work->request;
    remaining = Milliseconds(work->deadline - Now());
    if (!registrations_.contains(request.identifier)) {
      request.token.clear();
      work->snapshot.status = TaskStatus::Failed;
      work->error = SystemTaskError{SystemTaskErrorCode::Configuration, "HuxerUI OHOS background handler is missing"};
      ++work->revision;
      catalog.Save();
    }
  }
  if (request.token.empty()) {
    StopWork(work_id, request.schedule);
    return;
  }
  auto grant = std::make_shared<SystemTaskGrant>([weak = weak_from_this(), work_id, token = request.token](
      SystemTaskOutcome outcome, const std::optional<SystemTaskError>& error) {
    const auto retained = weak.lock();
    return retained ? retained->Finish(work_id, token, outcome, error) : SystemTaskResult<void>::Success();
  });
  grant->begin = [weak = weak_from_this(), work_id, token = request.token] {
    const auto retained = weak.lock();
    return retained && retained->Begin(work_id, token);
  };
  grant->update_progress = [weak = weak_from_this(), work_id, token = request.token](TaskProgress progress) {
    const auto retained = weak.lock();
    return retained ? retained->ReportProgress(work_id, token, progress) :
        TaskFailure<void>(SystemTaskErrorCode::Unavailable, "HuxerUI OHOS background execution ended");
  };
  executions_.emplace(work_id, Execution{request, grant});
  on_event_({.kind = SystemTaskEvent::Kind::Granted, .request = std::move(request), .grant = std::move(grant),
             .deadline = std::chrono::steady_clock::now() + remaining});
}

void OhosBackgroundTasks::OnStop(int work_id) {
  {
    Catalog catalog(directory_);
    auto* work = catalog.Find(work_id);
    if (!work || !work->Active()) return;
    Interrupt(*work, TaskStopReason::System);
    catalog.Save();
  }
  static_cast<void>(Refresh());
}

bool OhosBackgroundTasks::Begin(int work_id, std::string_view token) {
  Catalog catalog(directory_);
  auto* work = catalog.Find(work_id);
  if (!work || work->request.token != token || work->snapshot.status != TaskStatus::Pending ||
      Now() >= work->deadline) return false;
  work->snapshot.status = TaskStatus::Running;
  ++work->revision;
  catalog.Save();
  return true;
}

SystemTaskResult<void> OhosBackgroundTasks::Finish(int work_id, std::string_view token, SystemTaskOutcome outcome,
                                                  const std::optional<SystemTaskError>& error) {
  executions_.erase(work_id);
  auto result = SystemTaskResult<void>::Success();
  BackgroundTask::Schedule schedule;
  {
    Catalog catalog(directory_);
    auto* work = catalog.Find(work_id);
    if (!work || work->request.token != token || !work->Active()) return result;
    schedule = work->request.schedule;
    const bool expired = work->deadline && Now() >= work->deadline;
    if (expired) {
      outcome = SystemTaskOutcome::Interrupted;
    } else if (work->snapshot.status == TaskStatus::Stopping &&
        (outcome == SystemTaskOutcome::Retry || outcome == SystemTaskOutcome::Completed)) {
      outcome = SystemTaskOutcome::Stopped;
    }
    switch (outcome) {
      case SystemTaskOutcome::Completed: work->snapshot.status = TaskStatus::Completed; break;
      case SystemTaskOutcome::Stopped: work->snapshot.status = TaskStatus::Stopped; break;
      case SystemTaskOutcome::Failed: work->snapshot.status = TaskStatus::Failed; break;
      case SystemTaskOutcome::Interrupted:
        Interrupt(*work, expired ? TaskStopReason::Deadline : TaskStopReason::System);
        break;
      case SystemTaskOutcome::Retry: {
        work->id = catalog.Allocate();
        work->snapshot = {TaskStatus::Pending};
        work->deadline = 0;
        ++work->revision;
        catalog.Save();
        result = StartWork(work->id, work->request.schedule);
        if (!result.Succeeded()) work->snapshot.status = TaskStatus::Failed;
        break;
      }
    }
    work->error = result.Succeeded() ? error : std::optional(result.Error());
    ++work->revision;
    catalog.Save();
  }
  StopWork(work_id, schedule);
  return result;
}

SystemTaskResult<void> OhosBackgroundTasks::ReportProgress(int work_id, std::string_view token, TaskProgress progress) {
  try {
    Catalog catalog(directory_);
    auto* work = catalog.Find(work_id);
    if (!work || work->request.token != token || !work->Active()) {
      return TaskFailure<void>(SystemTaskErrorCode::Unavailable, "HuxerUI OHOS background execution ended");
    }
    work->snapshot.progress = progress;
    ++work->revision;
    catalog.Save();
    return SystemTaskResult<void>::Success();
  } catch (const std::exception& error) {
    return TaskFailure<void>(SystemTaskErrorCode::Platform, error.what());
  }
}

SystemTaskResult<void> OhosBackgroundTasks::StartWork(int work_id, const BackgroundTask::Schedule& schedule) {
  HandleScope scope(env_);
  napi_value id, delay, network, power;
  auto wait = Milliseconds::zero();
  const auto now = Clock::now();
  if (schedule.earliest_start && *schedule.earliest_start > now) {
    wait = std::chrono::ceil<Milliseconds>(*schedule.earliest_start - now);
  }
  Check(napi_create_int32(env_, work_id, &id));
  Check(napi_create_double(env_, static_cast<double>(wait.count()), &delay));
  Check(napi_get_boolean(env_, schedule.requires_network, &network));
  Check(napi_get_boolean(env_, schedule.requires_external_power, &power));
  napi_value args[]{id, delay, network, power};
  return Call("submit", args);
}

void OhosBackgroundTasks::StopWork(int work_id, const BackgroundTask::Schedule& schedule) noexcept {
  try {
    HandleScope scope(env_);
    napi_value id, network, power;
    Check(napi_create_int32(env_, work_id, &id));
    Check(napi_get_boolean(env_, schedule.requires_network, &network));
    Check(napi_get_boolean(env_, schedule.requires_external_power, &power));
    napi_value args[]{id, network, power};
    static_cast<void>(Call("withdraw", args));
  } catch (...) {
  }
}

SystemTaskResult<void> OhosBackgroundTasks::Call(const char* method, std::span<napi_value> args) {
  try {
    napi_value host, function, result;
    int status = 0;
    Check(napi_get_reference_value(env_, host_, &host));
    Check(napi_get_named_property(env_, host, method, &function));
    Check(napi_call_function(env_, host, function, args.size(), args.data(), &result));
    Check(napi_get_value_int32(env_, result, &status));
    switch (status) {
      case 0: return SystemTaskResult<void>::Success();
      case 1: return TaskFailure<void>(SystemTaskErrorCode::Unsupported,
                                      "HuxerUI OHOS background schedule requires a supported scheduler condition");
      case 2: return TaskFailure<void>(SystemTaskErrorCode::Configuration,
                                      "HuxerUI OHOS WorkScheduler extension or schedule is invalid");
      case 3: return TaskFailure<void>(SystemTaskErrorCode::Rejected, "HuxerUI OHOS WorkScheduler rejected the request");
      default: break;
    }
  } catch (...) {
    bool pending = false;
    if (napi_is_exception_pending(env_, &pending) == napi_ok && pending) {
      napi_value exception;
      napi_get_and_clear_last_exception(env_, &exception);
    }
  }
  return TaskFailure<void>(SystemTaskErrorCode::Platform, "HuxerUI could not call the OHOS work scheduler host");
}

} // namespace huxerui::detail
