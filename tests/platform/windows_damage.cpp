#include <catch2/catch_amalgamated.hpp>

#include <limits>

#include "win32_damage_internal.h"

namespace huxerui::test {

namespace {

void RequireRect(const RECT& rect, LONG left, LONG top, LONG right, LONG bottom) {
  REQUIRE(rect.left == left);
  REQUIRE(rect.top == top);
  REQUIRE(rect.right == right);
  REQUIRE(rect.bottom == bottom);
}

} // namespace

TEST_CASE("TestWin32DamageRoundsOutwardAndClipsToClient") {
  DamageRegion damage;
  damage.rects = {
      {1.1F, 2.2F, 10.1F, 4.1F},
      {190.0F, 100.0F, 20.0F, 50.0F},
      {20.0F, 20.0F, 0.0F, 10.0F},
  };
  const RECT client{0, 0, 300, 200};

  const detail::Win32DamageRegion resolved = detail::ResolveWin32Damage(damage, 1.5F, client);

  REQUIRE_FALSE(resolved.full);
  REQUIRE(resolved.rects.size() == 2);
  RequireRect(resolved.rects[0], 1, 3, 17, 10);
  RequireRect(resolved.rects[1], 285, 150, 300, 200);
}

TEST_CASE("TestWin32DamageUsesFullFallbackForUnsafeInput") {
  const RECT client{0, 0, 300, 200};

  DamageRegion full_damage;
  full_damage.full = true;
  REQUIRE(detail::ResolveWin32Damage(full_damage, 1.0F, client).full);

  DamageRegion invalid_damage;
  invalid_damage.rects = {{0.0F, 0.0F, std::numeric_limits<float>::infinity(), 10.0F}};
  REQUIRE(detail::ResolveWin32Damage(invalid_damage, 1.0F, client).full);
  REQUIRE(detail::ResolveWin32Damage({}, 0.0F, client).full);
}

TEST_CASE("TestWin32PaintRectConvertsBackToDips") {
  const RECT pixels{15, 30, 165, 90};
  REQUIRE(detail::Win32PixelRectToDips(pixels, 1.5F) == Rect{10.0F, 20.0F, 100.0F, 40.0F});

  const RECT client{0, 0, 300, 200};
  REQUIRE(detail::Win32RectCovers(client, client));
  REQUIRE_FALSE(detail::Win32RectCovers(RECT{0, 0, 299, 200}, client));
}

} // namespace huxerui::test
