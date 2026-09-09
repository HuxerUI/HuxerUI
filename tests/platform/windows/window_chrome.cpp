#include <catch2/catch_amalgamated.hpp>

#include "win32_internal.h"

namespace huxerui::test {

TEST_CASE("Win32MaximizedClientRectStaysInsideTheWorkArea") {
  const RECT proposed_window{-8, -8, 1928, 1048};
  const RECT client = detail::InsetWin32MaximizedClientRect(proposed_window, 8, 8);
  REQUIRE(client.left == 0);
  REQUIRE(client.top == 0);
  REQUIRE(client.right == 1920);
  REQUIRE(client.bottom == 1040);
}

TEST_CASE("Win32CaptionButtonsUseTheModernMinimumWidth") {
  REQUIRE(detail::ResolveWin32CaptionButtonWidth(36.0F) == 46.0F);
  REQUIRE(detail::ResolveWin32CaptionButtonWidth(52.0F) == 52.0F);
}

TEST_CASE("Win32TitleBarMetricsStayInsideNarrowClientBounds") {
  const WindowTitleBarMetrics constrained = detail::ConstrainWin32TitleBarMetrics(
      WindowTitleBarMetrics{.height = 48.0F, .left_inset = 10.0F, .right_inset = 138.0F, .maximized = true},
      {100.0F, 32.0F}
  );

  REQUIRE(constrained.height == 32.0F);
  REQUIRE(constrained.left_inset == 10.0F);
  REQUIRE(constrained.right_inset == 90.0F);
  REQUIRE(constrained.maximized);
}

TEST_CASE("Win32MinimumTrackSizePreservesClientSizeAtTheCurrentScale") {
  const POINT minimum = detail::ResolveWin32MinimumTrackSize(
      {640.0F, 480.0F}, 1.25F, SIZE{16, 39}, POINT{160, 120}
  );

  REQUIRE(minimum.x == 816);
  REQUIRE(minimum.y == 639);
}

TEST_CASE("Win32MinimumTrackSizePreservesLargerNativeLimits") {
  const POINT minimum = detail::ResolveWin32MinimumTrackSize(
      {320.0F, 240.0F}, 1.0F, SIZE{}, POINT{500, 300}
  );

  REQUIRE(minimum.x == 500);
  REQUIRE(minimum.y == 300);
}

} // namespace huxerui::test
