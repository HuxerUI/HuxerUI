#include <catch2/catch_amalgamated.hpp>

#include <huxerui/paint.h>
#include <huxerui/vector.h>

using namespace huxerui;

namespace {

Path Triangle() {
  return Path{}.MoveTo({0.0F, 0.0F}).LineTo({20.0F, 0.0F}).LineTo({10.0F, 10.0F}).Close();
}

} // namespace

TEST_CASE("VectorAssetsRetainImmutableDrawingData") {
  const VectorAsset vector =
      VectorAsset::Create({20.0F, 10.0F}, [](VectorBuilder& builder) { builder.FillPath(Triangle(), Color::Black()); });

  REQUIRE(vector.HasValue());
  REQUIRE(vector.ViewBox() == Rect{0.0F, 0.0F, 20.0F, 10.0F});
  REQUIRE(vector.IntrinsicSize() == Size{20.0F, 10.0F});
  REQUIRE(vector == vector);
}

TEST_CASE("PaintContextExpandsVectorImagesIntoPlatformNeutralPathCommands") {
  const VectorAsset vector =
      VectorAsset::Create({20.0F, 10.0F}, [](VectorBuilder& builder) { builder.FillPath(Triangle(), Color::Black()); });
  PaintSequence sequence;
  PaintContext context(sequence, {0.0F, 0.0F, 100.0F, 100.0F});

  context.DrawImage(vector, {10.0F, 20.0F, 40.0F, 20.0F}, Color::Rgb(20, 40, 60, 0.5F), 0.5F);
  context.Finish();

  REQUIRE(sequence.Bounds() == Rect{10.0F, 20.0F, 40.0F, 20.0F});
  REQUIRE(sequence.Commands().size() == 5);
  REQUIRE(std::holds_alternative<PushClipCommand>(sequence.Commands()[0]));
  REQUIRE(std::holds_alternative<PushTransformCommand>(sequence.Commands()[1]));
  const auto& fill = std::get<FillPathCommand>(sequence.Commands()[2]);
  REQUIRE(fill.color == Color::Rgb(20, 40, 60, 0.25F));
  REQUIRE(std::holds_alternative<PopTransformCommand>(sequence.Commands()[3]));
  REQUIRE(std::holds_alternative<PopClipCommand>(sequence.Commands()[4]));
}

TEST_CASE("VectorAssetsRetainNormalizedStrokeStyles") {
  const VectorAsset vector = VectorAsset::Create({20.0F, 10.0F}, [](VectorBuilder& builder) {
    builder.StrokePath(Triangle(), Color::Black(),
                       StrokeStyle{
                           .width = 2.0F,
                           .cap = StrokeCap::Round,
                           .dash_pattern = {3.0F, 1.0F, 2.0F},
                           .dash_offset = -2.0F,
                       });
  });
  PaintSequence sequence;
  PaintContext context(sequence, {0.0F, 0.0F, 20.0F, 10.0F});
  context.DrawImage(vector, {0.0F, 0.0F, 20.0F, 10.0F});
  context.Finish();

  const auto& command = std::get<StrokePathCommand>(sequence.Commands()[2]);
  REQUIRE(command.style.width == 2.0F);
  REQUIRE(command.style.cap == StrokeCap::Round);
  REQUIRE(command.style.dash_pattern == std::vector<float>{3.0F, 1.0F, 2.0F, 3.0F, 1.0F, 2.0F});
  REQUIRE(command.style.dash_offset == 10.0F);
}

TEST_CASE("VectorAssetsPreserveGradientPathFillsAndResolveTint") {
  const LinearGradient gradient{
      .stops = {{0.0F, Color::Rgb(255, 0, 0, 0.5F)}, {1.0F, Color::Rgb(0, 0, 255)}},
      .transform = {0.75F, 0.2F, -0.1F, 1.0F, 0.25F, -0.1F},
  };
  const VectorAsset vector = VectorAsset::Create({20.0F, 10.0F}, [&](VectorBuilder& builder) {
    builder.FillPath(Triangle(), gradient, {0.0F, 0.0F, 40.0F, 20.0F});
  });
  PaintSequence sequence;
  PaintContext context(sequence, {0.0F, 0.0F, 40.0F, 20.0F});
  context.DrawImage(vector, {0.0F, 0.0F, 40.0F, 20.0F}, Color::Rgb(20, 40, 60, 0.5F), 0.5F);
  context.Finish();

  const auto& command = std::get<FillLinearGradientPathCommand>(sequence.Commands()[2]);
  REQUIRE(command.gradient_rect == Rect{0.0F, 0.0F, 40.0F, 20.0F});
  REQUIRE(command.gradient.transform == gradient.transform);
  REQUIRE(command.gradient.stops[0].color == Color::Rgb(20, 40, 60, 0.125F));
  REQUIRE(command.gradient.stops[1].color == Color::Rgb(20, 40, 60, 0.25F));
}

TEST_CASE("VectorAssetsPreserveGradientPathStrokesAndResolveTint") {
  const RadialGradient gradient{
      .center = {0.5F, 0.5F},
      .radius = {0.5F, 0.25F},
      .stops = {{0.0F, Color::Rgb(255, 0, 0, 0.5F)}, {1.0F, Color::Rgb(0, 0, 255)}},
      .transform = {0.75F, 0.2F, -0.1F, 1.0F, 0.25F, -0.1F},
  };
  const StrokeStyle style{.width = 2.0F, .cap = StrokeCap::Round, .dash_pattern = {3.0F, 1.0F}};
  const VectorAsset vector = VectorAsset::Create({20.0F, 10.0F}, [&](VectorBuilder& builder) {
    builder.StrokePath(Triangle(), gradient, {0.0F, 0.0F, 40.0F, 20.0F}, style);
  });
  PaintSequence sequence;
  PaintContext context(sequence, {0.0F, 0.0F, 40.0F, 20.0F});
  context.DrawImage(vector, {0.0F, 0.0F, 40.0F, 20.0F}, Color::Rgb(20, 40, 60, 0.5F), 0.5F);
  context.Finish();

  const auto& command = std::get<StrokeRadialGradientPathCommand>(sequence.Commands()[2]);
  REQUIRE(command.gradient_rect == Rect{0.0F, 0.0F, 40.0F, 20.0F});
  REQUIRE(command.gradient.transform == gradient.transform);
  REQUIRE(command.gradient.stops[0].color == Color::Rgb(20, 40, 60, 0.125F));
  REQUIRE(command.gradient.stops[1].color == Color::Rgb(20, 40, 60, 0.25F));
  REQUIRE(command.style == style);
}

TEST_CASE("VectorAssetsValidateGeometryAndBuilderBalance") {
  REQUIRE_THROWS_AS(VectorAsset::Create({}, [](VectorBuilder&) {}), std::invalid_argument);
  REQUIRE_THROWS_AS(
      VectorAsset::Create({10.0F, 10.0F}, [](VectorBuilder& builder) { builder.PopTransform(); }),
      std::logic_error
  );
}
