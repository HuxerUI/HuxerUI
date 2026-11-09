#include <catch2/catch_amalgamated.hpp>

#include <limits>

#include "application/platform_frame_internal.h"

namespace huxerui::detail {
namespace {

TEST_CASE("PlatformFrameCoordinatorNormalizesAndBeginsPendingCommits") {
  PlatformFrameCoordinator coordinator;

  REQUIRE(coordinator.Request(std::numeric_limits<double>::quiet_NaN(), 4.0, true) == 4.0);
  REQUIRE(coordinator.FrameBuildPending());
  REQUIRE(coordinator.BeginCommit());
  REQUIRE_FALSE(coordinator.FrameBuildPending());
  REQUIRE_FALSE(coordinator.BeginCommit());

  REQUIRE(coordinator.Request(std::numeric_limits<double>::infinity(), 5.0, true) ==
          std::numeric_limits<double>::max());
}

TEST_CASE("PlatformFrameCoordinatorDefersTheEarliestRequestUntilPaintingCompletes") {
  PlatformFrameCoordinator coordinator;
  coordinator.MarkPaintPending();

  REQUIRE_FALSE(coordinator.Request(8.0, 2.0, true).has_value());
  REQUIRE_FALSE(coordinator.Request(6.0, 2.0, true).has_value());
  coordinator.BeginPaint();
  REQUIRE(coordinator.EndPaint(true) == 6.0);
  REQUIRE_FALSE(coordinator.TakeDeferred(true).has_value());
}

TEST_CASE("PlatformFrameCoordinatorDefersRequestsMadeDuringPlatformPainting") {
  PlatformFrameCoordinator coordinator;

  REQUIRE(coordinator.Request(1.0, 0.0, true) == 1.0);
  REQUIRE(coordinator.BeginCommit());
  coordinator.MarkPaintPending();
  coordinator.BeginPaint();

  REQUIRE_FALSE(coordinator.Request(3.0, 2.0, true).has_value());
  REQUIRE(coordinator.EndPaint(true) == 3.0);
  REQUIRE(coordinator.FrameBuildPending());
}

TEST_CASE("PlatformFrameCoordinatorWaitsForPlatformReadinessBeforeScheduling") {
  PlatformFrameCoordinator coordinator;

  REQUIRE_FALSE(coordinator.Request(3.0, 1.0, false).has_value());
  REQUIRE_FALSE(coordinator.TakeDeferred(false).has_value());
  REQUIRE(coordinator.TakeDeferred(true) == 3.0);
}

} // namespace
} // namespace huxerui::detail
