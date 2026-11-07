#include "runtime_test_support.h"

#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "application/system_task_internal.h"

namespace huxerui::test {
namespace {

using detail::SystemTaskEvent;
using detail::SystemTaskGrant;
using detail::SystemTaskKind;
using detail::SystemTaskOutcome;
using detail::SystemTaskRequest;

class TaskTransport final : public detail::ProcessLocalSystemTasks {
public:
  SystemTaskCapabilities Capabilities(SystemTaskKind kind) const noexcept override {
    return {true, kind == SystemTaskKind::Background};
  }

  void Install(std::span<const detail::SystemTaskRegistration> registrations, EventHandler on_event) override {
    ProcessLocalSystemTasks::Install(registrations, std::move(on_event));
    for (const auto& registration : registrations) {
      if (registration.kind == SystemTaskKind::Background) {
        on_event_({SystemTaskEvent::Kind::Reconciled, recovered.value_or(
            SystemTaskRequest{.identifier = registration.identifier})});
      }
    }
  }

  SystemTaskResult<void> Submit(const SystemTaskRequest& request) override {
    submitted.push_back(request);
    return SystemTaskResult<void>::Success();
  }

  SystemTaskResult<void> Withdraw(const SystemTaskRequest& request) override {
    withdrawals.push_back(request.token);
    if (reject_withdrawal) {
      return detail::TaskFailure<void>(SystemTaskErrorCode::Platform, "HuxerUI test withdrawal failed");
    }
    return SystemTaskResult<void>::Success();
  }

  std::shared_ptr<SystemTaskGrant> Grant(const SystemTaskRequest& request) {
    auto grant = std::make_shared<SystemTaskGrant>([this](SystemTaskOutcome outcome) {
      outcomes.push_back(outcome);
      return SystemTaskResult<void>::Success();
    });
    on_event_({SystemTaskEvent::Kind::Granted, request, grant});
    return grant;
  }

  void Revoke(const SystemTaskRequest& request, const std::shared_ptr<SystemTaskGrant>& grant) {
    grant->Revoke();
    on_event_({SystemTaskEvent::Kind::Revoked, request, grant});
  }

  std::optional<SystemTaskRequest> recovered;
  std::vector<SystemTaskRequest> submitted;
  std::vector<std::string> withdrawals;
  std::vector<SystemTaskOutcome> outcomes;
  bool reject_withdrawal = false;
};

class TaskPlatform final : public TestPlatform {
public:
  std::shared_ptr<TaskTransport> transport = std::make_shared<TaskTransport>();

protected:
  std::shared_ptr<detail::SystemTaskTransport> CreateSystemTaskTransport() override { return transport; }
};

View EmptyRoot() { return Text("System tasks"); }

Task<BackgroundTask::Result> CompleteBackground(BackgroundTask::Context) {
  co_return BackgroundTask::Result::Completed;
}

Task<void> CompleteContinued(ContinuedTask::Context) { co_return; }

struct TaskViewModel {
  ContinuedTask task;
};

View ObserveContinued() {
  auto model = UseService<TaskViewModel>();
  const auto state = model->task.GetState("export");
  return Text(state ? "Present" : "Absent");
}

} // namespace

TEST_CASE("System tasks validate one installation list before invoking application hooks") {
  REQUIRE_THROWS_AS(BackgroundTask("", CompleteBackground), std::invalid_argument);
  REQUIRE_THROWS_AS(ContinuedTask("test", {}), std::invalid_argument);
  REQUIRE_THROWS_AS(Application(EmptyRoot, {.system_task_stop_timeout = 0s}), std::invalid_argument);

  const BackgroundTask background("test.duplicate", CompleteBackground);
  const ContinuedTask continued("test.duplicate", CompleteContinued);
  int hooks = 0;
  TaskPlatform platform;
  const Application app(EmptyRoot, {.application_hooks = {[&](ApplicationContext&) { ++hooks; }},
                                    .system_tasks = {background, continued}});
  REQUIRE_THROWS_AS(RuntimeLifetime(app, platform), std::invalid_argument);
  REQUIRE(hooks == 0);
}

TEST_CASE("Continued tasks admit independent requests and retain the selected terminal identity") {
  TaskPlatform platform;
  std::vector<std::string> started;
  const ContinuedTask continued("test.export", [&](ContinuedTask::Context context) -> Task<void> {
    started.push_back(context.InstanceId());
    REQUIRE(context.ReportProgress({1, 2}).Succeeded());
    co_await Delay(1s);
  });
  const Application app(EmptyRoot, {.system_tasks = {continued}});
  RuntimeLifetime runtime(app, platform, std::nullopt);

  REQUIRE(continued.Start("a", {"Export", "First"}).Value() == SubmissionStatus::Accepted);
  REQUIRE(continued.Start("a", {"Replacement", ""}).Value() == SubmissionStatus::NoChange);
  REQUIRE(continued.Start("b", {"Export", "Second"}).Value() == SubmissionStatus::Accepted);
  REQUIRE(platform.transport->submitted.size() == 2);
  REQUIRE(continued.GetState("a")->Status() == TaskStatus::Pending);
  REQUIRE(continued.GetState("a")->Presentation()->subtitle == StringVariant("First"));
  auto selected = continued.WaitAsync("a");
  auto first = platform.transport->Grant(platform.transport->submitted[0]);
  auto second = platform.transport->Grant(platform.transport->submitted[1]);
  platform.RunPlatformModuleTasks();
  REQUIRE(started == std::vector<std::string>{"a", "b"});
  REQUIRE(continued.GetState("a")->Progress() == TaskProgress{1, 2});
  platform.AdvanceTime(1.0);
  platform.RunPlatformModuleTasks();
  const auto old = *continued.GetState("a");
  REQUIRE(old.Status() == TaskStatus::Completed);
  REQUIRE(continued.Start("a", {"Fresh export", ""}).Value() == SubmissionStatus::Accepted);
  REQUIRE_FALSE(continued.Forget("a", old));

  std::optional<TaskState> observed;
  UseApplicationTaskScope().Launch([wait = std::move(selected), &observed]() mutable -> Task<void> {
    auto result = co_await std::move(wait);
    REQUIRE(result.Succeeded());
    observed = result.Value();
  });
  platform.RunPlatformModuleTasks();
  REQUIRE(observed == old);
  REQUIRE(continued.GetState("a")->Status() == TaskStatus::Pending);
  REQUIRE(continued.Forget("b", *continued.GetState("b")));
  REQUIRE_FALSE(continued.GetState("b"));
  REQUIRE(platform.transport->outcomes == std::vector{SystemTaskOutcome::Completed, SystemTaskOutcome::Completed});
}

TEST_CASE("Task observation refreshes each window when an instance appears and is forgotten") {
  TaskPlatform platform;
  const ContinuedTask task("test.ui", CompleteContinued);
  auto model = std::make_shared<TaskViewModel>(TaskViewModel{task});
  const Application app(ObserveContinued, {.show_debug_overlay = false,
      .application_hooks = {[model](ApplicationContext& context) { context.Provide(model); }},
      .system_tasks = {task}});
  RuntimeLifetime runtime(app, platform, std::nullopt);
  TestPlatform first;
  TestPlatform second;
  UiWindow first_window(runtime, first);
  UiWindow second_window(runtime, second);
  first_window.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  second_window.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  REQUIRE(FindText(first_window.BuildFrame(), "Absent"));
  REQUIRE(FindText(second_window.BuildFrame(), "Absent"));
  REQUIRE(task.Start("export", {"Export", ""}).Succeeded());
  REQUIRE(FindText(first_window.BuildFrame(), "Present"));
  REQUIRE(FindText(second_window.BuildFrame(), "Present"));
  auto grant = platform.transport->Grant(platform.transport->submitted.front());
  platform.RunPlatformModuleTasks();
  REQUIRE(task.Forget("export", *task.GetState("export")));
  REQUIRE(FindText(first_window.BuildFrame(), "Absent"));
  REQUIRE(FindText(second_window.BuildFrame(), "Absent"));
}

TEST_CASE("Background scheduling is lazy, retains retries, and records failed withdrawal") {
  TaskPlatform platform;
  int attempts = 0;
  const BackgroundTask task("test.refresh", [&](BackgroundTask::Context) -> Task<BackgroundTask::Result> {
    ++attempts;
    co_return attempts == 1 ? BackgroundTask::Result::Retry : BackgroundTask::Result::Completed;
  });
  const Application app(EmptyRoot, {.system_tasks = {task}});
  RuntimeLifetime runtime(app, platform, std::nullopt);
  REQUIRE(task.GetState().Status() == TaskStatus::Unknown);
  platform.RunPlatformModuleTasks();
  auto lazy = task.ScheduleAsync({.requires_network = true});
  REQUIRE(platform.transport->submitted.empty());
  REQUIRE(task.GetState().Status() == TaskStatus::Idle);
  auto tasks = UseApplicationTaskScope();
  tasks.Launch([submission = std::move(lazy)]() mutable -> Task<void> {
    const auto result = co_await std::move(submission);
    REQUIRE(result.Value() == SubmissionStatus::Accepted);
  });
  platform.RunPlatformModuleTasks();
  REQUIRE(task.GetCapabilities().can_launch_process);
  REQUIRE(task.GetState().Status() == TaskStatus::Pending);
  tasks.Launch([&]() -> Task<void> {
    const auto identical = co_await task.ScheduleAsync({.requires_network = true});
    const auto conflicting = co_await task.ScheduleAsync();
    REQUIRE(identical.Value() == SubmissionStatus::NoChange);
    REQUIRE(conflicting.Error().code == SystemTaskErrorCode::Busy);
  });
  platform.RunPlatformModuleTasks();
  platform.transport->reject_withdrawal = true;
  REQUIRE(task.RequestStop().Error().code == SystemTaskErrorCode::Platform);
  REQUIRE(task.GetState().Status() == TaskStatus::Pending);
  REQUIRE(task.GetState().Error()->code == SystemTaskErrorCode::Platform);

  std::optional<TaskState> observed;
  tasks.Launch([&]() -> Task<void> { observed = (co_await task.WaitAsync()).Value(); });
  auto first = platform.transport->Grant(platform.transport->submitted.front());
  platform.RunPlatformModuleTasks();
  REQUIRE(task.GetState().Status() == TaskStatus::Pending);
  REQUIRE_FALSE(observed);
  REQUIRE(platform.transport->outcomes == std::vector{SystemTaskOutcome::Retry});

  auto second = platform.transport->Grant(platform.transport->submitted.front());
  platform.RunPlatformModuleTasks();
  REQUIRE(attempts == 2);
  REQUIRE(observed->Status() == TaskStatus::Completed);
}

TEST_CASE("Cooperative stop retires a callback before bounded asynchronous cleanup") {
  TaskPlatform platform;
  std::optional<CallbackCompletion<void>> work;
  std::optional<CallbackCompletion<void>> cleanup;
  std::optional<OngoingTask::Context> stale_context;
  int cancellations = 0;
  bool cleaned = false;
  const OngoingTask task("test.session", [&](OngoingTask::Context context) -> Task<void> {
    stale_context = context;
    try {
      co_await AwaitCallback<void>([&](auto completion) {
        work = std::move(completion);
        return [&] { ++cancellations; };
      });
      FAIL("Stopped work resumed as a success");
    } catch (const TaskStopped&) {
      REQUIRE(context.StopReason() == TaskStopReason::Requested);
    }
    auto close = AwaitCallback<void>([&](auto completion) {
      cleanup = std::move(completion);
      return std::function<void()>{};
    });
    co_await context.Cleanup(std::move(close));
    REQUIRE(context.StopToken().stop_requested());
    cleaned = true;
  });
  const Application app(EmptyRoot, {.system_tasks = {task}});
  RuntimeLifetime runtime(app, platform, std::nullopt);
  REQUIRE(task.Start({"Session", "Connecting"}).Succeeded());
  auto grant = platform.transport->Grant(platform.transport->submitted.front());
  platform.RunPlatformModuleTasks();
  REQUIRE(work);
  REQUIRE(task.RequestStop().Value() == StopRequestStatus::Requested);
  platform.RunPlatformModuleTasks();
  REQUIRE(task.GetState().Status() == TaskStatus::Stopping);
  REQUIRE(cancellations == 1);
  REQUIRE(cleanup);
  work->Complete();
  platform.RunPlatformModuleTasks();
  REQUIRE_FALSE(cleaned);
  cleanup->Complete();
  platform.RunPlatformModuleTasks();
  REQUIRE(cleaned);
  REQUIRE(task.GetState().Status() == TaskStatus::Stopped);
  REQUIRE(stale_context->UpdatePresentation({"Stale", ""}).Error().code == SystemTaskErrorCode::Unavailable);
  REQUIRE(platform.transport->outcomes == std::vector{SystemTaskOutcome::Stopped});
}

TEST_CASE("A platform allowance revoked before handler dispatch retires without running it") {
  TaskPlatform platform;
  int executions = 0;
  const BackgroundTask task("test.cold", [&](BackgroundTask::Context) -> Task<BackgroundTask::Result> {
    ++executions;
    co_return BackgroundTask::Result::Completed;
  });
  const SystemTaskRequest request{.identifier = std::string(task.Identifier()), .token = "cold-request"};
  platform.transport->recovered = request;
  const Application app(EmptyRoot, {.system_tasks = {task}});
  RuntimeLifetime runtime(app, platform, std::nullopt);
  auto grant = platform.transport->Grant(request);
  platform.transport->Revoke(request, grant);
  platform.RunPlatformModuleTasks();
  REQUIRE(executions == 0);
  REQUIRE(task.GetState().Status() == TaskStatus::Interrupted);
  REQUIRE(platform.transport->outcomes.empty());
}

TEST_CASE("Revocation suppresses an already queued continuation, including asynchronous cleanup") {
  bool during_cleanup = false;
  SECTION("Ordinary handler work") {}
  SECTION("Bounded cleanup") { during_cleanup = true; }

  TaskPlatform platform;
  std::optional<CallbackCompletion<void>> completion;
  std::optional<TaskState> observed;
  bool resumed = false;
  bool stop_observed = false;
  const BackgroundTask task("test.queued", [&](BackgroundTask::Context context) -> Task<BackgroundTask::Result> {
    std::stop_callback on_stop(context.StopToken(), [&] { stop_observed = true; });
    auto wait = AwaitCallback<void>([&](auto pending) {
      completion = std::move(pending);
      return std::function<void()>{};
    });
    if (during_cleanup) co_await context.Cleanup(std::move(wait));
    else co_await std::move(wait);
    resumed = true;
    co_return BackgroundTask::Result::Retry;
  });
  const SystemTaskRequest request{.identifier = std::string(task.Identifier()), .token = "queued-request"};
  platform.transport->recovered = request;
  const Application app(EmptyRoot, {.system_tasks = {task}});
  RuntimeLifetime runtime(app, platform, std::nullopt);
  auto grant = platform.transport->Grant(request);
  UseApplicationTaskScope().Launch([&]() -> Task<void> { observed = (co_await task.WaitAsync()).Value(); });
  platform.RunPlatformModuleTasks();
  REQUIRE(completion);

  completion->Complete();
  platform.transport->Revoke(request, grant);
  platform.RunPlatformModuleTasks();

  REQUIRE_FALSE(resumed);
  REQUIRE(stop_observed);
  REQUIRE(task.GetState().Status() == TaskStatus::Interrupted);
  REQUIRE(observed);
  REQUIRE(observed->Status() == TaskStatus::Interrupted);
  REQUIRE(platform.transport->outcomes.empty());
}

TEST_CASE("Revocation before handler return cannot publish a retry") {
  bool requested = false;
  SECTION("Platform revocation") {}
  SECTION("An earlier cooperative stop retains its cause") { requested = true; }

  TaskPlatform platform;
  std::shared_ptr<SystemTaskGrant> grant;
  const SystemTaskRequest request{.identifier = "test.return", .token = "return-request"};
  const BackgroundTask task(request.identifier, [&](BackgroundTask::Context) -> Task<BackgroundTask::Result> {
    if (requested) REQUIRE(task.RequestStop().Value() == StopRequestStatus::Requested);
    platform.transport->Revoke(request, grant);
    co_return BackgroundTask::Result::Retry;
  });
  platform.transport->recovered = request;
  const Application app(EmptyRoot, {.system_tasks = {task}});
  RuntimeLifetime runtime(app, platform, std::nullopt);
  grant = platform.transport->Grant(request);
  platform.RunPlatformModuleTasks();

  REQUIRE(task.GetState().Status() == TaskStatus::Interrupted);
  REQUIRE(task.GetState().StopReason() == (requested ? TaskStopReason::Requested : TaskStopReason::System));
  REQUIRE(platform.transport->outcomes.empty());
}

TEST_CASE("Forced retirement signals the system stop token before canceling suspended work") {
  TaskPlatform platform;
  std::stop_token stop_token;
  bool canceled = false;
  bool stop_observed = false;
  const OngoingTask task("test.retire", [&](OngoingTask::Context context) -> Task<void> {
    stop_token = context.StopToken();
    co_await AwaitCallback<void>([&](auto) {
      return [&] {
        canceled = true;
        stop_observed = stop_token.stop_requested();
      };
    });
  });
  const Application app(EmptyRoot, {.system_tasks = {task}});
  {
    RuntimeLifetime runtime(app, platform, std::nullopt);
    REQUIRE(task.Start({"Session", ""}).Succeeded());
    auto grant = platform.transport->Grant(platform.transport->submitted.front());
    platform.RunPlatformModuleTasks();
    REQUIRE_FALSE(canceled);
  }

  REQUIRE(canceled);
  REQUIRE(stop_observed);
  REQUIRE(stop_token.stop_requested());
  REQUIRE(platform.transport->outcomes == std::vector{SystemTaskOutcome::Interrupted});
}

TEST_CASE("Orderly shutdown keeps services and timers available only through the system stop budget") {
  TaskPlatform platform;
  bool cleanup_started = false;
  bool cleanup_completed = false;
  const OngoingTask task("test.shutdown", [&](OngoingTask::Context context) -> Task<void> {
    try {
      co_await Delay(1h);
    } catch (const TaskStopped&) {
      REQUIRE(context.StopReason() == TaskStopReason::Shutdown);
    }
    REQUIRE(*UseService<int>() == 42);
    cleanup_started = true;
    co_await context.Cleanup(Delay(1h));
    cleanup_completed = true;
  });
  const Application app(EmptyRoot, {.application_hooks = {[](ApplicationContext& context) {
                                    context.Provide(std::make_shared<int>(42));
                                  }},
                                  .system_tasks = {task},
                                  .system_task_stop_timeout = 25ms});
  RuntimeLifetime runtime(app, platform, std::nullopt);
  auto ordinary = UseApplicationTaskScope();
  REQUIRE(task.Start({"Session", ""}).Succeeded());
  auto grant = platform.transport->Grant(platform.transport->submitted.front());
  platform.RunPlatformModuleTasks();
  runtime.RequestShutdown();
  platform.RunPlatformModuleTasks();
  REQUIRE(cleanup_started);
  REQUIRE_FALSE(cleanup_completed);
  REQUIRE(platform.application_quit_requests == 0);
  REQUIRE_THROWS_AS(ordinary.Launch(Delay(0ms)), std::logic_error);
  platform.AdvanceTime(0.03);
  platform.RunPlatformModuleTasks();
  REQUIRE_FALSE(cleanup_completed);
  REQUIRE(platform.application_quit_requests == 1);
  REQUIRE(platform.transport->outcomes == std::vector{SystemTaskOutcome::Interrupted});
}

} // namespace huxerui::test
