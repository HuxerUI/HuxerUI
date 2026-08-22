#include <catch2/catch_amalgamated.hpp>

#include <huxerui/paint.h>

#include <concepts>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <variant>
#include <vector>

#include "external_texture_test_support.h"
#include "image_test_support.h"
#include "shadow_internal.h"

namespace huxerui::test {

namespace {

ImageAsset TestImage() {
  return ImageAsset::FromEncoded(MakeTestPng(40, 20), 2.0F);
}

ExternalTexture TestExternalTexture() {
  return MakeTestExternalTexture({20.0F, 10.0F});
}

} // namespace

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
  REQUIRE_FALSE(sequence.HasExternalTextureCommands());
  REQUIRE(sequence.Commands().size() == 2);
  REQUIRE(std::holds_alternative<DrawRectCommand>(sequence.Commands()[0]));
  REQUIRE(std::holds_alternative<DrawBorderCommand>(sequence.Commands()[1]));
  REQUIRE(sequence.Bounds().x == 4.0F);
  REQUIRE(sequence.Bounds().y == 9.0F);
  REQUIRE(sequence.Bounds().width == 36.0F);
  REQUIRE(sequence.Bounds().height == 26.0F);
  REQUIRE_THROWS_AS(context.DrawRect({}, Color::White()), std::logic_error);
}

TEST_CASE("PaintContextRecordsPlatformNeutralGradients") {
  const LinearGradient linear{
      .start = {0.0F, 0.0F},
      .end = {1.0F, 1.0F},
      .stops = {{0.0F, Color::Black()}, {1.0F, Color::White()}},
  };
  const RadialGradient radial{
      .center = {0.5F, 0.5F},
      .radius = {0.5F, 0.25F},
      .stops = {{0.0F, Color::White()}, {1.0F, Color::Transparent()}},
  };
  PaintSequence sequence;
  PaintContext context{sequence, Rect{0.0F, 0.0F, 100.0F, 80.0F}};
  context.DrawLinearGradient({10.0F, 12.0F, 40.0F, 20.0F}, linear, CornerRadii{6.0F});
  context.DrawRadialGradient({20.0F, 30.0F, 50.0F, 30.0F}, radial, CornerRadii{8.0F});
  context.Finish();

  REQUIRE(sequence.Commands().size() == 2);
  const auto& linear_command = std::get<DrawLinearGradientCommand>(sequence.Commands()[0]);
  const auto& radial_command = std::get<DrawRadialGradientCommand>(sequence.Commands()[1]);
  REQUIRE(linear_command.gradient == linear);
  REQUIRE(linear_command.corner_radius == 6.0F);
  REQUIRE(radial_command.gradient == radial);
  REQUIRE(radial_command.corner_radius == 8.0F);
  REQUIRE(sequence.Bounds() == Rect{10.0F, 12.0F, 60.0F, 48.0F});
}

TEST_CASE("PaintContextClipsAsymmetricGradientCornersWithoutApplyingASecondUniformRadius") {
  PaintSequence sequence;
  PaintContext context{sequence, Rect{0.0F, 0.0F, 100.0F, 80.0F}};
  context.DrawLinearGradient(
      {10.0F, 12.0F, 40.0F, 20.0F},
      LinearGradient{.stops = {{0.0F, Color::Black()}, {1.0F, Color::White()}}},
      CornerRadii{8.0F, 4.0F, 2.0F, 0.0F}
  );
  context.Finish();

  REQUIRE(sequence.Commands().size() == 3);
  REQUIRE(std::holds_alternative<PushClipCommand>(sequence.Commands()[0]));
  REQUIRE(std::get<DrawLinearGradientCommand>(sequence.Commands()[1]).corner_radius == 0.0F);
  REQUIRE(std::holds_alternative<PopClipCommand>(sequence.Commands()[2]));
}

TEST_CASE("PaintContextValidatesGradientGeometryAndStops") {
  PaintSequence sequence;
  PaintContext context{sequence, Rect{0.0F, 0.0F, 100.0F, 80.0F}};
  REQUIRE_THROWS_AS(context.DrawLinearGradient({}, LinearGradient{}), std::invalid_argument);
  REQUIRE_THROWS_AS(
      context.DrawLinearGradient(
          {},
          LinearGradient{.stops = {{0.8F, Color::White()}, {0.2F, Color::Black()}}}
      ),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      context.DrawRadialGradient(
          {},
          RadialGradient{.radius = {0.0F, 0.5F}, .stops = {{0.0F, Color::White()}}}
      ),
      std::invalid_argument
  );
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

TEST_CASE("PaintContextCoalescesAdjacentTextRuns") {
  PaintSequence sequence;
  PaintContext context{sequence, Rect{0.0F, 0.0F, 100.0F, 80.0F}};
  const TextStyle style{Font::Monospace(14.0F), Color::White(), TextDecoration::Underline};
  context.DrawTextRun({10.0F, 12.0F, 20.0F, 16.0F}, {10.0F, 24.0F}, "first", style);
  context.DrawTextRun({30.0F, 12.0F, 24.0F, 16.0F}, {30.0F, 24.0F}, "second", style);
  context.DrawTextRuns({TextRun{{54.0F, 12.0F, 12.0F, 16.0F}, {54.0F, 24.0F}, "third", style, {}}});
  context.Finish();

  REQUIRE(sequence.Commands().size() == 1);
  const auto* command = std::get_if<DrawTextRunsCommand>(&sequence.Commands().front());
  REQUIRE(command != nullptr);
  REQUIRE(command->runs.size() == 3);
  REQUIRE(command->runs[0].baseline_origin == Point{10.0F, 24.0F});
  REQUIRE(command->runs[1].bounds == Rect{30.0F, 12.0F, 24.0F, 16.0F});
  REQUIRE(sequence.Bounds() == Rect{10.0F, 12.0F, 56.0F, 16.0F});
}

TEST_CASE("PaintContextRecordsResolvedImageGeometry") {
  PaintSequence sequence;
  PaintContext context{sequence, Rect{0.0F, 0.0F, 100.0F, 80.0F}};
  context
      .DrawImageRect(TestImage(), {2.0F, 1.0F, 8.0F, 4.0F}, {10.0F, 20.0F, 40.0F, 20.0F}, ImageSampling::Nearest, 0.5F);
  context.Finish();

  REQUIRE(sequence.Commands().size() == 1);
  const auto& command = std::get<DrawImageCommand>(sequence.Commands().front());
  REQUIRE(command.source == Rect{2.0F, 1.0F, 8.0F, 4.0F});
  REQUIRE(command.destination == Rect{10.0F, 20.0F, 40.0F, 20.0F});
  REQUIRE(command.sampling == ImageSampling::Nearest);
  REQUIRE(command.opacity == 0.5F);
  REQUIRE(sequence.Bounds() == command.destination);
}

TEST_CASE("PaintContextValidatesImageSourceAndOpacity") {
  PaintSequence sequence;
  PaintContext context{sequence, Rect{0.0F, 0.0F, 100.0F, 80.0F}};
  REQUIRE_THROWS_AS(context.DrawImageRect(TestImage(), {19.0F, 0.0F, 2.0F, 2.0F}, {}), std::invalid_argument);
  REQUIRE_THROWS_AS(
      context.DrawImage(TestImage(), {0.0F, 0.0F, 10.0F, 10.0F}, ImageSampling::Linear, 1.1F),
      std::invalid_argument
  );
}

TEST_CASE("PaintContextRecordsExternalTexturesWithoutFlatteningThemToImages") {
  const ExternalTexture texture = TestExternalTexture();
  PaintSequence sequence;
  PaintContext context{sequence, Rect{0.0F, 0.0F, 100.0F, 80.0F}};
  context.DrawImageRect(
      texture,
      {2.0F, 1.0F, 8.0F, 4.0F},
      {10.0F, 20.0F, 40.0F, 20.0F},
      ImageSampling::Nearest,
      0.5F
  );
  context.Finish();

  REQUIRE(sequence.Commands().size() == 1);
  REQUIRE(sequence.HasExternalTextureCommands());
  const auto& command = std::get<DrawExternalTextureCommand>(sequence.Commands().front());
  REQUIRE(command.texture == texture);
  REQUIRE(command.source == Rect{2.0F, 1.0F, 8.0F, 4.0F});
  REQUIRE(command.destination == Rect{10.0F, 20.0F, 40.0F, 20.0F});
  REQUIRE(command.sampling == ImageSampling::Nearest);
  REQUIRE(command.opacity == 0.5F);
  REQUIRE(sequence.Bounds() == command.destination);
}

TEST_CASE("PaintContextValidatesExternalTextureSourceAndOpacity") {
  PaintSequence sequence;
  PaintContext context{sequence, Rect{0.0F, 0.0F, 100.0F, 80.0F}};
  REQUIRE_THROWS_AS(
      context.DrawImageRect(TestExternalTexture(), {19.0F, 0.0F, 2.0F, 2.0F}, {}),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      context.DrawImage(TestExternalTexture(), {0.0F, 0.0F, 10.0F, 10.0F}, ImageSampling::Linear, 1.1F),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(context.DrawImage(ExternalTexture{}, {}), std::invalid_argument);
}

TEST_CASE("PaintContextOmitsTextRunsWithoutVisibleBounds") {
  PaintSequence sequence;
  PaintContext context{sequence, Rect{0.0F, 0.0F, 100.0F, 80.0F}};
  const TextStyle style{Font::Monospace(14.0F), Color::White()};
  context.DrawTextRun({}, {10.0F, 24.0F}, "hidden", style);
  context.DrawTextRuns({
      TextRun{{10.0F, 12.0F, 20.0F, 16.0F}, {10.0F, 24.0F}, "visible", style, {}},
      TextRun{{30.0F, 12.0F, 0.0F, 16.0F}, {30.0F, 24.0F}, "hidden", style, {}},
  });
  context.Finish();

  REQUIRE(sequence.Commands().size() == 1);
  const auto& command = std::get<DrawTextRunsCommand>(sequence.Commands().front());
  REQUIRE(command.runs.size() == 1);
  REQUIRE(command.runs.front().text == "visible");
  REQUIRE(sequence.Bounds() == Rect{10.0F, 12.0F, 20.0F, 16.0F});
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

TEST_CASE("PaintContextRecordsShadowOverflowBounds") {
  PaintSequence sequence;
  PaintContext context{sequence, Rect{0.0F, 0.0F, 100.0F, 80.0F}};
  context.DrawShadow({10.0F, 20.0F, 30.0F, 15.0F}, Color::Rgb(0, 0, 0, 0.25F), {4.0F, 6.0F}, 8.0F, 2.0F, 5.0F);
  context.Finish();

  REQUIRE(sequence.Commands().size() == 1);
  const auto* shadow = std::get_if<DrawShadowCommand>(&sequence.Commands().front());
  REQUIRE(shadow != nullptr);
  REQUIRE(shadow->offset == Point{4.0F, 6.0F});
  REQUIRE(sequence.Bounds() == Rect{4.0F, 16.0F, 50.0F, 35.0F});
}

TEST_CASE("PaintContextAllowsNegativeShadowSpread") {
  PaintSequence sequence;
  PaintContext context{sequence, Rect{0.0F, 0.0F, 100.0F, 80.0F}};
  context.DrawShadow({10.0F, 20.0F, 30.0F, 15.0F}, Color::Black(), {4.0F, 6.0F}, 8.0F, -2.0F, 5.0F);
  context.Finish();

  REQUIRE(sequence.Bounds() == Rect{8.0F, 20.0F, 42.0F, 27.0F});
}

TEST_CASE("ShadowResolutionClampsCornerRadiusAndRejectsCollapsedCasters") {
  const detail::ResolvedShadow expanded = detail::ResolveShadow(
      DrawShadowCommand{
          .rect = {10.0F, 20.0F, 20.0F, 10.0F},
          .color = Color::Black(),
          .blur_radius = 6.0F,
          .spread = 3.0F,
          .corner_radius = 4.0F,
      }
  );
  REQUIRE(expanded.caster == Rect{7.0F, 17.0F, 26.0F, 16.0F});
  REQUIRE(expanded.corner_radius == 7.0F);
  REQUIRE(expanded.standard_deviation == 2.0F);

  const detail::ResolvedShadow collapsed = detail::ResolveShadow(
      DrawShadowCommand{
          .rect = {0.0F, 0.0F, 10.0F, 10.0F},
          .color = Color::Black(),
          .spread = -5.0F,
          .corner_radius = 8.0F,
      }
  );
  REQUIRE(collapsed.IsEmpty());
  REQUIRE(collapsed.bounds.IsEmpty());
}

TEST_CASE("PaintContextRejectsInvalidDrawingParameters") {
  const float nan = std::numeric_limits<float>::quiet_NaN();

  PaintSequence sequence;
  REQUIRE_THROWS_AS(PaintContext(sequence, Rect{0.0F, 0.0F, -1.0F, 20.0F}), std::invalid_argument);

  PaintContext context{sequence, Rect{0.0F, 0.0F, 100.0F, 80.0F}};
  REQUIRE_THROWS_AS(context.DrawRect({0.0F, 0.0F, nan, 10.0F}, Color::White()), std::invalid_argument);
  REQUIRE_THROWS_AS(Font::System(0.0F), std::invalid_argument);
  REQUIRE_THROWS_AS(
      context.DrawTextRun({0.0F, 0.0F, 10.0F, 10.0F}, {0.0F, 8.0F}, "first\nsecond", TextStyle{}),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(context.DrawCircle({}, -1.0F, Color::White()), std::invalid_argument);
  REQUIRE_THROWS_AS(context.DrawArc({}, 10.0F, 0.0F, nan, Color::White(), 1.0F), std::invalid_argument);
  REQUIRE_THROWS_AS(context.DrawBorder({}, Color::White(), -1.0F), std::invalid_argument);
  REQUIRE_THROWS_AS(context.DrawShadow({}, Color::Black(), {}, -1.0F), std::invalid_argument);
  REQUIRE_THROWS_AS(context.DrawShadow({}, Color::Black(), {}, nan), std::invalid_argument);
  REQUIRE_THROWS_AS(context.DrawShadow({}, Color::Black(), {nan, 0.0F}, 1.0F), std::invalid_argument);
  REQUIRE_THROWS_AS(context.DrawShadow({}, Color::Black(), {}, 1.0F, nan), std::invalid_argument);
  REQUIRE_THROWS_AS(context.PushClip({}, -1.0F), std::invalid_argument);
  REQUIRE_THROWS_AS(context.PushTransform(Transform2D{1.0F, 0.0F, 0.0F, 1.0F, nan, 0.0F}), std::invalid_argument);
  context.Finish();
}

TEST_CASE("PaintContextRecordsPathCommandsAndBounds") {
  Path path;
  path.MoveTo({10.0F, 20.0F}).LineTo({40.0F, 20.0F}).LineTo({25.0F, 50.0F}).Close();

  PaintSequence sequence;
  PaintContext context{sequence, Rect{0.0F, 0.0F, 100.0F, 80.0F}};
  context.FillPath(path, Color::White(), PathFillRule::EvenOdd);
  context.StrokePath(path, Color::Black(), 2.0F, StrokeCap::Round, StrokeJoin::Round);
  context.DrawPathShadow(path, Color::Rgb(0, 0, 0, 0.25F), {4.0F, 6.0F}, 8.0F);
  context.PushPathClip(path);
  context.DrawRect({0.0F, 0.0F, 100.0F, 80.0F}, Color::White());
  context.PopClip();
  context.Finish();

  REQUIRE(sequence.Commands().size() == 6);
  REQUIRE(std::holds_alternative<FillPathCommand>(sequence.Commands()[0]));
  REQUIRE(std::holds_alternative<StrokePathCommand>(sequence.Commands()[1]));
  REQUIRE(std::holds_alternative<DrawPathShadowCommand>(sequence.Commands()[2]));
  REQUIRE(std::holds_alternative<PushPathClipCommand>(sequence.Commands()[3]));
  REQUIRE(std::holds_alternative<PopClipCommand>(sequence.Commands()[5]));
  REQUIRE(sequence.Bounds() == Rect{6.0F, 18.0F, 46.0F, 46.0F});
}

TEST_CASE("PaintContextUsesPathCommandsForAsymmetricCornerRadii") {
  PaintSequence sequence;
  PaintContext context{sequence, Rect{0.0F, 0.0F, 100.0F, 80.0F}};
  const CornerRadii corners = CornerRadii::Top(16.0F);
  context.DrawShadow({10.0F, 10.0F, 60.0F, 40.0F}, Color::Black(), {}, 4.0F, 0.0F, corners);
  context.DrawRect({10.0F, 10.0F, 60.0F, 40.0F}, Color::White(), corners);
  context.DrawBorder({10.0F, 10.0F, 60.0F, 40.0F}, Color::Black(), 1.0F, corners);
  context.PushClip({10.0F, 10.0F, 60.0F, 40.0F}, corners);
  context.PopClip();
  context.Finish();

  REQUIRE(std::holds_alternative<DrawPathShadowCommand>(sequence.Commands()[0]));
  REQUIRE(std::holds_alternative<FillPathCommand>(sequence.Commands()[1]));
  REQUIRE(std::holds_alternative<StrokePathCommand>(sequence.Commands()[2]));
  const auto* border = std::get_if<StrokePathCommand>(&sequence.Commands()[2]);
  REQUIRE(border != nullptr);
  REQUIRE(border->path.Bounds() == Rect{10.5F, 10.5F, 59.0F, 39.0F});
  REQUIRE(std::holds_alternative<PushPathClipCommand>(sequence.Commands()[3]));
  REQUIRE(std::holds_alternative<PopClipCommand>(sequence.Commands()[4]));
}

} // namespace huxerui::test
