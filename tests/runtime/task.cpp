#include "runtime_test_support.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stop_token>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "runtime/task_internal.h"

namespace huxerui::test {

namespace {

static_assert(std::move_constructible<Task<int>>);
static_assert(!std::copy_constructible<Task<int>>);
static_assert(std::copy_constructible<TaskScope>);
static_assert(std::copy_constructible<TaskHandle>);
static_assert(std::copy_constructible<WorkerSequence>);
static_assert(std::same_as<decltype(RunWorker([] { return 1; })), Task<int>>);
static_assert(std::same_as<decltype(WorkerSequence{}.Run([](std::stop_token) { return 1; })), Task<int>>);

class ThreadSafeTaskQueue {
public:
  UIThreadDispatcher Dispatcher() {
    return [this](std::function<void()> callback) {
      {
        std::scoped_lock lock(mutex_);
        callbacks_.push_back(std::move(callback));
      }
      condition_.notify_one();
    };
  }

  void Drain() {
    for (;;) {
      std::vector<std::function<void()>> callbacks;
      {
        std::scoped_lock lock(mutex_);
        callbacks = std::move(callbacks_);
      }
      if (callbacks.empty()) {
        return;
      }
      for (auto& callback : callbacks) {
        callback();
      }
    }
  }

  [[nodiscard]] bool WaitForWork() {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, std::chrono::seconds(5), [this] { return !callbacks_.empty(); });
  }

private:
  std::mutex mutex_;
  std::condition_variable condition_;
  std::vector<std::function<void()>> callbacks_;
};

template <class Predicate> void DrainUntil(ThreadSafeTaskQueue& queue, Predicate predicate) {
  while (!predicate()) {
    REQUIRE(queue.WaitForWork());
    queue.Drain();
  }
}

class WorkerGate {
public:
  ~WorkerGate() {
    Release();
  }

  void EnterAndWait() {
    std::unique_lock lock(mutex_);
    ++started_;
    condition_.notify_all();
    condition_.wait(lock, [this] { return released_; });
    ++departed_;
    condition_.notify_all();
  }

  [[nodiscard]] bool WaitForStarted(std::size_t count) {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, std::chrono::seconds(5), [this, count] { return started_ >= count; });
  }

  [[nodiscard]] bool WaitForDeparted(std::size_t count) {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, std::chrono::seconds(5), [this, count] { return departed_ >= count; });
  }

  [[nodiscard]] std::size_t Started() const {
    std::scoped_lock lock(mutex_);
    return started_;
  }

  void Release() {
    {
      std::scoped_lock lock(mutex_);
      released_ = true;
    }
    condition_.notify_all();
  }

private:
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::size_t started_ = 0;
  std::size_t departed_ = 0;
  bool released_ = false;
};

class StopAwareWorkerGate {
public:
  void EnterAndWait(std::stop_token stop_token) {
    std::stop_callback stop_callback(stop_token, [this] {
      {
        std::scoped_lock lock(mutex_);
        stopped_ = true;
      }
      condition_.notify_all();
    });
    std::unique_lock lock(mutex_);
    started_ = true;
    condition_.notify_all();
    condition_.wait(lock, [this] { return stopped_; });
    cleaned_ = true;
    condition_.notify_all();
  }

  [[nodiscard]] bool WaitForStarted() {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, std::chrono::seconds(5), [this] { return started_; });
  }

  [[nodiscard]] bool WaitForCleanup() {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, std::chrono::seconds(5), [this] { return cleaned_; });
  }

  [[nodiscard]] bool Cleaned() const {
    std::scoped_lock lock(mutex_);
    return cleaned_;
  }

private:
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  bool started_ = false;
  bool stopped_ = false;
  bool cleaned_ = false;
};

struct ManualSuspensionState {
  void Suspend(std::weak_ptr<detail::TaskExecution> execution, std::coroutine_handle<> continuation) {
    std::scoped_lock lock(mutex);
    execution_ = std::move(execution);
    continuation_ = continuation;
  }

  void Resume() {
    std::weak_ptr<detail::TaskExecution> execution;
    std::coroutine_handle<> continuation;
    {
      std::scoped_lock lock(mutex);
      execution = execution_;
      continuation = std::exchange(continuation_, {});
    }
    detail::ResumeTask(execution, continuation);
  }

  void Cancel() noexcept {
    std::scoped_lock lock(mutex);
    if (continuation_) {
      continuation_ = {};
      canceled = true;
    }
  }

  std::mutex mutex;
  std::weak_ptr<detail::TaskExecution> execution_;
  std::coroutine_handle<> continuation_;
  bool canceled = false;
};

class ManualAwaiter {
public:
  explicit ManualAwaiter(std::shared_ptr<ManualSuspensionState> state) : state_(std::move(state)) {}

  ~ManualAwaiter() {
    state_->Cancel();
  }

  [[nodiscard]] bool await_ready() const noexcept {
    return false;
  }

  template <class Promise> void await_suspend(std::coroutine_handle<Promise> continuation) {
    state_->Suspend(detail::TaskExecutionFor(continuation), continuation);
  }

  void await_resume() const noexcept {}

private:
  std::shared_ptr<ManualSuspensionState> state_;
};

TaskScope captured_task_scope;
TaskScope captured_child_task_scope;
State<int> task_value;
State<int> task_recompose_trigger;
State<bool> task_child_visible;
State<bool> post_child_visible;
TaskScope captured_post_child_scope;
std::shared_ptr<ManualSuspensionState> child_suspension;
int child_task_completions = 0;
bool child_cleanup_preceded_scope_cancellation = false;
StateList<int> task_keyed_items;
std::unordered_map<int, std::shared_ptr<ManualSuspensionState>> keyed_suspensions;
ScrollController task_virtual_scroll;
std::shared_ptr<ManualSuspensionState> virtual_suspension;
int composition_task_starts = 0;
int composition_post_runs = 0;

int AddWorkerValues(int left, int right) {
  return left + right;
}

Task<int> TaskValue(int value) {
  co_return value;
}

Task<int> FailingTaskValue() {
  throw std::runtime_error("task failed");
  co_return 0;
}

Task<void> DirectTask(int* starts) {
  ++*starts;
  co_return;
}

Task<void> DelayedStateTask(State<int> state, std::thread::id* resumed_thread) {
  co_await Delay(3ms);
  *resumed_thread = std::this_thread::get_id();
  state = 7;
}

Task<void> SuspendedTask(std::shared_ptr<ManualSuspensionState> suspension, int* completions = nullptr) {
  co_await ManualAwaiter(std::move(suspension));
  if (completions != nullptr) {
    ++*completions;
  }
}

View TaskScopeApp() {
  captured_task_scope = UseTaskScope();
  task_value = UseState(0);
  task_recompose_trigger = UseState(0);
  return Text::Format("{}:{}", task_value, task_recompose_trigger);
}

View TaskChild() {
  captured_child_task_scope = UseTaskScope();
  Lifecycle([tasks = captured_child_task_scope] {
    tasks.Launch(SuspendedTask(child_suspension, &child_task_completions));
    return [] { child_cleanup_preceded_scope_cancellation = !child_suspension->canceled; };
  });
  return Text("child");
}

View TaskUnmountApp() {
  task_child_visible = UseState(true);
  if (task_child_visible) {
    return Scope(TaskChild);
  }
  return Text("removed");
}

View TaskKeyedApp() {
  task_keyed_items = UseStateList<int>({1, 2});
  return Column {
    ForEach(task_keyed_items, [](int item) {
      return Scope([item] {
        auto tasks = UseTaskScope();
        Lifecycle([tasks, item] { tasks.Launch(SuspendedTask(keyed_suspensions.at(item))); });
        return Text::Format("{}", item);
      }).Key(item);
    }),
  };
}

View TaskVirtualItem(std::size_t index) {
  return Scope([index] {
    if (index == 0) {
      auto tasks = UseTaskScope();
      Lifecycle([tasks] { tasks.Launch(SuspendedTask(virtual_suspension)); });
    }
    return Text::Format("{}", index);
  }).Key(index);
}

View TaskVirtualListApp() {
  auto scroll = UseScrollController();
  task_virtual_scroll = scroll;
  return VirtualList(std::size_t{100}, TaskVirtualItem)
      .Controller(scroll)
      .ItemExtent(20.0F)
      .CacheExtent(0.0F);
}

View IllegalTaskLaunchApp() {
  auto tasks = UseTaskScope();
  tasks.Launch(DirectTask(&composition_task_starts));
  return Text("invalid");
}

View PostChild() {
  captured_post_child_scope = UseTaskScope();
  return Text("child");
}

View PostUnmountApp() {
  post_child_visible = UseState(true);
  if (post_child_visible) {
    return Scope(PostChild);
  }
  return Text("removed");
}

View IllegalTaskPostApp() {
  auto tasks = UseTaskScope();
  tasks.Post([] { ++composition_post_runs; });
  return Text("invalid");
}

} // namespace

TEST_CASE("TaskScopeLaunchIsLazyAndComposesNestedTaskValuesAndExceptions") {
  captured_task_scope = {};
  task_value = State<int>{};
  int direct_starts = 0;
  int factory_starts = 0;
  TestPlatform platform;
  Runtime runtime(TaskScopeApp, platform);

  runtime.BuildFrame();
  captured_task_scope.Launch(DirectTask(&direct_starts));
  captured_task_scope.Launch([state = task_value, &factory_starts]() -> Task<void> {
    ++factory_starts;
    state = co_await TaskValue(41);
    try {
      static_cast<void>(co_await FailingTaskValue());
    } catch (const std::runtime_error&) {
      state += 1;
    }
  });

  REQUIRE(direct_starts == 0);
  REQUIRE(factory_starts == 0);
  REQUIRE(task_value.Get() == 0);

  platform.RunPlatformModuleTasks();

  REQUIRE(direct_starts == 1);
  REQUIRE(factory_starts == 1);
  REQUIRE(task_value.Get() == 42);
}

TEST_CASE("DelayIsLazyAndResumesAfterItsDeadlineOnTheOwningUIThread") {
  captured_task_scope = {};
  task_value = State<int>{};
  TestPlatform platform;
  Runtime runtime(TaskScopeApp, platform);
  runtime.BuildFrame();
  const std::thread::id ui_thread = std::this_thread::get_id();

  std::thread::id resumed_thread;
  Task<void> delayed = DelayedStateTask(task_value, &resumed_thread);
  REQUIRE(platform.requested_deadlines.empty());

  captured_task_scope.Launch(std::move(delayed));
  REQUIRE(platform.requested_deadlines.empty());
  platform.RunPlatformModuleTasks();
  REQUIRE(platform.requested_deadlines.back() == Catch::Approx(0.003));

  platform.AdvanceTime(0.002);
  runtime.BuildFrame();
  REQUIRE(task_value.Get() == 0);

  platform.AdvanceTime(0.001);
  runtime.BuildFrame();
  REQUIRE(task_value.Get() == 7);
  REQUIRE(resumed_thread == ui_thread);
}

TEST_CASE("DelayOrdersDeadlinesAndDefersZeroDurationChains") {
  captured_task_scope = {};
  TestPlatform platform;
  Runtime runtime(TaskScopeApp, platform);
  runtime.BuildFrame();
  std::vector<int> completions;

  captured_task_scope.Launch([&completions]() -> Task<void> {
    co_await Delay(3s);
    completions.push_back(3);
  });
  captured_task_scope.Launch([&completions]() -> Task<void> {
    co_await Delay(3ms);
    completions.push_back(1);
    co_await Delay(0ms);
    completions.push_back(2);
  });
  platform.RunPlatformModuleTasks();

  platform.AdvanceTime(0.003);
  runtime.BuildFrame();
  REQUIRE(completions == std::vector<int>{1});

  runtime.BuildFrame();
  REQUIRE(completions == std::vector<int>{1, 2});

  platform.AdvanceTime(2.997);
  runtime.BuildFrame();
  REQUIRE(completions == std::vector<int>{1, 2, 3});
}

TEST_CASE("DelayCancellationPreventsLaterResumption") {
  captured_task_scope = {};
  TestPlatform platform;
  Runtime runtime(TaskScopeApp, platform);
  runtime.BuildFrame();
  int completions = 0;

  TaskHandle delayed = captured_task_scope.Launch([&completions]() -> Task<void> {
    co_await Delay(3s);
    ++completions;
  });
  platform.RunPlatformModuleTasks();
  delayed.Cancel();

  platform.AdvanceTime(3.0);
  runtime.BuildFrame();
  REQUIRE(completions == 0);
}

TEST_CASE("DelayRejectsInvalidDurations") {
  REQUIRE_THROWS_AS(Delay(std::chrono::duration<double>{-1.0}), std::invalid_argument);
  REQUIRE_THROWS_AS(
      Delay(std::chrono::duration<double>{std::numeric_limits<double>::infinity()}),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      Delay(std::chrono::duration<double>{std::numeric_limits<double>::quiet_NaN()}),
      std::invalid_argument
  );
}

TEST_CASE("TaskHandleCancellationIsIndividualAndIgnoredHandlesRemainScopeOwned") {
  captured_task_scope = {};
  TestPlatform platform;
  Runtime runtime(TaskScopeApp, platform);
  runtime.BuildFrame();

  int canceled_starts = 0;
  TaskHandle canceled = captured_task_scope.Launch(DirectTask(&canceled_starts));
  canceled.Cancel();
  canceled.Cancel();

  auto suspension = std::make_shared<ManualSuspensionState>();
  int completions = 0;
  captured_task_scope.Launch(SuspendedTask(suspension, &completions));
  platform.RunPlatformModuleTasks();

  REQUIRE(canceled_starts == 0);
  REQUIRE(completions == 0);
  REQUIRE_FALSE(suspension->canceled);

  suspension->Resume();
  platform.RunPlatformModuleTasks();
  REQUIRE(completions == 1);
}

TEST_CASE("TaskScopeSurvivesCompatibleRecomposition") {
  captured_task_scope = {};
  TestPlatform platform;
  Runtime runtime(TaskScopeApp, platform);
  runtime.BuildFrame();

  auto suspension = std::make_shared<ManualSuspensionState>();
  int completions = 0;
  captured_task_scope.Launch(SuspendedTask(suspension, &completions));
  platform.RunPlatformModuleTasks();

  task_recompose_trigger = 1;
  runtime.BuildFrame();
  REQUIRE_FALSE(suspension->canceled);

  suspension->Resume();
  platform.RunPlatformModuleTasks();
  REQUIRE(completions == 1);
}

TEST_CASE("TaskScopeFollowsKeyedMovementAndClosesOnlyRemovedChildren") {
  task_keyed_items = {};
  keyed_suspensions = {
      {1, std::make_shared<ManualSuspensionState>()},
      {2, std::make_shared<ManualSuspensionState>()},
  };
  TestPlatform platform;
  Runtime runtime(TaskKeyedApp, platform);

  runtime.BuildFrame();
  platform.RunPlatformModuleTasks();
  task_keyed_items.Move(1, 0);
  runtime.BuildFrame();

  REQUIRE_FALSE(keyed_suspensions.at(1)->canceled);
  REQUIRE_FALSE(keyed_suspensions.at(2)->canceled);

  task_keyed_items.Erase(0);
  runtime.BuildFrame();
  REQUIRE_FALSE(keyed_suspensions.at(1)->canceled);
  REQUIRE(keyed_suspensions.at(2)->canceled);
}

TEST_CASE("VirtualItemEvictionClosesItsTaskScope") {
  virtual_suspension = std::make_shared<ManualSuspensionState>();
  TestPlatform platform;
  Runtime runtime(TaskVirtualListApp, platform);
  runtime.SetWindowMetrics({.viewport = {100.0F, 40.0F}});

  runtime.BuildFrame();
  platform.RunPlatformModuleTasks();
  REQUIRE_FALSE(virtual_suspension->canceled);

  REQUIRE(task_virtual_scroll.ScrollToItem(50));
  runtime.BuildFrame();
  REQUIRE(virtual_suspension->canceled);
}

TEST_CASE("UnmountClosesTaskScopeAfterLifecycleCleanupAndRejectsLaterLaunch") {
  captured_child_task_scope = {};
  child_suspension = std::make_shared<ManualSuspensionState>();
  child_task_completions = 0;
  child_cleanup_preceded_scope_cancellation = false;
  TestPlatform platform;
  Runtime runtime(TaskUnmountApp, platform);

  runtime.BuildFrame();
  platform.RunPlatformModuleTasks();
  REQUIRE_FALSE(child_suspension->canceled);

  task_child_visible = false;
  runtime.BuildFrame();

  REQUIRE(child_suspension->canceled);
  REQUIRE(child_cleanup_preceded_scope_cancellation);
  REQUIRE(child_task_completions == 0);
  child_suspension->Resume();
  platform.RunPlatformModuleTasks();
  REQUIRE(child_task_completions == 0);
  REQUIRE_THROWS_AS(captured_child_task_scope.Launch(DirectTask(&child_task_completions)), std::logic_error);
}

TEST_CASE("RuntimeDestructionClosesRunningTaskScopes") {
  captured_task_scope = {};
  auto suspension = std::make_shared<ManualSuspensionState>();
  TestPlatform platform;
  {
    Runtime runtime(TaskScopeApp, platform);
    runtime.BuildFrame();
    captured_task_scope.Launch(SuspendedTask(suspension));
    platform.RunPlatformModuleTasks();
    REQUIRE_FALSE(suspension->canceled);
  }
  REQUIRE(suspension->canceled);
}

TEST_CASE("HuxerUIAwaitableRestoresTaskExecutionToTheOwningUIThread") {
  captured_task_scope = {};
  task_value = State<int>{};
  std::mutex queue_mutex;
  std::vector<std::function<void()>> queued;
  const std::thread::id ui_thread = std::this_thread::get_id();
  TestPlatform platform([&](std::function<void()> callback) {
    std::scoped_lock lock(queue_mutex);
    queued.push_back(std::move(callback));
  });
  const auto drain = [&] {
    for (;;) {
      std::vector<std::function<void()>> current;
      {
        std::scoped_lock lock(queue_mutex);
        current = std::move(queued);
      }
      if (current.empty()) {
        return;
      }
      for (const auto& callback : current) {
        callback();
      }
    }
  };
  Runtime runtime(TaskScopeApp, platform);
  runtime.BuildFrame();

  auto suspension = std::make_shared<ManualSuspensionState>();
  std::thread::id resumed_thread;
  captured_task_scope.Launch([state = task_value, suspension, &resumed_thread]() -> Task<void> {
    co_await ManualAwaiter(suspension);
    resumed_thread = std::this_thread::get_id();
    state = 7;
  });
  drain();

  std::thread worker([suspension] { suspension->Resume(); });
  worker.join();
  REQUIRE(task_value.Get() == 0);

  drain();
  REQUIRE(resumed_thread == ui_thread);
  REQUIRE(task_value.Get() == 7);
}

#if !defined(__EMSCRIPTEN__)

TEST_CASE("RunWorkerOwnsItsInvocationAndRestoresResultsAndExceptionsToTheUIThread") {
  captured_task_scope = {};
  ThreadSafeTaskQueue queue;
  TestPlatform platform(queue.Dispatcher());
  Runtime runtime(TaskScopeApp, platform);
  runtime.BuildFrame();
  const std::thread::id ui_thread = std::this_thread::get_id();
  std::thread::id worker_thread;
  std::thread::id resumed_thread;
  int result = 0;
  bool completed = false;
  bool caught = false;

  captured_task_scope.Launch([&]() -> Task<void> {
    const int sum = co_await RunWorker(AddWorkerValues, 20, 22);
    auto owned = co_await RunWorker(
        [&worker_thread](std::unique_ptr<int> value) {
          worker_thread = std::this_thread::get_id();
          ++*value;
          return value;
        },
        std::make_unique<int>(sum)
    );
    co_await RunWorker([] {});
    try {
      static_cast<void>(co_await RunWorker([]() -> int { throw std::runtime_error("worker failed"); }));
    } catch (const std::runtime_error&) {
      caught = true;
    }
    resumed_thread = std::this_thread::get_id();
    result = *owned;
    completed = true;
  });

  REQUIRE_FALSE(completed);
  queue.Drain();
  DrainUntil(queue, [&] { return completed; });

  REQUIRE(result == 43);
  REQUIRE(caught);
  REQUIRE(worker_thread != ui_thread);
  REQUIRE(resumed_thread == ui_thread);
}

TEST_CASE("RunWorkerUsesBoundedConcurrencyAndDiscardsCanceledWork") {
  captured_task_scope = {};
  ThreadSafeTaskQueue queue;
  TestPlatform platform(queue.Dispatcher());
  Runtime runtime(TaskScopeApp, platform);
  runtime.BuildFrame();
  const std::size_t worker_count = detail::WorkerConcurrency();
  REQUIRE(worker_count > 0);
  WorkerGate gate;
  std::vector<TaskHandle> running;
  running.reserve(worker_count);
  int completions = 0;

  for (std::size_t index = 0; index < worker_count; ++index) {
    running.push_back(captured_task_scope.Launch([&]() -> Task<void> {
      co_await RunWorker([&gate] { gate.EnterAndWait(); });
      ++completions;
    }));
  }
  queue.Drain();
  REQUIRE(gate.WaitForStarted(worker_count));
  REQUIRE(gate.Started() == worker_count);

  std::atomic<bool> queued_executed = false;
  TaskHandle queued = captured_task_scope.Launch([&]() -> Task<void> {
    co_await RunWorker([&queued_executed] { queued_executed = true; });
    ++completions;
  });
  queue.Drain();
  running.front().Cancel();
  queued.Cancel();
  gate.Release();

  DrainUntil(queue, [&] { return completions == static_cast<int>(worker_count - 1); });
  REQUIRE(gate.WaitForDeparted(worker_count));
  REQUIRE_FALSE(queued_executed);
}

TEST_CASE("WorkerSequenceSerializesOperationsAcrossCopiedHandles") {
  captured_task_scope = {};
  ThreadSafeTaskQueue queue;
  TestPlatform platform(queue.Dispatcher());
  Runtime runtime(TaskScopeApp, platform);
  runtime.BuildFrame();
  const std::thread::id ui_thread = std::this_thread::get_id();
  std::thread::id resumed_thread;
  WorkerGate first_gate;
  std::atomic<bool> second_started = false;
  std::vector<int> order;
  int result = 0;
  int completions = 0;

  {
    WorkerSequence sequence;
    WorkerSequence copied_sequence = sequence;
    auto first = sequence.Run(
        [&](std::stop_token, std::unique_ptr<int> value) {
          order.push_back(1);
          first_gate.EnterAndWait();
          ++*value;
          return value;
        },
        std::make_unique<int>(41)
    );
    auto second = copied_sequence.Run([&](std::stop_token) {
      second_started = true;
      order.push_back(2);
    });
    captured_task_scope.Launch([&, work = std::move(first)]() mutable -> Task<void> {
      auto owned = co_await std::move(work);
      result = *owned;
      ++completions;
    });
    captured_task_scope.Launch([&, work = std::move(second)]() mutable -> Task<void> {
      co_await std::move(work);
      resumed_thread = std::this_thread::get_id();
      ++completions;
    });
  }
  queue.Drain();

  REQUIRE(first_gate.WaitForStarted(1));
  REQUIRE_FALSE(second_started);
  first_gate.Release();
  DrainUntil(queue, [&] { return completions == 2; });

  REQUIRE(result == 42);
  REQUIRE(order == std::vector<int>{1, 2});
  REQUIRE(resumed_thread == ui_thread);
}

TEST_CASE("IndependentWorkerSequencesCanUseTheSharedPoolConcurrently") {
  if (detail::WorkerConcurrency() < 2) {
    return;
  }
  captured_task_scope = {};
  ThreadSafeTaskQueue queue;
  TestPlatform platform(queue.Dispatcher());
  Runtime runtime(TaskScopeApp, platform);
  runtime.BuildFrame();
  WorkerSequence first_sequence;
  WorkerSequence second_sequence;
  WorkerGate gate;
  int completions = 0;

  captured_task_scope.Launch([&]() -> Task<void> {
    co_await first_sequence.Run([&](std::stop_token) { gate.EnterAndWait(); });
    ++completions;
  });
  captured_task_scope.Launch([&]() -> Task<void> {
    co_await second_sequence.Run([&](std::stop_token) { gate.EnterAndWait(); });
    ++completions;
  });
  queue.Drain();

  REQUIRE(gate.WaitForStarted(2));
  gate.Release();
  DrainUntil(queue, [&] { return completions == 2; });
}

TEST_CASE("WorkerSequenceAndRunWorkerCanUseTheSharedPoolConcurrently") {
  if (detail::WorkerConcurrency() < 2) {
    return;
  }
  captured_task_scope = {};
  ThreadSafeTaskQueue queue;
  TestPlatform platform(queue.Dispatcher());
  Runtime runtime(TaskScopeApp, platform);
  runtime.BuildFrame();
  WorkerSequence sequence;
  WorkerGate gate;
  int completions = 0;

  captured_task_scope.Launch([&]() -> Task<void> {
    co_await sequence.Run([&](std::stop_token) { gate.EnterAndWait(); });
    ++completions;
  });
  captured_task_scope.Launch([&]() -> Task<void> {
    co_await RunWorker([&] { gate.EnterAndWait(); });
    ++completions;
  });
  queue.Drain();

  REQUIRE(gate.WaitForStarted(2));
  gate.Release();
  DrainUntil(queue, [&] { return completions == 2; });
}

TEST_CASE("WorkerSequenceCancellationSkipsQueuedWorkAndStopsActiveWorkCooperatively") {
  captured_task_scope = {};
  ThreadSafeTaskQueue queue;
  TestPlatform platform(queue.Dispatcher());
  Runtime runtime(TaskScopeApp, platform);
  runtime.BuildFrame();
  WorkerSequence sequence;
  StopAwareWorkerGate active_gate;
  std::atomic<bool> queued_executed = false;
  bool active_resumed = false;
  bool trailing_completed = false;
  bool trailing_started_after_cleanup = false;

  TaskHandle active = captured_task_scope.Launch([&]() -> Task<void> {
    co_await sequence.Run([&](std::stop_token stop_token) {
      active_gate.EnterAndWait(stop_token);
      throw std::runtime_error("canceled sequence operation failed");
    });
    active_resumed = true;
  });
  TaskHandle queued = captured_task_scope.Launch([&]() -> Task<void> {
    co_await sequence.Run([&](std::stop_token) { queued_executed = true; });
  });
  captured_task_scope.Launch([&]() -> Task<void> {
    co_await sequence.Run([&](std::stop_token) {
      trailing_started_after_cleanup = active_gate.Cleaned();
    });
    trailing_completed = true;
  });
  queue.Drain();
  REQUIRE(active_gate.WaitForStarted());

  queued.Cancel();
  active.Cancel();
  queue.Drain();
  REQUIRE(active_gate.WaitForCleanup());
  DrainUntil(queue, [&] { return trailing_completed; });

  REQUIRE_FALSE(active_resumed);
  REQUIRE_FALSE(queued_executed);
  REQUIRE(trailing_started_after_cleanup);
}

TEST_CASE("WorkerSequencePromotesTheNextOperationAfterAnException") {
  captured_task_scope = {};
  ThreadSafeTaskQueue queue;
  TestPlatform platform(queue.Dispatcher());
  Runtime runtime(TaskScopeApp, platform);
  runtime.BuildFrame();
  WorkerSequence sequence;
  bool caught = false;
  int result = 0;
  int completions = 0;

  captured_task_scope.Launch([&]() -> Task<void> {
    try {
      static_cast<void>(co_await sequence.Run([](std::stop_token) -> int {
        throw std::runtime_error("sequenced worker failed");
      }));
    } catch (const std::runtime_error&) {
      caught = true;
    }
    ++completions;
  });
  captured_task_scope.Launch([&]() -> Task<void> {
    result = co_await sequence.Run([](std::stop_token) { return 7; });
    ++completions;
  });
  queue.Drain();
  DrainUntil(queue, [&] { return completions == 2; });

  REQUIRE(caught);
  REQUIRE(result == 7);
}

TEST_CASE("RuntimeDestructionDiscardsARunningWorkerResult") {
  captured_task_scope = {};
  ThreadSafeTaskQueue queue;
  TestPlatform platform(queue.Dispatcher());
  WorkerGate gate;
  bool resumed = false;
  {
    Runtime runtime(TaskScopeApp, platform);
    runtime.BuildFrame();
    captured_task_scope.Launch([&]() -> Task<void> {
      co_await RunWorker([&gate] { gate.EnterAndWait(); });
      resumed = true;
    });
    queue.Drain();
    REQUIRE(gate.WaitForStarted(1));
  }

  gate.Release();
  REQUIRE(gate.WaitForDeparted(1));
  queue.Drain();
  REQUIRE_FALSE(resumed);
}

TEST_CASE("RuntimeDestructionCancelsAWorkerSequenceAndSkipsItsQueuedWork") {
  captured_task_scope = {};
  ThreadSafeTaskQueue queue;
  TestPlatform platform(queue.Dispatcher());
  StopAwareWorkerGate active_gate;
  std::atomic<bool> queued_executed = false;
  bool resumed = false;
  {
    Runtime runtime(TaskScopeApp, platform);
    runtime.BuildFrame();
    WorkerSequence sequence;
    captured_task_scope.Launch([&, sequence]() -> Task<void> {
      co_await sequence.Run([&](std::stop_token stop_token) { active_gate.EnterAndWait(stop_token); });
      resumed = true;
    });
    captured_task_scope.Launch([&, sequence]() -> Task<void> {
      co_await sequence.Run([&](std::stop_token) { queued_executed = true; });
    });
    queue.Drain();
    REQUIRE(active_gate.WaitForStarted());
  }

  REQUIRE(active_gate.WaitForCleanup());
  queue.Drain();
  REQUIRE_FALSE(resumed);
  REQUIRE_FALSE(queued_executed);
}

#else

TEST_CASE("RunWorkerReportsUnavailableWebWorkerExecution") {
  captured_task_scope = {};
  TestPlatform platform;
  Runtime runtime(TaskScopeApp, platform);
  runtime.BuildFrame();
  bool unavailable = false;

  captured_task_scope.Launch([&]() -> Task<void> {
    try {
      static_cast<void>(co_await RunWorker([] { return 1; }));
    } catch (const std::runtime_error&) {
      unavailable = true;
    }
  });
  platform.RunPlatformModuleTasks();

  REQUIRE(unavailable);
}

TEST_CASE("WorkerSequenceReportsUnavailableWebWorkerExecution") {
  captured_task_scope = {};
  TestPlatform platform;
  Runtime runtime(TaskScopeApp, platform);
  runtime.BuildFrame();
  WorkerSequence sequence;
  bool unavailable = false;
  bool invoked = false;

  captured_task_scope.Launch([&]() -> Task<void> {
    try {
      static_cast<void>(co_await sequence.Run([&](std::stop_token) {
        invoked = true;
        return 1;
      }));
    } catch (const std::runtime_error&) {
      unavailable = true;
    }
  });
  platform.RunPlatformModuleTasks();

  REQUIRE(unavailable);
  REQUIRE_FALSE(invoked);
}

#endif

TEST_CASE("TaskScopePostDefersOwnedCallbacksAndPreservesExternalThreadOrder") {
  captured_task_scope = {};
  ThreadSafeTaskQueue queue;
  TestPlatform platform(queue.Dispatcher());
  Runtime runtime(TaskScopeApp, platform);
  runtime.BuildFrame();
  const std::thread::id ui_thread = std::this_thread::get_id();
  std::thread::id callback_thread;
  std::vector<int> values;

  captured_task_scope.Post([value = std::make_unique<int>(1), &values] { values.push_back(*value); });
  REQUIRE(values.empty());
  queue.Drain();
  REQUIRE(values == std::vector<int>{1});

  std::thread external([tasks = captured_task_scope, &values, &callback_thread] {
    tasks.Post([&values, &callback_thread] {
      callback_thread = std::this_thread::get_id();
      values.push_back(2);
    });
    tasks.Post([&values] { values.push_back(3); });
  });
  external.join();
  REQUIRE(values == std::vector<int>{1});

  queue.Drain();
  REQUIRE(values == std::vector<int>{1, 2, 3});
  REQUIRE(callback_thread == ui_thread);
}

TEST_CASE("TaskScopePostSuppressesCallbacksAfterScopeAndRuntimeClosure") {
  captured_post_child_scope = {};
  post_child_visible = State<bool>{};
  ThreadSafeTaskQueue queue;
  TestPlatform platform(queue.Dispatcher());
  int callbacks = 0;
  Runtime runtime(PostUnmountApp, platform);
  runtime.BuildFrame();

  captured_post_child_scope.Post([&callbacks] { ++callbacks; });
  post_child_visible = false;
  runtime.BuildFrame();
  queue.Drain();
  REQUIRE(callbacks == 0);

  captured_post_child_scope.Post([&callbacks] { ++callbacks; });
  queue.Drain();
  REQUIRE(callbacks == 0);

  {
    Runtime pending_runtime(TaskScopeApp, platform);
    pending_runtime.BuildFrame();
    captured_task_scope.Post([&callbacks] { ++callbacks; });
  }
  queue.Drain();
  REQUIRE(callbacks == 0);
}

TEST_CASE("TaskScopeReportsInvalidUsage") {
  int starts = 0;
  REQUIRE_THROWS_AS(UseTaskScope(), std::logic_error);
  REQUIRE_THROWS_AS(TaskScope{}.Launch(DirectTask(&starts)), std::logic_error);
  REQUIRE_THROWS_AS(TaskScope{}.Post([] {}), std::logic_error);

  TestPlatform valid_platform;
  Runtime valid_runtime(TaskScopeApp, valid_platform);
  valid_runtime.BuildFrame();
  Task<void> empty;
  REQUIRE_THROWS_AS(captured_task_scope.Launch(std::move(empty)), std::logic_error);

  TestPlatform platform(UIThreadDispatcher{});
  Runtime runtime(TaskScopeApp, platform);
  REQUIRE_THROWS_AS(runtime.BuildFrame(), std::logic_error);

  composition_task_starts = 0;
  TestPlatform dispatched_platform;
  Runtime composition_runtime(IllegalTaskLaunchApp, dispatched_platform);
  REQUIRE_THROWS_AS(composition_runtime.BuildFrame(), std::logic_error);
  dispatched_platform.RunPlatformModuleTasks();
  REQUIRE(composition_task_starts == 0);

  composition_post_runs = 0;
  Runtime post_runtime(IllegalTaskPostApp, dispatched_platform);
  REQUIRE_THROWS_AS(post_runtime.BuildFrame(), std::logic_error);
  dispatched_platform.RunPlatformModuleTasks();
  REQUIRE(composition_post_runs == 0);
}

} // namespace huxerui::test
