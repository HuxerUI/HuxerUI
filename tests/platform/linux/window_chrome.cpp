#include "linux_internal.h"
#include "runtime_test_support.h"

#include <limits>
#include <utility>

namespace huxerui::test {

TEST_CASE("LinuxResizeEdgesUseWiderCornersWithoutConsumingNearbyControls") {
  using Edge = detail::LinuxResizeEdge;
  const Size viewport{800.0F, 600.0F};
  const std::pair<Point, Edge> cases[] = {
      {{14.0F, 14.0F}, Edge::NorthWest},
      {{400.0F, 10.0F}, Edge::North},
      {{786.0F, 14.0F}, Edge::NorthEast},
      {{790.0F, 300.0F}, Edge::East},
      {{786.0F, 586.0F}, Edge::SouthEast},
      {{400.0F, 590.0F}, Edge::South},
      {{14.0F, 586.0F}, Edge::SouthWest},
      {{10.0F, 300.0F}, Edge::West},
      {{0.0F, 0.0F}, Edge::NorthWest},
      {{800.0F, 600.0F}, Edge::SouthEast},
  };
  for (const auto& [point, edge] : cases) {
    CAPTURE(point.x, point.y);
    REQUIRE(detail::ResolveLinuxResizeEdge(point, viewport, false) == edge);
  }
  const Point interior[] = {
      {400.0F, 300.0F}, {10.01F, 300.0F}, {789.99F, 300.0F},
      {400.0F, 10.01F}, {400.0F, 589.99F}, {14.01F, 14.0F}, {14.0F, 14.01F},
      {785.99F, 586.0F}, {786.0F, 585.99F},
  };
  for (const Point point : interior) {
    CAPTURE(point.x, point.y);
    REQUIRE_FALSE(detail::ResolveLinuxResizeEdge(point, viewport, false));
  }
  const auto title_bar = detail::ResolveLinuxTitleBarMetrics(40.0F, viewport, false);
  for (int index = 0; index < 3; ++index) {
    const Point caption_center{
        viewport.width - detail::kLinuxCaptionButtonWidth * (static_cast<float>(index) + 0.5F),
        title_bar.height * 0.5F,
    };
    REQUIRE_FALSE(detail::ResolveLinuxResizeEdge(caption_center, viewport, false));
  }
}

TEST_CASE("LinuxResizeEdgesRejectDisabledAndInvalidGeometry") {
  const Size viewport{800.0F, 600.0F};
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float infinity = std::numeric_limits<float>::infinity();
  REQUIRE_FALSE(detail::ResolveLinuxResizeEdge({0.0F, 0.0F}, viewport, true));
  const Point invalid_points[] = {
      {-0.01F, 300.0F}, {800.01F, 300.0F}, {400.0F, -0.01F}, {400.0F, 600.01F},
      {nan, 0.0F}, {0.0F, nan}, {infinity, 0.0F}, {0.0F, infinity},
      {-infinity, 0.0F}, {0.0F, -infinity},
  };
  for (const Point point : invalid_points) {
    CAPTURE(point.x, point.y);
    REQUIRE_FALSE(detail::ResolveLinuxResizeEdge(point, viewport, false));
  }
  const Size invalid_sizes[] = {
      {0.0F, 600.0F}, {800.0F, 0.0F}, {-1.0F, 600.0F}, {800.0F, -1.0F},
      {nan, 600.0F}, {800.0F, nan}, {infinity, 600.0F}, {800.0F, infinity},
  };
  for (const Size size : invalid_sizes) {
    CAPTURE(size.width, size.height);
    REQUIRE_FALSE(detail::ResolveLinuxResizeEdge({0.0F, 0.0F}, size, false));
  }
}

TEST_CASE("LinuxResizeEdgesChooseNearestSideWhenSmallWindowZonesOverlap") {
  using Edge = detail::LinuxResizeEdge;
  REQUIRE(detail::ResolveLinuxResizeEdge({9.0F, 9.0F}, {20.0F, 20.0F}, false) == Edge::NorthWest);
  REQUIRE(detail::ResolveLinuxResizeEdge({11.0F, 9.0F}, {20.0F, 20.0F}, false) == Edge::NorthEast);
  REQUIRE(detail::ResolveLinuxResizeEdge({9.0F, 11.0F}, {20.0F, 20.0F}, false) == Edge::SouthWest);
  REQUIRE(detail::ResolveLinuxResizeEdge({11.0F, 11.0F}, {20.0F, 20.0F}, false) == Edge::SouthEast);
  REQUIRE(detail::ResolveLinuxResizeEdge({10.0F, 10.0F}, {20.0F, 20.0F}, false) == Edge::NorthWest);
  REQUIRE(detail::ResolveLinuxResizeEdge({9.0F, 50.0F}, {20.0F, 100.0F}, false) == Edge::West);
  REQUIRE(detail::ResolveLinuxResizeEdge({11.0F, 50.0F}, {20.0F, 100.0F}, false) == Edge::East);
  REQUIRE(detail::ResolveLinuxResizeEdge({50.0F, 9.0F}, {100.0F, 20.0F}, false) == Edge::North);
  REQUIRE(detail::ResolveLinuxResizeEdge({50.0F, 11.0F}, {100.0F, 20.0F}, false) == Edge::South);
  REQUIRE(detail::ResolveLinuxResizeEdge({1.0F, 1.0F}, {1.0F, 1.0F}, false) == Edge::SouthEast);
}

TEST_CASE("LinuxResizeEdgesMapToMatchingPointerCursors") {
  using Edge = detail::LinuxResizeEdge;
  REQUIRE(detail::LinuxResizePointerCursor(Edge::North) == PointerCursorKind::ResizeVertical);
  REQUIRE(detail::LinuxResizePointerCursor(Edge::South) == PointerCursorKind::ResizeVertical);
  REQUIRE(detail::LinuxResizePointerCursor(Edge::East) == PointerCursorKind::ResizeHorizontal);
  REQUIRE(detail::LinuxResizePointerCursor(Edge::West) == PointerCursorKind::ResizeHorizontal);
  REQUIRE(detail::LinuxResizePointerCursor(Edge::NorthEast) == PointerCursorKind::ResizeNorthEastSouthWest);
  REQUIRE(detail::LinuxResizePointerCursor(Edge::SouthWest) == PointerCursorKind::ResizeNorthEastSouthWest);
  REQUIRE(detail::LinuxResizePointerCursor(Edge::NorthWest) == PointerCursorKind::ResizeNorthWestSouthEast);
  REQUIRE(detail::LinuxResizePointerCursor(Edge::SouthEast) == PointerCursorKind::ResizeNorthWestSouthEast);
}

TEST_CASE("LinuxTitleBarMetricsResolvePreferredHeightAndReserveControls") {
  const WindowTitleBarMetrics preferred = detail::ResolveLinuxTitleBarMetrics(40.0F, {320.0F, 200.0F}, false);
  REQUIRE(preferred.height == 40.0F);
  REQUIRE(preferred.left_inset == 0.0F);
  REQUIRE(preferred.right_inset == 3.0F * detail::kLinuxCaptionButtonWidth);
  REQUIRE_FALSE(preferred.maximized);

  const WindowTitleBarMetrics floored = detail::ResolveLinuxTitleBarMetrics(12.0F, {320.0F, 200.0F}, false);
  REQUIRE(floored.height == detail::kLinuxMinTitleBarHeight);

  const WindowTitleBarMetrics constrained = detail::ResolveLinuxTitleBarMetrics(40.0F, {100.0F, 20.0F}, false);
  REQUIRE(constrained.height == 20.0F);
  REQUIRE(constrained.right_inset == 100.0F);

  const WindowTitleBarMetrics maximized = detail::ResolveLinuxTitleBarMetrics(40.0F, {320.0F, 200.0F}, true);
  REQUIRE(maximized.maximized);

  const WindowTitleBarMetrics non_finite =
      detail::ResolveLinuxTitleBarMetrics(std::numeric_limits<float>::quiet_NaN(), {320.0F, 200.0F}, false);
  REQUIRE(non_finite.height == detail::kLinuxMinTitleBarHeight);
}

TEST_CASE("LinuxApplicationLifecycleIncludesMinimizedToplevelState") {
  REQUIRE(
      detail::ResolveLinuxApplicationLifecycleState(true, true, false) == ApplicationLifecycleState::Active
  );
  REQUIRE(
      detail::ResolveLinuxApplicationLifecycleState(true, false, false) == ApplicationLifecycleState::Inactive
  );
  REQUIRE(
      detail::ResolveLinuxApplicationLifecycleState(true, true, true) == ApplicationLifecycleState::Background
  );
  REQUIRE(
      detail::ResolveLinuxApplicationLifecycleState(false, true, false) == ApplicationLifecycleState::Background
  );
}

TEST_CASE("LinuxKeyTrackingBalancesInputMethodFilteringAndRepeat") {
  detail::LinuxKeyTracker keys;
  REQUIRE(keys.Press(38, false) == (detail::LinuxKeyPressResult{true, false}));
  REQUIRE(keys.Press(38, false) == (detail::LinuxKeyPressResult{true, true}));
  REQUIRE(keys.Release(38, false));

  REQUIRE(keys.Press(39, true) == (detail::LinuxKeyPressResult{}));
  REQUIRE_FALSE(keys.Release(39, false));
  REQUIRE(keys.Press(39, false) == (detail::LinuxKeyPressResult{true, false}));
  keys.Reset();
  REQUIRE(keys.Press(39, false) == (detail::LinuxKeyPressResult{true, false}));
}

} // namespace huxerui::test
