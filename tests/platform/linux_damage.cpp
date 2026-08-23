#include <catch2/catch_amalgamated.hpp>

#include <limits>
#include <optional>

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

TEST_CASE("LinuxDamagePreservesAnEmptyRegion") {
  const detail::LinuxDamageRegion resolved = detail::ResolveLinuxDamage({}, 1.0F, 100, 100);
  REQUIRE_FALSE(resolved.full);
  REQUIRE(resolved.rects.empty());
}

TEST_CASE("LinuxFrameRenderActionSkipsUndamagedCommits") {
  const detail::LinuxDamageRegion empty;
  REQUIRE(detail::ResolveLinuxFrameRenderAction(empty, false, false) == detail::LinuxFrameRenderAction::Skip);
  REQUIRE(detail::ResolveLinuxFrameRenderAction(empty, false, true) == detail::LinuxFrameRenderAction::Skip);
}

TEST_CASE("LinuxFrameRenderActionPresentsRetainedContentAfterExpose") {
  const detail::LinuxDamageRegion empty;
  REQUIRE(detail::ResolveLinuxFrameRenderAction(empty, true, true) == detail::LinuxFrameRenderAction::PresentRetained);
  REQUIRE(detail::ResolveLinuxFrameRenderAction(empty, true, false) == detail::LinuxFrameRenderAction::Repaint);
}

TEST_CASE("LinuxFrameRenderActionRepaintsDamage") {
  const detail::LinuxDamageRegion full{.full = true};
  const detail::LinuxDamageRegion partial{.rects = {{1, 2, 3, 4}}};
  REQUIRE(detail::ResolveLinuxFrameRenderAction(full, false, true) == detail::LinuxFrameRenderAction::Repaint);
  REQUIRE(detail::ResolveLinuxFrameRenderAction(partial, false, true) == detail::LinuxFrameRenderAction::Repaint);
}

TEST_CASE("LinuxPollTimeoutRoundsPositiveDeadlinesUp") {
  REQUIRE(detail::ResolveLinuxPollTimeout(1.0001, 1.0) == 1);
  REQUIRE(detail::ResolveLinuxPollTimeout(1.0162, 1.0) == 17);
}

TEST_CASE("LinuxPollTimeoutHandlesDueAndUnboundedDeadlines") {
  REQUIRE(detail::ResolveLinuxPollTimeout(1.0, 1.0) == 0);
  REQUIRE(detail::ResolveLinuxPollTimeout(0.5, 1.0) == 0);
  REQUIRE(detail::ResolveLinuxPollTimeout(std::nullopt, 1.0) == -1);
  REQUIRE(
      detail::ResolveLinuxPollTimeout(std::numeric_limits<double>::infinity(), 1.0) == std::numeric_limits<int>::max()
  );
  REQUIRE(detail::ResolveLinuxPollTimeout(std::numeric_limits<double>::max(), 1.0) == std::numeric_limits<int>::max());
}

TEST_CASE("LinuxTextureUploadKeepsMultipleSmallDamageRectanglesPartial") {
  const std::vector<XRectangle> damage{{10, 20, 30, 40}, {200, 100, 20, 10}};
  const detail::LinuxTextureUploadPlan plan = detail::ResolveLinuxTextureUpload(false, damage, 800, 600, true);
  REQUIRE_FALSE(plan.full);
  REQUIRE(plan.rects.size() == 2);
  REQUIRE(plan.rects[0].x == 10);
  REQUIRE(plan.rects[0].y == 20);
  REQUIRE(plan.rects[0].width == 30);
  REQUIRE(plan.rects[0].height == 40);
  REQUIRE(plan.rects[1].x == 200);
  REQUIRE(plan.rects[1].y == 100);
  REQUIRE(plan.rects[1].width == 20);
  REQUIRE(plan.rects[1].height == 10);
  REQUIRE(plan.pixel_count == 1400);
}

TEST_CASE("LinuxTextureUploadUsesFullSurfaceForLargeDamage") {
  const std::vector<XRectangle> damage{{0, 0, 800, 400}};
  const detail::LinuxTextureUploadPlan plan = detail::ResolveLinuxTextureUpload(false, damage, 800, 600, true);
  REQUIRE(plan.full);
  REQUIRE(plan.rects.empty());
  REQUIRE(plan.pixel_count == 480000);
}

TEST_CASE("LinuxTextureUploadInitializesNewTextureCompletely") {
  const std::vector<XRectangle> damage{{10, 20, 30, 40}};
  const detail::LinuxTextureUploadPlan plan = detail::ResolveLinuxTextureUpload(false, damage, 800, 600, false);
  REQUIRE(plan.full);
  REQUIRE(plan.pixel_count == 480000);
}

TEST_CASE("LinuxTextureUploadClampsDamageToTheSurface") {
  const std::vector<XRectangle> damage{{-10, -5, 30, 20}};
  const detail::LinuxTextureUploadPlan plan = detail::ResolveLinuxTextureUpload(false, damage, 100, 100, true);
  REQUIRE_FALSE(plan.full);
  REQUIRE(plan.rects.size() == 1);
  REQUIRE(plan.rects[0].x == 0);
  REQUIRE(plan.rects[0].y == 0);
  REQUIRE(plan.rects[0].width == 20);
  REQUIRE(plan.rects[0].height == 15);
  REQUIRE(plan.pixel_count == 300);
}

} // namespace
} // namespace huxerui::test
