#include <catch2/catch_amalgamated.hpp>

#include <huxerui/paint.h>

#include <concepts>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <variant>

namespace huxerui::test {

static_assert(std::equality_comparable<Color>);
static_assert(std::equality_comparable<Rect>);
static_assert(std::equality_comparable<PaintCommand>);

TEST_CASE("PaintCommandsCompareByValue") {
  const PaintCommand left = DrawRectCommand{
      {1.0F, 2.0F, 3.0F, 4.0F},
      Color::White(),
      5.0F,
  };
  const PaintCommand equal = DrawRectCommand{
      {1.0F, 2.0F, 3.0F, 4.0F},
      Color::White(),
      5.0F,
  };
  const PaintCommand different = DrawRectCommand{
      {1.0F, 2.0F, 3.0F, 4.0F},
      Color::Black(),
      5.0F,
  };

  REQUIRE(left == equal);
  REQUIRE(left != different);
}

TEST_CASE("PaintContextBuildsAnImmutableLocalSequence") {
  PaintSequence sequence;
  REQUIRE(sequence.Revision() == 0);
  PaintContext context{sequence, Rect{0.0F, 0.0F, 100.0F, 80.0F}};
  context.DrawRect({10.0F, 20.0F, 30.0F, 15.0F}, Color::White(), 4.0F);
  context.DrawBorder({5.0F, 10.0F, 20.0F, 20.0F}, Color::Black(), 2.0F);
  context.Finish();

  REQUIRE(sequence.Revision() == 1);
  REQUIRE(sequence.Commands().size() == 2);
  REQUIRE(std::holds_alternative<DrawRectCommand>(sequence.Commands()[0]));
  REQUIRE(std::holds_alternative<DrawBorderCommand>(sequence.Commands()[1]));
  REQUIRE(sequence.Bounds().x == 4.0F);
  REQUIRE(sequence.Bounds().y == 9.0F);
  REQUIRE(sequence.Bounds().width == 36.0F);
  REQUIRE(sequence.Bounds().height == 26.0F);
  REQUIRE_THROWS_AS(context.DrawRect({}, Color::White()), std::logic_error);
}

TEST_CASE("PaintContextRejectsUnbalancedCommandStacks") {
  PaintSequence sequence;
  PaintContext context{sequence, Rect{0.0F, 0.0F, 100.0F, 80.0F}};
  REQUIRE_THROWS_AS(context.PopClip(), std::logic_error);
  context.PushClip({0.0F, 0.0F, 10.0F, 10.0F});
  REQUIRE_THROWS_AS(context.Finish(), std::logic_error);
}

TEST_CASE("PaintContextRejectsCrossedCommandStacks") {
  PaintSequence sequence;
  PaintContext context{sequence, Rect{0.0F, 0.0F, 100.0F, 80.0F}};
  context.PushClip({0.0F, 0.0F, 10.0F, 10.0F});
  context.PushTransform(Transform2D{});
  REQUIRE_THROWS_AS(context.PopClip(), std::logic_error);
}

TEST_CASE("PaintContextTracksTransformedAndClippedBounds") {
  PaintSequence sequence;
  PaintContext context{sequence, Rect{0.0F, 0.0F, 100.0F, 80.0F}};
  context.PushTransform(Transform2D{1.0F, 0.0F, 0.0F, 1.0F, 40.0F, 20.0F});
  context.PushClip({0.0F, 0.0F, 20.0F, 20.0F});
  context.DrawRect({10.0F, 10.0F, 30.0F, 30.0F}, Color::White());
  context.PopClip();
  context.PopTransform();
  context.Finish();

  REQUIRE(sequence.Bounds() == Rect{50.0F, 30.0F, 10.0F, 10.0F});
}

TEST_CASE("PaintContextIncludesSquareArcCapsInBounds") {
  PaintSequence sequence;
  PaintContext context{sequence, Rect{0.0F, 0.0F, 100.0F, 80.0F}};
  context
      .DrawArc({50.0F, 40.0F}, 10.0F, 0.0F, std::numbers::pi_v<float> * 0.5F, Color::White(), 4.0F, StrokeCap::Square);
  context.Finish();

  REQUIRE(sequence.Bounds().x < 38.0F);
  REQUIRE(sequence.Bounds().y < 28.0F);
  REQUIRE(sequence.Bounds().x + sequence.Bounds().width > 62.0F);
  REQUIRE(sequence.Bounds().y + sequence.Bounds().height > 52.0F);
}

TEST_CASE("PaintContextRejectsInvalidDrawingParameters") {
  const float nan = std::numeric_limits<float>::quiet_NaN();

  PaintSequence sequence;
  REQUIRE_THROWS_AS(PaintContext(sequence, Rect{0.0F, 0.0F, -1.0F, 20.0F}), std::invalid_argument);

  PaintContext context{sequence, Rect{0.0F, 0.0F, 100.0F, 80.0F}};
  REQUIRE_THROWS_AS(context.DrawRect({0.0F, 0.0F, nan, 10.0F}, Color::White()), std::invalid_argument);
  REQUIRE_THROWS_AS(context.DrawText({}, "Text", Color::White(), 0.0F), std::invalid_argument);
  REQUIRE_THROWS_AS(context.DrawCircle({}, -1.0F, Color::White()), std::invalid_argument);
  REQUIRE_THROWS_AS(context.DrawArc({}, 10.0F, 0.0F, nan, Color::White(), 1.0F), std::invalid_argument);
  REQUIRE_THROWS_AS(context.DrawBorder({}, Color::White(), -1.0F), std::invalid_argument);
  REQUIRE_THROWS_AS(context.PushClip({}, -1.0F), std::invalid_argument);
  REQUIRE_THROWS_AS(context.PushTransform(Transform2D{1.0F, 0.0F, 0.0F, 1.0F, nan, 0.0F}), std::invalid_argument);
  context.Finish();
}

} // namespace huxerui::test
