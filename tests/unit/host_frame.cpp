#include <catch2/catch_amalgamated.hpp>

#include <limits>

#include "host_frame_internal.h"

namespace huxerui::detail {
namespace {

TEST_CASE("HostFrameStateNormalizesAndBeginsPendingCommits") {
  HostFrameState state;

  REQUIRE(state.Request(std::numeric_limits<double>::quiet_NaN(), 4.0, true) == 4.0);
  REQUIRE(state.FrameBuildPending());
  REQUIRE(state.BeginCommit());
  REQUIRE_FALSE(state.FrameBuildPending());
  REQUIRE_FALSE(state.BeginCommit());

  REQUIRE(state.Request(std::numeric_limits<double>::infinity(), 5.0, true) == std::numeric_limits<double>::max());
}

TEST_CASE("HostFrameStateDefersTheEarliestRequestUntilPaintingCompletes") {
  HostFrameState state;
  state.MarkPaintPending();

  REQUIRE_FALSE(state.Request(8.0, 2.0, true).has_value());
  REQUIRE_FALSE(state.Request(6.0, 2.0, true).has_value());
  state.BeginPaint();
  REQUIRE(state.EndPaint(true) == 6.0);
  REQUIRE_FALSE(state.TakeDeferred(true).has_value());
}

TEST_CASE("HostFrameStateDefersRequestsMadeDuringNativePainting") {
  HostFrameState state;

  REQUIRE(state.Request(1.0, 0.0, true) == 1.0);
  REQUIRE(state.BeginCommit());
  state.MarkPaintPending();
  state.BeginPaint();

  REQUIRE_FALSE(state.Request(3.0, 2.0, true).has_value());
  REQUIRE(state.EndPaint(true) == 3.0);
  REQUIRE(state.FrameBuildPending());
}

TEST_CASE("HostFrameStateWaitsForTheNativeHostBeforeScheduling") {
  HostFrameState state;

  REQUIRE_FALSE(state.Request(3.0, 1.0, false).has_value());
  REQUIRE_FALSE(state.TakeDeferred(false).has_value());
  REQUIRE(state.TakeDeferred(true) == 3.0);
}

} // namespace
} // namespace huxerui::detail
