#include "runtime_test_support.h"

#include <coroutine>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "task_internal.h"

namespace huxerui::test {

namespace {

static_assert(std::move_constructible<Task<int>>);
static_assert(!std::copy_constructible<Task<int>>);
static_assert(std::copy_constructible<TaskScope>);
static_assert(std::copy_constructible<TaskHandle>);

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
std::shared_ptr<ManualSuspensionState> child_suspension;
int child_task_completions = 0;
bool child_cleanup_preceded_scope_cancellation = false;
StateList<int> task_keyed_items;
std::unordered_map<int, std::shared_ptr<ManualSuspensionState>> keyed_suspensions;
ScrollController task_virtual_scroll;
std::shared_ptr<ManualSuspensionState> virtual_suspension;
int composition_task_starts = 0;

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

TEST_CASE("TaskScopeReportsInvalidUsage") {
  int starts = 0;
  REQUIRE_THROWS_AS(UseTaskScope(), std::logic_error);
  REQUIRE_THROWS_AS(TaskScope{}.Launch(DirectTask(&starts)), std::logic_error);

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
}

} // namespace huxerui::test
