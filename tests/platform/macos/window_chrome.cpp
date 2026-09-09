#include "appkit_window_chrome.h"
#include "runtime_test_support.h"

namespace huxerui::test {

TEST_CASE("MacTitleBarMetricsReserveSystemControlsWithinTheViewport") {
  const WindowTitleBarMetrics preferred =
      detail::ResolveMacTitleBarMetrics(40.0F, 28.0F, {320.0F, 200.0F}, Rect{8.0F, 7.0F, 52.0F, 14.0F}, false);
  REQUIRE(preferred.height == 40.0F);
  REQUIRE(preferred.left_inset == 60.0F);
  REQUIRE(preferred.right_inset == 0.0F);
  REQUIRE_FALSE(preferred.maximized);

  const WindowTitleBarMetrics system_height =
      detail::ResolveMacTitleBarMetrics(12.0F, 28.0F, {320.0F, 200.0F}, Rect{8.0F, 7.0F, 52.0F, 14.0F}, false);
  REQUIRE(system_height.height == 28.0F);

  const WindowTitleBarMetrics constrained =
      detail::ResolveMacTitleBarMetrics(40.0F, 28.0F, {40.0F, 16.0F}, Rect{8.0F, 7.0F, 52.0F, 14.0F}, true);
  REQUIRE(constrained.height == 16.0F);
  REQUIRE(constrained.left_inset == 40.0F);
  REQUIRE(constrained.maximized);

  const WindowTitleBarMetrics without_controls =
      detail::ResolveMacTitleBarMetrics(40.0F, 28.0F, {320.0F, 200.0F}, std::nullopt, false);
  REQUIRE(without_controls.height == 40.0F);
  REQUIRE(without_controls.left_inset == 0.0F);
}

TEST_CASE("MacTitleBarControlsCenterWithinTheResolvedHeight") {
  REQUIRE(detail::ResolveMacTitleBarControlOriginY(40.0F, 14.0F) == 13.0F);
  REQUIRE(detail::ResolveMacTitleBarControlOriginY(28.0F, 14.0F) == 7.0F);
  REQUIRE(detail::ResolveMacTitleBarControlOriginY(16.0F, 14.0F) == 1.0F);
}

} // namespace huxerui::test
