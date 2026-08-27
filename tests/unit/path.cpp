#include <catch2/catch_amalgamated.hpp>

#include <huxerui/paint.h>

#include <concepts>
#include <limits>
#include <stdexcept>
#include <utility>

#include "path_internal.h"

namespace huxerui::test {

static_assert(std::equality_comparable<Path>);

TEST_CASE("PathCopiesDetachBeforeMutation") {
  Path original;
  original.MoveTo({0.0F, 0.0F}).LineTo({20.0F, 10.0F});
  Path copy = original;

  REQUIRE(copy == original);
  original.LineTo({40.0F, 30.0F});

  REQUIRE_FALSE(copy == original);
  REQUIRE(copy.Bounds() == Rect{0.0F, 0.0F, 20.0F, 10.0F});
  REQUIRE(original.Bounds() == Rect{0.0F, 0.0F, 40.0F, 30.0F});
}

TEST_CASE("PathBoundsIncludeCurveExtrema") {
  Path path;
  path.MoveTo({0.0F, 0.0F}).QuadraticTo({50.0F, 100.0F}, {100.0F, 0.0F});

  REQUIRE(path.Bounds() == Rect{0.0F, 0.0F, 100.0F, 50.0F});
}

TEST_CASE("PathBoundsIgnoreMoveOnlyContours") {
  Path path;
  path.MoveTo({1000.0F, 1000.0F});
  REQUIRE(path.IsEmpty());
  REQUIRE(path.Bounds().IsEmpty());

  path.MoveTo({10.0F, 20.0F}).LineTo({40.0F, 50.0F}).MoveTo({2000.0F, 2000.0F});
  REQUIRE(path.Bounds() == Rect{10.0F, 20.0F, 30.0F, 30.0F});
}

TEST_CASE("ClosingPathRequiresMoveToBeforeAnotherContour") {
  Path path;
  path.MoveTo({0.0F, 0.0F}).LineTo({20.0F, 10.0F}).Close();

  REQUIRE_THROWS_AS(path.LineTo({30.0F, 20.0F}), std::logic_error);
  REQUIRE_THROWS_AS(path.QuadraticTo({}, {}), std::logic_error);
  REQUIRE_THROWS_AS(path.CubicTo({}, {}, {}), std::logic_error);
  REQUIRE_THROWS_AS(path.Close(), std::logic_error);

  REQUIRE_NOTHROW(path.MoveTo({30.0F, 20.0F}).LineTo({40.0F, 30.0F}));
}

TEST_CASE("PathRejectsInvalidGeometryAndContourOperations") {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  Path path;

  REQUIRE_THROWS_AS(path.LineTo({1.0F, 1.0F}), std::logic_error);
  REQUIRE_THROWS_AS(path.Close(), std::logic_error);
  REQUIRE_THROWS_AS(path.MoveTo({nan, 0.0F}), std::invalid_argument);
  path.MoveTo({0.0F, 0.0F});
  REQUIRE_THROWS_AS(path.CubicTo({}, {nan, 0.0F}, {}), std::invalid_argument);
}

TEST_CASE("RoundedRectUsesCubicCircularCorners") {
  const Path path = Path::RoundedRect({10.0F, 20.0F, 80.0F, 60.0F}, CornerRadii::Top(16.0F));
  const auto elements = detail::PathAccess::Elements(path);

  REQUIRE(elements.size() == 10);
  REQUIRE(elements[2].verb == detail::PathVerb::CubicTo);
  REQUIRE(elements[4].verb == detail::PathVerb::CubicTo);
  REQUIRE(elements[6].verb == detail::PathVerb::CubicTo);
  REQUIRE(elements[8].verb == detail::PathVerb::CubicTo);
  REQUIRE(path.Bounds() == Rect{10.0F, 20.0F, 80.0F, 60.0F});
}

TEST_CASE("RoundedRectScalesOverconstrainedCornerRadiiProportionally") {
  constexpr float scale = 2.0F / 7.0F;
  const Path path = Path::RoundedRect({0.0F, 0.0F, 120.0F, 40.0F}, {80.0F, 40.0F, 20.0F, 60.0F});
  const auto elements = detail::PathAccess::Elements(path);

  REQUIRE(elements[0].points[0].x == Catch::Approx(80.0F * scale));
  REQUIRE(elements[1].points[0].x == Catch::Approx(120.0F - 40.0F * scale));
  REQUIRE(elements[3].points[0].y == Catch::Approx(40.0F - 20.0F * scale));
  REQUIRE(elements[5].points[0].x == Catch::Approx(60.0F * scale));
  REQUIRE(path.Bounds() == Rect{0.0F, 0.0F, 120.0F, 40.0F});
}

TEST_CASE("MovedFromPathRemainsAnEmptyReusableValue") {
  Path source;
  source.MoveTo({1.0F, 2.0F}).LineTo({3.0F, 4.0F});
  Path moved = std::move(source);

  REQUIRE_FALSE(moved.IsEmpty());
  REQUIRE(source.IsEmpty());
  REQUIRE(source.Bounds().IsEmpty());
  REQUIRE(source == Path{});

  source.MoveTo({5.0F, 6.0F}).LineTo({7.0F, 8.0F});
  REQUIRE(source.Bounds() == Rect{5.0F, 6.0F, 2.0F, 2.0F});
}

} // namespace huxerui::test
