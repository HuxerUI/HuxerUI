#include "linux_internal.h"
#include "runtime_test_support.h"

#include <limits>

namespace huxerui::test {

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

TEST_CASE("LinuxCustomChromeResizeEdgesUseLargerCornerTargets") {
  using detail::LinuxResizeEdge;
  const Size viewport{800.0F, 600.0F};
  REQUIRE(detail::ResolveLinuxResizeEdge({0.0F, 0.0F}, viewport, false) == LinuxResizeEdge::NorthWest);
  REQUIRE(detail::ResolveLinuxResizeEdge({799.0F, 1.0F}, viewport, false) == LinuxResizeEdge::NorthEast);
  REQUIRE(detail::ResolveLinuxResizeEdge({1.0F, 599.0F}, viewport, false) == LinuxResizeEdge::SouthWest);
  REQUIRE(detail::ResolveLinuxResizeEdge({799.0F, 599.0F}, viewport, false) == LinuxResizeEdge::SouthEast);
  REQUIRE(detail::ResolveLinuxResizeEdge({5.0F, 300.0F}, viewport, false) == LinuxResizeEdge::West);
  REQUIRE(detail::ResolveLinuxResizeEdge({795.0F, 300.0F}, viewport, false) == LinuxResizeEdge::East);
  REQUIRE(detail::ResolveLinuxResizeEdge({400.0F, 5.0F}, viewport, false) == LinuxResizeEdge::North);
  REQUIRE(detail::ResolveLinuxResizeEdge({400.0F, 595.0F}, viewport, false) == LinuxResizeEdge::South);

  // The corner target is intentionally deeper than the straight edge target.
  REQUIRE(detail::ResolveLinuxResizeEdge({12.0F, 12.0F}, viewport, false) == LinuxResizeEdge::NorthWest);
  REQUIRE_FALSE(detail::ResolveLinuxResizeEdge({12.0F, 300.0F}, viewport, false).has_value());
  REQUIRE_FALSE(detail::ResolveLinuxResizeEdge({400.0F, 300.0F}, viewport, false).has_value());
}

TEST_CASE("LinuxCustomChromeResizeEdgesRejectDisabledAndInvalidGeometry") {
  const Size viewport{800.0F, 600.0F};
  REQUIRE_FALSE(detail::ResolveLinuxResizeEdge({1.0F, 1.0F}, viewport, true).has_value());
  REQUIRE_FALSE(detail::ResolveLinuxResizeEdge({-1.0F, 10.0F}, viewport, false).has_value());
  REQUIRE_FALSE(detail::ResolveLinuxResizeEdge({801.0F, 10.0F}, viewport, false).has_value());
  REQUIRE_FALSE(detail::ResolveLinuxResizeEdge({1.0F, 1.0F}, {0.0F, 600.0F}, false).has_value());
  REQUIRE_FALSE(detail::ResolveLinuxResizeEdge(
      {std::numeric_limits<float>::quiet_NaN(), 1.0F}, viewport, false).has_value());
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
