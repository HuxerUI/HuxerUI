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
