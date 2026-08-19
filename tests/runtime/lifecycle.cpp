#include "runtime_test_support.h"

#include <stdexcept>
#include <string>
#include <vector>

namespace huxerui::test {

State<int> lifecycle_dependency;
State<int> lifecycle_recompose_trigger;
State<int> lifecycle_plain_dependency;
State<bool> lifecycle_child_visible;
State<bool> lifecycle_setup_state;
State<bool> lifecycle_cleanup_state;
StateList<int> lifecycle_list_dependency;
StateList<int> lifecycle_keyed_items;
ScrollController lifecycle_virtual_scroll;
bool lifecycle_composition_failed = false;
bool lifecycle_setup_throws = false;
bool lifecycle_dependency_comparison_throws = false;
int lifecycle_setups = 0;
int lifecycle_cleanups = 0;
int lifecycle_setup_state_compositions = 0;
int lifecycle_cleanup_state_compositions = 0;
std::vector<std::string> lifecycle_events;

struct ThrowingLifecycleDependency {
  int value = 0;

  bool operator==(const ThrowingLifecycleDependency& other) const {
    if (lifecycle_dependency_comparison_throws) {
      throw std::runtime_error("dependency comparison failed");
    }
    return value == other.value;
  }
};

View LifecycleStateApp() {
  lifecycle_dependency = UseState(0);
  lifecycle_recompose_trigger = UseState(0);
  Lifecycle(
      [] {
        ++lifecycle_setups;
        return []() noexcept { ++lifecycle_cleanups; };
      },
      lifecycle_dependency
  );
  if (lifecycle_recompose_trigger % 2 == 0) {
    return Text::Format("{}", lifecycle_recompose_trigger);
  }
  return Row {
    Text::Format("{}", lifecycle_recompose_trigger),
  };
}

View LifecycleListApp() {
  lifecycle_list_dependency = UseStateList<int>({1});
  Lifecycle(
      [] {
        ++lifecycle_setups;
        return []() noexcept { ++lifecycle_cleanups; };
      },
      lifecycle_list_dependency
  );
  return Text::Format("{}", lifecycle_list_dependency.Size());
}

View LifecycleChild() {
  Lifecycle([] {
    ++lifecycle_setups;
    return []() noexcept { ++lifecycle_cleanups; };
  });
  return Text("child");
}

View LifecycleUnmountApp() {
  lifecycle_child_visible = UseState(true);
  if (lifecycle_child_visible) {
    return Scope(LifecycleChild);
  }
  return Text("removed");
}

View LifecycleConditionalApp() {
  lifecycle_child_visible = UseState(true);
  if (lifecycle_child_visible) {
    Lifecycle([] {
      ++lifecycle_setups;
      return []() noexcept { ++lifecycle_cleanups; };
    });
  }
  return Text("conditional");
}

View LifecycleOrderingApp() {
  lifecycle_dependency = UseState(0);
  Lifecycle(
      [] {
        lifecycle_events.push_back("setup-a");
        return []() noexcept { lifecycle_events.push_back("cleanup-a"); };
      },
      lifecycle_dependency
  );
  Lifecycle(
      [] {
        lifecycle_events.push_back("setup-b");
        return []() noexcept { lifecycle_events.push_back("cleanup-b"); };
      },
      lifecycle_dependency
  );
  return Text("ordering");
}

View LifecycleFailureApp() {
  lifecycle_dependency = UseState(0);
  Lifecycle(
      [] {
        ++lifecycle_setups;
        return []() noexcept { ++lifecycle_cleanups; };
      },
      lifecycle_dependency
  );
  if (lifecycle_composition_failed) {
    throw std::runtime_error("composition failed");
  }
  return Text("failure");
}

View LifecyclePlainDependencyApp() {
  lifecycle_plain_dependency = UseState(0);
  const int parity = lifecycle_plain_dependency % 2;
  Lifecycle(
      [] {
        ++lifecycle_setups;
        return []() noexcept { ++lifecycle_cleanups; };
      },
      parity
  );
  return Text::Format("{}", lifecycle_plain_dependency);
}

View LifecycleSetupStateApp() {
  ++lifecycle_setup_state_compositions;
  lifecycle_setup_state = UseState(false);
  Lifecycle([state = lifecycle_setup_state] { state = true; });
  return Text(lifecycle_setup_state ? "ready" : "waiting");
}

View LifecycleCleanupStateApp() {
  ++lifecycle_cleanup_state_compositions;
  lifecycle_child_visible = UseState(true);
  lifecycle_cleanup_state = UseState(false);
  if (lifecycle_child_visible) {
    Lifecycle([] {
      return [state = lifecycle_cleanup_state]() noexcept { state = true; };
    });
  }
  return Text(lifecycle_cleanup_state ? "ready" : "waiting");
}

View LifecycleKeyedApp() {
  lifecycle_keyed_items = UseStateList<int>({1, 2});
  return Column {
    ForEach(lifecycle_keyed_items, [](int item) {
      return Scope([item] {
        Lifecycle([] {
          ++lifecycle_setups;
          return []() noexcept { ++lifecycle_cleanups; };
        });
        return Text::Format("{}", item);
      }).Key(item);
    }),
  };
}

View LifecycleSetupFailureApp() {
  lifecycle_dependency = UseState(0);
  Lifecycle([] {
    if (lifecycle_setup_throws) {
      throw std::runtime_error("setup failed");
    }
    ++lifecycle_setups;
    return []() noexcept { ++lifecycle_cleanups; };
  }, lifecycle_dependency);
  return Text("setup");
}

View LifecycleThrowingDependencyApp() {
  lifecycle_recompose_trigger = UseState(0);
  Lifecycle([] {
    ++lifecycle_setups;
    return []() noexcept { ++lifecycle_cleanups; };
  }, ThrowingLifecycleDependency{});
  return Text::Format("{}", lifecycle_recompose_trigger);
}

View LifecycleVirtualItem(std::size_t index) {
  return Scope([index] {
    if (index == 0) {
      Lifecycle([] {
        ++lifecycle_setups;
        return []() noexcept { ++lifecycle_cleanups; };
      });
    }
    return Text::Format("{}", index);
  }).Key(index);
}

View LifecycleVirtualListApp() {
  auto scroll = UseScrollController();
  lifecycle_virtual_scroll = scroll;
  return VirtualList(std::size_t{100}, LifecycleVirtualItem)
      .Controller(scroll)
      .ItemExtent(20.0F)
      .CacheExtent(0.0F);
}

TEST_CASE("LifecycleRunsAfterCommitAndRestartsOnlyWhenItsStateDependencyChanges") {
  lifecycle_setups = 0;
  lifecycle_cleanups = 0;
  TestPlatform platform;
  Runtime runtime(LifecycleStateApp, platform);

  REQUIRE(lifecycle_setups == 0);
  runtime.BuildFrame();
  REQUIRE(lifecycle_setups == 1);
  REQUIRE(lifecycle_cleanups == 0);

  lifecycle_recompose_trigger = 1;
  runtime.BuildFrame();
  REQUIRE(lifecycle_setups == 1);
  REQUIRE(lifecycle_cleanups == 0);

  lifecycle_dependency = 1;
  runtime.BuildFrame();
  REQUIRE(lifecycle_setups == 2);
  REQUIRE(lifecycle_cleanups == 1);
}

TEST_CASE("LifecycleObservesStateListVersions") {
  lifecycle_setups = 0;
  lifecycle_cleanups = 0;
  TestPlatform platform;
  Runtime runtime(LifecycleListApp, platform);

  runtime.BuildFrame();
  lifecycle_list_dependency.PushBack(2);
  runtime.BuildFrame();

  REQUIRE(lifecycle_setups == 2);
  REQUIRE(lifecycle_cleanups == 1);
}

TEST_CASE("LifecycleComparesOrdinaryValueDependencies") {
  lifecycle_setups = 0;
  lifecycle_cleanups = 0;
  TestPlatform platform;
  Runtime runtime(LifecyclePlainDependencyApp, platform);

  runtime.BuildFrame();
  lifecycle_plain_dependency = 2;
  runtime.BuildFrame();
  REQUIRE(lifecycle_setups == 1);
  REQUIRE(lifecycle_cleanups == 0);

  lifecycle_plain_dependency = 3;
  runtime.BuildFrame();
  REQUIRE(lifecycle_setups == 2);
  REQUIRE(lifecycle_cleanups == 1);
}

TEST_CASE("LifecycleStateWritesScheduleTheNextFrame") {
  lifecycle_setup_state_compositions = 0;
  TestPlatform platform;
  Runtime runtime(LifecycleSetupStateApp, platform);

  runtime.BuildFrame();
  REQUIRE(lifecycle_setup_state.Get());
  REQUIRE(lifecycle_setup_state_compositions == 1);
  REQUIRE(runtime.LastCommit().next_frame_deadline.has_value());
  runtime.BuildFrame();
  REQUIRE(lifecycle_setup_state_compositions == 2);
}

TEST_CASE("LifecycleCleanupStateWritesScheduleTheNextFrame") {
  lifecycle_cleanup_state_compositions = 0;
  TestPlatform platform;
  Runtime runtime(LifecycleCleanupStateApp, platform);

  runtime.BuildFrame();
  lifecycle_child_visible = false;
  runtime.BuildFrame();
  REQUIRE(lifecycle_cleanup_state.Get());
  REQUIRE(lifecycle_cleanup_state_compositions == 2);
  REQUIRE(runtime.LastCommit().next_frame_deadline.has_value());
  runtime.BuildFrame();
  REQUIRE(lifecycle_cleanup_state_compositions == 3);
}

TEST_CASE("LifecycleCleansUpWhenItsScopeUnmountsAndWhenRuntimeIsDestroyed") {
  lifecycle_setups = 0;
  lifecycle_cleanups = 0;
  TestPlatform platform;
  {
    Runtime runtime(LifecycleUnmountApp, platform);
    runtime.BuildFrame();
    REQUIRE(lifecycle_setups == 1);

    lifecycle_child_visible = false;
    runtime.BuildFrame();
    REQUIRE(lifecycle_cleanups == 1);
  }
  REQUIRE(lifecycle_cleanups == 1);

  lifecycle_child_visible = State<bool>{};
  lifecycle_setups = 0;
  lifecycle_cleanups = 0;
  {
    Runtime runtime(LifecycleStateApp, platform);
    runtime.BuildFrame();
  }
  REQUIRE(lifecycle_setups == 1);
  REQUIRE(lifecycle_cleanups == 1);
}

TEST_CASE("LifecycleCleansUpWhenACommittedCompositionOmitsItsDeclaration") {
  lifecycle_setups = 0;
  lifecycle_cleanups = 0;
  TestPlatform platform;
  Runtime runtime(LifecycleConditionalApp, platform);

  runtime.BuildFrame();
  lifecycle_child_visible = false;
  runtime.BuildFrame();

  REQUIRE(lifecycle_setups == 1);
  REQUIRE(lifecycle_cleanups == 1);
}

TEST_CASE("LifecycleRestartsWhenAVirtualizedScopeIsEvictedAndRealizedAgain") {
  lifecycle_setups = 0;
  lifecycle_cleanups = 0;
  TestPlatform platform;
  Runtime runtime(LifecycleVirtualListApp, platform);
  runtime.SetWindowMetrics({.viewport = {100.0F, 40.0F}});

  runtime.BuildFrame();
  REQUIRE(lifecycle_setups == 1);

  REQUIRE(lifecycle_virtual_scroll.ScrollToItem(50));
  runtime.BuildFrame();
  REQUIRE(lifecycle_cleanups == 1);

  REQUIRE(lifecycle_virtual_scroll.ScrollToItem(0));
  runtime.BuildFrame();
  REQUIRE(lifecycle_setups == 2);
}

TEST_CASE("LifecyclePreservesKeyedScopesWhenItemsMove") {
  lifecycle_setups = 0;
  lifecycle_cleanups = 0;
  TestPlatform platform;
  Runtime runtime(LifecycleKeyedApp, platform);

  runtime.BuildFrame();
  REQUIRE(lifecycle_setups == 2);
  lifecycle_keyed_items.Move(1, 0);
  runtime.BuildFrame();
  REQUIRE(lifecycle_setups == 2);
  REQUIRE(lifecycle_cleanups == 0);
}

TEST_CASE("LifecycleCleansUpInReverseOrderBeforeStartingChangedDependencies") {
  lifecycle_events.clear();
  TestPlatform platform;
  Runtime runtime(LifecycleOrderingApp, platform);

  runtime.BuildFrame();
  REQUIRE(lifecycle_events == std::vector<std::string>{"setup-a", "setup-b"});

  lifecycle_dependency = 1;
  runtime.BuildFrame();
  REQUIRE(
      lifecycle_events ==
      std::vector<std::string>{
          "setup-a",
          "setup-b",
          "cleanup-b",
          "cleanup-a",
          "setup-a",
          "setup-b",
      }
  );
}

TEST_CASE("LifecycleDiscardsDeclarationsFromFailedComposition") {
  lifecycle_setups = 0;
  lifecycle_cleanups = 0;
  lifecycle_composition_failed = true;
  TestPlatform platform;
  Runtime runtime(LifecycleFailureApp, platform);

  REQUIRE_THROWS_AS(runtime.BuildFrame(), std::runtime_error);
  REQUIRE(lifecycle_setups == 0);
  REQUIRE(lifecycle_cleanups == 0);

  lifecycle_composition_failed = false;
  runtime.BuildFrame();
  REQUIRE(lifecycle_setups == 1);

  lifecycle_composition_failed = true;
  lifecycle_dependency = 1;
  REQUIRE_THROWS_AS(runtime.BuildFrame(), std::runtime_error);
  REQUIRE(lifecycle_setups == 1);
  REQUIRE(lifecycle_cleanups == 0);
}

TEST_CASE("LifecyclePropagatesSetupExceptionsAndCanRetryAfterInvalidation") {
  lifecycle_setups = 0;
  lifecycle_cleanups = 0;
  lifecycle_setup_throws = true;
  TestPlatform platform;
  Runtime runtime(LifecycleSetupFailureApp, platform);

  REQUIRE_THROWS_AS(runtime.BuildFrame(), std::runtime_error);
  REQUIRE(lifecycle_setups == 0);
  REQUIRE(lifecycle_cleanups == 0);

  lifecycle_setup_throws = false;
  lifecycle_dependency = 1;
  runtime.BuildFrame();
  REQUIRE(lifecycle_setups == 1);
  REQUIRE(lifecycle_cleanups == 0);
}

TEST_CASE("LifecyclePropagatesDependencyComparisonExceptionsBeforeCleanup") {
  lifecycle_setups = 0;
  lifecycle_cleanups = 0;
  lifecycle_dependency_comparison_throws = false;
  TestPlatform platform;
  Runtime runtime(LifecycleThrowingDependencyApp, platform);

  runtime.BuildFrame();
  REQUIRE(lifecycle_setups == 1);
  lifecycle_dependency_comparison_throws = true;
  lifecycle_recompose_trigger = 1;
  REQUIRE_THROWS_AS(runtime.BuildFrame(), std::runtime_error);
  REQUIRE(lifecycle_setups == 1);
  REQUIRE(lifecycle_cleanups == 0);

  lifecycle_dependency_comparison_throws = false;
  lifecycle_recompose_trigger = 2;
  runtime.BuildFrame();
  REQUIRE(lifecycle_setups == 1);
  REQUIRE(lifecycle_cleanups == 0);
}

TEST_CASE("LifecycleRequiresAnActiveComposition") {
  REQUIRE_THROWS_AS(Lifecycle([] {}), std::logic_error);
}

} // namespace huxerui::test
