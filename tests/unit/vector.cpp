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

TEST_CASE("VectorAssetsValidateGeometryAndBuilderBalance") {
  REQUIRE_THROWS_AS(VectorAsset::Create({}, [](VectorBuilder&) {}), std::invalid_argument);
  REQUIRE_THROWS_AS(
      VectorAsset::Create({10.0F, 10.0F}, [](VectorBuilder& builder) { builder.PopTransform(); }),
      std::logic_error
  );
}
