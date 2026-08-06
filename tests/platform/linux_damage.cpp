#include <catch2/catch_amalgamated.hpp>

#include <limits>

#include "linux_internal.h"

namespace huxerui::test {
namespace {

TEST_CASE("LinuxDamageFullRegionForcesFullInvalidation") {
  const detail::LinuxDamageRegion full =
      detail::ResolveLinuxDamage({.full = true, .rects = {{1.0F, 2.0F, 3.0F, 4.0F}}}, 1.5F, 300, 200);
  REQUIRE(full.full);
  REQUIRE(full.rects.empty());
}

TEST_CASE("LinuxDamageScalesAndRoundsOutwardToPixels") {
  const detail::LinuxDamageRegion resolved =
      detail::ResolveLinuxDamage({.full = false, .rects = {{1.1F, 2.2F, 10.1F, 4.1F}}}, 1.5F, 300, 200);
  REQUIRE_FALSE(resolved.full);
  REQUIRE(resolved.rects.size() == 1);
  const XRectangle& rect = resolved.rects.front();
  REQUIRE(rect.x == 1);
  REQUIRE(rect.y == 3);
  REQUIRE(rect.width == 16);
  REQUIRE(rect.height == 7);
}

TEST_CASE("LinuxDamageClampsToClientBounds") {
  const detail::LinuxDamageRegion resolved =
      detail::ResolveLinuxDamage({.full = false, .rects = {{190.0F, 100.0F, 20.0F, 50.0F}}}, 1.5F, 300, 200);
  REQUIRE_FALSE(resolved.full);
  REQUIRE(resolved.rects.size() == 1);
  const XRectangle& rect = resolved.rects.front();
  REQUIRE(rect.x == 285);
  REQUIRE(rect.y == 150);
  REQUIRE(rect.width == 15);
  REQUIRE(rect.height == 50);
}

TEST_CASE("LinuxDamageRejectsInvalidScaleAndEmptyRects") {
  const detail::LinuxDamageRegion invalid_scale = detail::ResolveLinuxDamage(
      {.full = false, .rects = {{1.0F, 1.0F, 2.0F, 2.0F}}},
      std::numeric_limits<float>::quiet_NaN(),
      300,
      200
  );
  REQUIRE(invalid_scale.full);

  const detail::LinuxDamageRegion non_finite = detail::ResolveLinuxDamage(
      {.full = false, .rects = {{std::numeric_limits<float>::infinity(), 1.0F, 2.0F, 2.0F}}},
      1.5F,
      300,
      200
  );
  REQUIRE(non_finite.full);

  const detail::LinuxDamageRegion empty = detail::ResolveLinuxDamage(
      {.full = false, .rects = {{1.0F, 1.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 2.0F, 2.0F}}},
      1.5F,
      300,
      200
  );
  REQUIRE_FALSE(empty.full);
  REQUIRE(empty.rects.size() == 1);
}

TEST_CASE("LinuxDamageClampsNegativeRectanglesToZero") {
  const detail::LinuxDamageRegion resolved =
      detail::ResolveLinuxDamage({.full = false, .rects = {{-10.0F, -10.0F, 30.0F, 30.0F}}}, 1.0F, 100, 100);
  REQUIRE_FALSE(resolved.full);
  REQUIRE(resolved.rects.size() == 1);
  const XRectangle& rect = resolved.rects.front();
  REQUIRE(rect.x == 0);
  REQUIRE(rect.y == 0);
  REQUIRE(rect.width == 20);
  REQUIRE(rect.height == 20);
}

} // namespace
} // namespace huxerui::test
