#include <catch2/catch_amalgamated.hpp>

#include <limits>
#include <stdexcept>

#include "application/platform_frame_internal.h"

namespace huxerui::detail {
namespace {

TEST_CASE("FrameCoordinatorNormalizesAndBeginsPendingCommits") {
  FrameCoordinator coordinator;
  int commits = 0;
  const auto publish = [&] { ++commits; };

  REQUIRE(coordinator.Request(std::numeric_limits<double>::quiet_NaN(), 4.0, true) == 4.0);
  REQUIRE(coordinator.FrameBuildPending());
  coordinator.Commit(publish);
  REQUIRE_FALSE(coordinator.FrameBuildPending());
  coordinator.Commit(publish);
  REQUIRE(commits == 1);

  REQUIRE(coordinator.Request(std::numeric_limits<double>::infinity(), 5.0, true) ==
          std::numeric_limits<double>::max());
}

TEST_CASE("FrameCoordinatorDefersTheEarliestRequestUntilPaintingCompletes") {
  FrameCoordinator coordinator;
  coordinator.MarkPaintPending();

  REQUIRE_FALSE(coordinator.Request(8.0, 2.0, true).has_value());
  REQUIRE_FALSE(coordinator.Request(6.0, 2.0, true).has_value());
  coordinator.BeginPaint();
  REQUIRE(coordinator.EndPaint(true) == 6.0);
  REQUIRE_FALSE(coordinator.TakeDeferred(true).has_value());
}

TEST_CASE("FrameCoordinatorDefersRequestsMadeDuringPlatformPainting") {
  FrameCoordinator coordinator;

  REQUIRE(coordinator.Request(1.0, 0.0, true) == 1.0);
  coordinator.Commit([] {});
  coordinator.MarkPaintPending();
  coordinator.BeginPaint();

  REQUIRE_FALSE(coordinator.Request(3.0, 2.0, true).has_value());
  REQUIRE(coordinator.EndPaint(true) == 3.0);
  REQUIRE(coordinator.FrameBuildPending());
}

TEST_CASE("FrameCoordinatorWaitsForPlatformReadinessBeforeScheduling") {
  FrameCoordinator coordinator;

  REQUIRE_FALSE(coordinator.Request(3.0, 1.0, false).has_value());
  REQUIRE_FALSE(coordinator.TakeDeferred(false).has_value());
  REQUIRE(coordinator.TakeDeferred(true) == 3.0);
}

TEST_CASE("FrameCoordinator holds drawing and frame requests until handoff completes") {
  FrameCoordinator coordinator;
  const auto publish = [&] {
    REQUIRE_FALSE(coordinator.Request(3.0, 1.0, true));
    coordinator.Commit([] { FAIL("A pending frame must wait until the current commit completes"); });
    REQUIRE_FALSE(coordinator.TakeDeferred(true));
    REQUIRE(coordinator.DeferPaintIfCommitting());
  };
  REQUIRE(coordinator.Request(1.0, 0.0, true) == 1.0);
  SECTION("Publish completes") {
    coordinator.Commit(publish);
  }
  SECTION("Publish throws") {
    REQUIRE_THROWS_AS(coordinator.Commit([&] {
      publish();
      throw std::runtime_error("publish failed");
    }), std::runtime_error);
  }

  REQUIRE_FALSE(coordinator.DeferPaintIfCommitting());
  REQUIRE(coordinator.PaintInterrupted());
  REQUIRE(coordinator.TakeDeferred(true) == 3.0);
  coordinator.Commit([] {});
  REQUIRE_FALSE(coordinator.PaintInterrupted());
}

} // namespace
} // namespace huxerui::detail
