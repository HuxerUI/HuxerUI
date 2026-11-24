#pragma once

#include <napi/native_api.h>

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "application/system_task_internal.h"
#include "io/file_internal.h"

namespace huxerui::detail {

/// Owns this Runtime's view of WorkScheduler requests stored in the application's private directory.
class OhosBackgroundTasks final : public std::enable_shared_from_this<OhosBackgroundTasks> {
public:
  OhosBackgroundTasks(napi_env env, napi_value host, std::string directory);
  ~OhosBackgroundTasks();

  /// Starts the work budget before cold initialization and checks for a previously persisted withdrawal.
  static bool PrepareDelivery(std::string_view directory, int work_id);
  void Install(std::span<const SystemTaskRegistration> registrations, SystemTaskTransport::EventHandler on_event);
  SystemTaskResult<void> Submit(const SystemTaskRequest& request);
  SystemTaskResult<void> Withdraw(const SystemTaskRequest& request);
  void Disconnect() noexcept;

  /// Returns pending work IDs for scheduler reconciliation and whether observation should continue.
  std::pair<std::vector<int>, bool> Refresh();
  void Reconcile(std::span<const int> missing);
  void OnStart(int work_id);
  void OnStop(int work_id);

private:
  struct Observation {
    std::pair<int, std::uint64_t> version{};
    std::string token;
    ReferenceDescriptor completion{};
  };

  struct Execution {
    SystemTaskRequest request;
    std::weak_ptr<SystemTaskGrant> grant;
    bool stop_sent = false;
  };

  bool Begin(int work_id, std::string_view token);
  SystemTaskResult<void> Finish(int work_id, std::string_view token, SystemTaskOutcome outcome,
                                const std::optional<SystemTaskError>& error);
  SystemTaskResult<void> ReportProgress(int work_id, std::string_view token, TaskProgress progress);
  SystemTaskResult<void> StartWork(int work_id, const BackgroundTask::Schedule& schedule);
  void StopWork(int work_id, const BackgroundTask::Schedule& schedule) noexcept;
  SystemTaskResult<void> Call(const char* method, std::span<napi_value> args);

  napi_env env_;
  napi_ref host_ = nullptr;
  std::string directory_;
  SystemTaskTransport::EventHandler on_event_;
  std::set<std::string, std::less<>> registrations_;
  std::map<std::string, Observation, std::less<>> observed_;
  std::map<int, Execution> executions_;
};

} // namespace huxerui::detail
