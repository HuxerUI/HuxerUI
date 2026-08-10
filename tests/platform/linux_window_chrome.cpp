#include "runtime_test_support.h"
#include "linux_internal.h"

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

TEST_CASE("LinuxResizeDirectionMatchesEdgeSemantics") {
  const Size viewport{300.0F, 200.0F};
  const float border = 6.0F;

  REQUIRE(detail::ResolveLinuxResizeDirection({0.0F, 0.0F}, border, viewport, false) ==
          detail::LinuxResizeDirection::TopLeft);
  REQUIRE(detail::ResolveLinuxResizeDirection({150.0F, 0.0F}, border, viewport, false) ==
          detail::LinuxResizeDirection::Top);
  REQUIRE(detail::ResolveLinuxResizeDirection({299.0F, 0.0F}, border, viewport, false) ==
          detail::LinuxResizeDirection::TopRight);
  REQUIRE(detail::ResolveLinuxResizeDirection({299.0F, 100.0F}, border, viewport, false) ==
          detail::LinuxResizeDirection::Right);
  REQUIRE(detail::ResolveLinuxResizeDirection({299.0F, 199.0F}, border, viewport, false) ==
          detail::LinuxResizeDirection::BottomRight);
  REQUIRE(detail::ResolveLinuxResizeDirection({150.0F, 199.0F}, border, viewport, false) ==
          detail::LinuxResizeDirection::Bottom);
  REQUIRE(detail::ResolveLinuxResizeDirection({0.0F, 199.0F}, border, viewport, false) ==
          detail::LinuxResizeDirection::BottomLeft);
  REQUIRE(detail::ResolveLinuxResizeDirection({0.0F, 100.0F}, border, viewport, false) ==
          detail::LinuxResizeDirection::Left);
  REQUIRE(detail::ResolveLinuxResizeDirection({150.0F, 100.0F}, border, viewport, false) ==
          detail::LinuxResizeDirection::None);

  REQUIRE(detail::ResolveLinuxResizeDirection({299.0F, 199.0F}, border, viewport, true) ==
          detail::LinuxResizeDirection::None);

  REQUIRE(detail::ResolveLinuxResizeDirection({0.0F, 100.0F}, 0.0F, viewport, false) ==
          detail::LinuxResizeDirection::None);

  const Rect caption_bounds{162.0F, 0.0F, 138.0F, 40.0F};
  REQUIRE(detail::ResolveLinuxResizeDirection({299.0F, 5.0F}, border, viewport, false, caption_bounds) ==
          detail::LinuxResizeDirection::None);
  REQUIRE(detail::ResolveLinuxResizeDirection({299.0F, 100.0F}, border, viewport, false, caption_bounds) ==
          detail::LinuxResizeDirection::Right);
  REQUIRE(detail::ResolveLinuxResizeDirection({150.0F, 0.0F}, border, viewport, false, caption_bounds) ==
          detail::LinuxResizeDirection::Top);

  REQUIRE(detail::ResolveLinuxResizeDirection({-10.0F, -10.0F}, border, viewport, false) ==
          detail::LinuxResizeDirection::TopLeft);
}

TEST_CASE("LinuxMaximizedStateRequiresBothAxes") {
  const Atom max_h = static_cast<Atom>(1);
  const Atom max_v = static_cast<Atom>(2);
  const Atom other = static_cast<Atom>(3);
  REQUIRE(detail::LinuxMaximizedFromAtoms({max_h, max_v}, max_h, max_v));
  REQUIRE_FALSE(detail::LinuxMaximizedFromAtoms({max_h}, max_h, max_v));
  REQUIRE_FALSE(detail::LinuxMaximizedFromAtoms({}, max_h, max_v));
  REQUIRE(detail::LinuxMaximizedFromAtoms({max_h, max_v, other}, max_h, max_v));
}

TEST_CASE("LinuxResizeBorderFallsBackWithoutFrameExtents") {
  REQUIRE(detail::LinuxResizeBorderDips({}, 1.0F, 6.0F) == 6.0F);
  REQUIRE(detail::LinuxResizeBorderDips({8, 8, 8, 8}, 2.0F, 6.0F) == 4.0F);
  REQUIRE(detail::LinuxResizeBorderDips({10, 6, 0, 0}, 1.0F, 6.0F) == 10.0F);

  const detail::LinuxFrameExtents empty = detail::LinuxReadFrameExtents(nullptr, 0);
  REQUIRE(empty.left == 0);
  REQUIRE(empty.right == 0);
  REQUIRE(empty.top == 0);
  REQUIRE(empty.bottom == 0);

  const long values[] = {1, 2, 3, 4};
  const detail::LinuxFrameExtents full = detail::LinuxReadFrameExtents(values, 4);
  REQUIRE(full.left == 1);
  REQUIRE(full.right == 2);
  REQUIRE(full.top == 3);
  REQUIRE(full.bottom == 4);

  const detail::LinuxFrameExtents partial = detail::LinuxReadFrameExtents(values, 2);
  REQUIRE(partial.left == 1);
  REQUIRE(partial.right == 2);
  REQUIRE(partial.top == 0);
  REQUIRE(partial.bottom == 0);
}

} // namespace huxerui::test
