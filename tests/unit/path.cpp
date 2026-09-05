#include <catch2/catch_amalgamated.hpp>

#include <huxerui/paint.h>

#include <cmath>
#include <concepts>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <utility>
#include <vector>

#include "graphics/path_internal.h"
#include "internal_access.h"

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
  REQUIRE_THROWS_AS(
      path.ArcTo({10.0F, 10.0F}, 0.0F, ArcSize::Small, ArcDirection::Clockwise, {30.0F, 20.0F}),
      std::logic_error
  );
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

TEST_CASE("PathArcToNormalizesEveryArcSelectionToCubics") {
  const auto arc = [](ArcSize size, ArcDirection direction) {
    Path path;
    path.MoveTo({10.0F, 0.0F}).ArcTo({10.0F, 10.0F}, 0.0F, size, direction, {0.0F, 10.0F});
    return path;
  };

  const Path clockwise_small = arc(ArcSize::Small, ArcDirection::Clockwise);
  const Path counterclockwise_small = arc(ArcSize::Small, ArcDirection::CounterClockwise);
  const Path clockwise_large = arc(ArcSize::Large, ArcDirection::Clockwise);
  const Path counterclockwise_large = arc(ArcSize::Large, ArcDirection::CounterClockwise);
  const auto clockwise_small_elements = detail::InternalAccess::Elements(clockwise_small);
  const auto counterclockwise_small_elements = detail::InternalAccess::Elements(counterclockwise_small);

  REQUIRE(clockwise_small_elements.size() == 2);
  REQUIRE(counterclockwise_small_elements.size() == 2);
  REQUIRE(detail::InternalAccess::Elements(clockwise_large).size() == 4);
  REQUIRE(detail::InternalAccess::Elements(counterclockwise_large).size() == 4);
  REQUIRE(clockwise_small_elements.back().verb == detail::PathVerb::CubicTo);
  REQUIRE(clockwise_small_elements.back().points[2] == Point{0.0F, 10.0F});
  REQUIRE(clockwise_small_elements.back().points[0].x == Catch::Approx(10.0F));
  REQUIRE(clockwise_small_elements.back().points[0].y == Catch::Approx(5.522847F));
  REQUIRE(counterclockwise_small_elements.back().points[0].x == Catch::Approx(4.477153F));
  REQUIRE(counterclockwise_small_elements.back().points[0].y == Catch::Approx(0.0F));
  REQUIRE(clockwise_small.Bounds() == Rect{0.0F, 0.0F, 10.0F, 10.0F});
  REQUIRE(counterclockwise_small.Bounds() == Rect{0.0F, 0.0F, 10.0F, 10.0F});
  REQUIRE(clockwise_large.Bounds() == Rect{0.0F, 0.0F, 20.0F, 20.0F});
  REQUIRE(counterclockwise_large.Bounds() == Rect{-10.0F, -10.0F, 20.0F, 20.0F});
}

TEST_CASE("PathArcToScalesUndersizedRadiiAndHandlesDegenerateSegments") {
  Path coincident;
  coincident.MoveTo({4.0F, 6.0F}).ArcTo(
      {20.0F, 10.0F}, 0.0F, ArcSize::Large, ArcDirection::Clockwise, {4.0F, 6.0F}
  );
  REQUIRE(coincident.IsEmpty());
  REQUIRE(detail::InternalAccess::Elements(coincident).size() == 1);

  Path line;
  line.MoveTo({4.0F, 6.0F}).ArcTo(
      {20.0F, 0.0F}, 0.0F, ArcSize::Large, ArcDirection::CounterClockwise, {24.0F, 16.0F}
  );
  REQUIRE(detail::InternalAccess::Elements(line).size() == 2);
  REQUIRE(detail::InternalAccess::Elements(line).back().verb == detail::PathVerb::LineTo);
  REQUIRE(line.Bounds() == Rect{4.0F, 6.0F, 20.0F, 10.0F});

  Path corrected;
  corrected.MoveTo({0.0F, 0.0F}).ArcTo(
      {10.0F, 5.0F}, 0.0F, ArcSize::Small, ArcDirection::Clockwise, {100.0F, 0.0F}
  );
  Path corrected_large;
  corrected_large.MoveTo({0.0F, 0.0F}).ArcTo(
      {10.0F, 5.0F}, 0.0F, ArcSize::Large, ArcDirection::Clockwise, {100.0F, 0.0F}
  );
  REQUIRE(detail::InternalAccess::Elements(corrected).size() == 3);
  REQUIRE(corrected == corrected_large);
  REQUIRE(corrected.Bounds().width == Catch::Approx(100.0F));
  REQUIRE(corrected.Bounds().height == Catch::Approx(25.0F));
}

TEST_CASE("PathArcToBuildsRotatedEllipsesWithCurveExtremaBounds") {
  constexpr float rotation = std::numbers::pi_v<float> * 0.25F;
  constexpr Point center{50.0F, 50.0F};
  constexpr float radius_x = 30.0F;
  constexpr float radius_y = 10.0F;
  const float axis = radius_x * std::cos(rotation);
  const Point start{center.x + axis, center.y + axis};
  const Point opposite{center.x - axis, center.y - axis};

  Path ellipse;
  ellipse.MoveTo(start)
      .ArcTo({radius_x, radius_y}, rotation, ArcSize::Small, ArcDirection::Clockwise, opposite)
      .ArcTo({radius_x, radius_y}, rotation, ArcSize::Small, ArcDirection::Clockwise, start)
      .Close();

  const auto elements = detail::InternalAccess::Elements(ellipse);
  REQUIRE(elements.size() == 6);
  REQUIRE(elements[1].verb == detail::PathVerb::CubicTo);
  REQUIRE(elements[2].verb == detail::PathVerb::CubicTo);
  REQUIRE(elements[3].verb == detail::PathVerb::CubicTo);
  REQUIRE(elements[4].verb == detail::PathVerb::CubicTo);
  REQUIRE(elements[5].verb == detail::PathVerb::Close);
  const float extent = std::sqrt(radius_x * radius_x * 0.5F + radius_y * radius_y * 0.5F);
  REQUIRE(ellipse.Bounds().x == Catch::Approx(center.x - extent).margin(0.01F));
  REQUIRE(ellipse.Bounds().y == Catch::Approx(center.y - extent).margin(0.01F));
  REQUIRE(ellipse.Bounds().width == Catch::Approx(extent * 2.0F).margin(0.02F));
  REQUIRE(ellipse.Bounds().height == Catch::Approx(extent * 2.0F).margin(0.02F));
}

TEST_CASE("PathArcToValidatesBeforeMutation") {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float infinity = std::numeric_limits<float>::infinity();
  Path empty;
  REQUIRE_THROWS_AS(
      empty.ArcTo({10.0F, 10.0F}, 0.0F, ArcSize::Small, ArcDirection::Clockwise, {10.0F, 0.0F}),
      std::logic_error
  );

  Path path;
  path.MoveTo({0.0F, 0.0F});
  const Path original = path;
  REQUIRE_THROWS_AS(
      path.ArcTo({-1.0F, 10.0F}, 0.0F, ArcSize::Small, ArcDirection::Clockwise, {10.0F, 0.0F}),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      path.ArcTo({infinity, 10.0F}, 0.0F, ArcSize::Small, ArcDirection::Clockwise, {10.0F, 0.0F}),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      path.ArcTo({10.0F, 10.0F}, infinity, ArcSize::Small, ArcDirection::Clockwise, {10.0F, 0.0F}),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      path.ArcTo({10.0F, 10.0F}, 0.0F, ArcSize::Small, ArcDirection::Clockwise, {nan, 0.0F}),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      path.ArcTo({10.0F, 10.0F}, 0.0F, static_cast<ArcSize>(20), ArcDirection::Clockwise, {10.0F, 0.0F}),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      path.ArcTo({10.0F, 10.0F}, 0.0F, ArcSize::Small, static_cast<ArcDirection>(20), {10.0F, 0.0F}),
      std::invalid_argument
  );
  REQUIRE(path == original);
}

TEST_CASE("PathArcToPreservesCopyOnWriteAndPaintSnapshots") {
  Path path;
  path.MoveTo({10.0F, 0.0F})
      .ArcTo({10.0F, 10.0F}, 0.0F, ArcSize::Small, ArcDirection::Clockwise, {0.0F, 10.0F});
  const Path copy = path;
  PaintSequence sequence;
  PaintContext context(sequence, {0.0F, 0.0F, 20.0F, 20.0F});
  context.FillPath(path, Color::Black());
  context.StrokePath(path, Color::Black(), StrokeStyle{.width = 2.0F, .dash_pattern = {2.0F, 1.0F}});
  context.DrawPathShadow(path, Color::Black(), {}, 2.0F);
  context.PushPathClip(path);
  context.PopClip();
  context.Finish();

  path.ArcTo({10.0F, 10.0F}, 0.0F, ArcSize::Large, ArcDirection::Clockwise, {10.0F, 0.0F});

  REQUIRE(path != copy);
  REQUIRE(sequence.Commands().size() == 5);
  REQUIRE(std::get<FillPathCommand>(sequence.Commands()[0]).path == copy);
  REQUIRE(std::get<StrokePathCommand>(sequence.Commands()[1]).path == copy);
  REQUIRE(std::get<StrokePathCommand>(sequence.Commands()[1]).style.dash_pattern == std::vector<float>{2.0F, 1.0F});
  REQUIRE(std::get<DrawPathShadowCommand>(sequence.Commands()[2]).path == copy);
  REQUIRE(std::get<PushPathClipCommand>(sequence.Commands()[3]).path == copy);
  REQUIRE(std::holds_alternative<PopClipCommand>(sequence.Commands()[4]));
  REQUIRE(detail::InternalAccess::Elements(copy).size() == 2);
  REQUIRE(detail::InternalAccess::Elements(path).size() == 5);
  path.Reset();
  REQUIRE(path == Path{});
}

TEST_CASE("PathContainsHandlesConvexConcaveAndImplicitlyClosedContours") {
  Path square;
  square.MoveTo({0.0F, 0.0F}).LineTo({100.0F, 0.0F}).LineTo({100.0F, 100.0F}).LineTo({0.0F, 100.0F});

  REQUIRE(square.Contains({50.0F, 50.0F}, PathFillRule::NonZero));
  REQUIRE(square.Contains({0.0F, 50.0F}, PathFillRule::NonZero));
  REQUIRE(square.Contains({0.0F, 0.0F}, PathFillRule::EvenOdd));
  REQUIRE_FALSE(square.Contains({110.0F, 50.0F}, PathFillRule::NonZero));

  Path concave;
  concave.MoveTo({0.0F, 0.0F})
      .LineTo({100.0F, 0.0F})
      .LineTo({100.0F, 30.0F})
      .LineTo({30.0F, 30.0F})
      .LineTo({30.0F, 100.0F})
      .LineTo({0.0F, 100.0F})
      .Close();

  REQUIRE(concave.Contains({80.0F, 10.0F}, PathFillRule::NonZero));
  REQUIRE(concave.Contains({10.0F, 80.0F}, PathFillRule::EvenOdd));
  REQUIRE_FALSE(concave.Contains({80.0F, 80.0F}, PathFillRule::NonZero));
}

TEST_CASE("PathContainsAppliesNonZeroAndEvenOddAcrossContours") {
  const auto append_clockwise_square = [](Path& path, float inset) {
    path.MoveTo({inset, inset})
        .LineTo({100.0F - inset, inset})
        .LineTo({100.0F - inset, 100.0F - inset})
        .LineTo({inset, 100.0F - inset})
        .Close();
  };
  const auto append_counterclockwise_square = [](Path& path, float inset) {
    path.MoveTo({inset, inset})
        .LineTo({inset, 100.0F - inset})
        .LineTo({100.0F - inset, 100.0F - inset})
        .LineTo({100.0F - inset, inset})
        .Close();
  };

  Path same_direction;
  append_clockwise_square(same_direction, 0.0F);
  append_clockwise_square(same_direction, 25.0F);
  REQUIRE(same_direction.Contains({50.0F, 50.0F}, PathFillRule::NonZero));
  REQUIRE_FALSE(same_direction.Contains({50.0F, 50.0F}, PathFillRule::EvenOdd));

  Path opposite_direction;
  append_clockwise_square(opposite_direction, 0.0F);
  append_counterclockwise_square(opposite_direction, 25.0F);
  REQUIRE_FALSE(opposite_direction.Contains({50.0F, 50.0F}, PathFillRule::NonZero));
  REQUIRE_FALSE(opposite_direction.Contains({50.0F, 50.0F}, PathFillRule::EvenOdd));

  Path overlapping;
  append_clockwise_square(overlapping, 0.0F);
  overlapping.MoveTo({50.0F, 0.0F})
      .LineTo({150.0F, 0.0F})
      .LineTo({150.0F, 100.0F})
      .LineTo({50.0F, 100.0F})
      .Close();
  REQUIRE(overlapping.Contains({75.0F, 50.0F}, PathFillRule::NonZero));
  REQUIRE_FALSE(overlapping.Contains({75.0F, 50.0F}, PathFillRule::EvenOdd));
  REQUIRE(overlapping.Contains({50.0F, 50.0F}, PathFillRule::EvenOdd));
}

TEST_CASE("PathContainsSubdividesQuadraticCubicAndNormalizedArcGeometry") {
  Path quadratic;
  quadratic.MoveTo({0.0F, 0.0F}).QuadraticTo({50.0F, 100.0F}, {100.0F, 0.0F});
  REQUIRE(quadratic.Contains({50.0F, 25.0F}, PathFillRule::NonZero));
  REQUIRE(quadratic.Contains({50.0F, 50.0F}, PathFillRule::EvenOdd));
  REQUIRE_FALSE(quadratic.Contains({50.0F, 51.0F}, PathFillRule::NonZero));

  Path collinear_quadratic;
  collinear_quadratic.MoveTo({0.0F, 0.0F}).QuadraticTo({200.0F, 0.0F}, {100.0F, 0.0F});
  REQUIRE(collinear_quadratic.Contains({125.0F, 0.0F}, PathFillRule::NonZero));

  Path cubic;
  cubic.MoveTo({0.0F, 0.0F}).CubicTo({0.0F, 100.0F}, {100.0F, 100.0F}, {100.0F, 0.0F});
  REQUIRE(cubic.Contains({50.0F, 50.0F}, PathFillRule::NonZero));
  REQUIRE(cubic.Contains({50.0F, 75.0F}, PathFillRule::EvenOdd));
  REQUIRE_FALSE(cubic.Contains({50.0F, 76.0F}, PathFillRule::NonZero));

  Path ellipse;
  ellipse.MoveTo({10.0F, 0.0F})
      .ArcTo({10.0F, 5.0F}, 0.0F, ArcSize::Small, ArcDirection::Clockwise, {-10.0F, 0.0F})
      .ArcTo({10.0F, 5.0F}, 0.0F, ArcSize::Small, ArcDirection::Clockwise, {10.0F, 0.0F})
      .Close();
  REQUIRE(ellipse.Contains({0.0F, 0.0F}, PathFillRule::NonZero));
  REQUIRE(ellipse.Contains({10.0F, 0.0F}, PathFillRule::EvenOdd));
  REQUIRE_FALSE(ellipse.Contains({11.0F, 0.0F}, PathFillRule::NonZero));
}

TEST_CASE("PathContainsDefinesEmptyAndDegenerateBoundaries") {
  const Path empty;
  Path move_only;
  move_only.MoveTo({4.0F, 6.0F});
  REQUIRE_FALSE(empty.Contains({}, PathFillRule::NonZero));
  REQUIRE_FALSE(move_only.Contains({4.0F, 6.0F}, PathFillRule::EvenOdd));

  Path line;
  line.MoveTo({0.0F, 0.0F}).LineTo({10.0F, 0.0F});
  REQUIRE(line.Contains({5.0F, 0.0F}, PathFillRule::NonZero));
  REQUIRE(line.Contains({5.0F, 0.00005F}, PathFillRule::EvenOdd));
  REQUIRE_FALSE(line.Contains({5.0F, 0.001F}, PathFillRule::NonZero));

  Path point_segment;
  point_segment.MoveTo({3.0F, 4.0F}).LineTo({3.0F, 4.0F});
  REQUIRE(point_segment.Contains({3.0F, 4.0F}, PathFillRule::NonZero));
  REQUIRE_FALSE(point_segment.Contains({3.0F, 4.01F}, PathFillRule::EvenOdd));

  Path closed_point;
  closed_point.MoveTo({3.0F, 4.0F}).Close();
  REQUIRE_FALSE(closed_point.Contains({3.0F, 4.0F}, PathFillRule::NonZero));
}

TEST_CASE("PathContainsValidatesWithoutMutatingPathOrSnapshots") {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float infinity = std::numeric_limits<float>::infinity();
  Path path = Path::RoundedRect({0.0F, 0.0F, 20.0F, 20.0F}, CornerRadii{4.0F});
  const Path copy = path;
  const auto* storage = detail::InternalAccess::Elements(path).data();
  PaintSequence sequence;
  PaintContext context(sequence, {0.0F, 0.0F, 20.0F, 20.0F});
  context.FillPath(path, Color::Black());
  context.Finish();

  REQUIRE_THROWS_AS(path.Contains({nan, 0.0F}, PathFillRule::NonZero), std::invalid_argument);
  REQUIRE_THROWS_AS(path.Contains({0.0F, infinity}, PathFillRule::EvenOdd), std::invalid_argument);
  REQUIRE_THROWS_AS(path.Contains({}, static_cast<PathFillRule>(20)), std::invalid_argument);
  REQUIRE(path.Contains({10.0F, 10.0F}, PathFillRule::NonZero));
  REQUIRE(path == copy);
  REQUIRE(detail::InternalAccess::Elements(path).data() == storage);

  path.MoveTo({30.0F, 30.0F}).LineTo({40.0F, 40.0F});
  REQUIRE(path != copy);
  REQUIRE(std::get<FillPathCommand>(sequence.Commands()[0]).path == copy);
}

TEST_CASE("RoundedRectUsesCubicCircularCorners") {
  const Path path = Path::RoundedRect({10.0F, 20.0F, 80.0F, 60.0F}, CornerRadii::Top(16.0F));
  const auto elements = detail::InternalAccess::Elements(path);

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
  const auto elements = detail::InternalAccess::Elements(path);

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
