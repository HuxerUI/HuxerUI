#include "runtime_test_support.h"

#include <algorithm>
#include <limits>
#include <ranges>
#include <string_view>
#include <variant>

namespace huxerui::test {
namespace {

constexpr Color custom_bar_color = Color::Rgb(220, 38, 38);
constexpr Color first_donut_color = Color::Rgb(37, 99, 235);
constexpr Color second_donut_color = Color::Rgb(5, 150, 105);

View BarChartApp() {
  return BarChart(
             {
                 ChartDataPoint{"Jan", 24.0F, custom_bar_color},
                 ChartDataPoint{"Feb", 42.0F},
                 ChartDataPoint{"Mar", 31.0F},
             },
             BarChartOptions{.maximum_value = 50.0F, .accessibility_label = "Quarterly revenue"}
  )
      .With(Frame{320.0F, 240.0F});
}

View DonutChartApp() {
  return DonutChart(
             {
                 ChartDataPoint{"Desktop", 60.0F, first_donut_color},
                 ChartDataPoint{"Mobile", 30.0F, second_donut_color},
                 ChartDataPoint{"Tablet", 10.0F},
             },
             DonutChartOptions{.accessibility_label = "Traffic sources"}
  )
      .With(Frame{420.0F, 260.0F});
}

void MoveMouse(Runtime& runtime, Point position) {
  runtime.HandlePointerEvent({PointerEventType::Move, 1, position, PointerDeviceKind::Mouse});
}

bool HasForegroundText(const detail::MountedNode& node, std::string_view expected) {
  return std::ranges::any_of(node.render_node.foreground.Commands(), [expected](const PaintCommand& command) {
    const auto* text = std::get_if<DrawTextCommand>(&command);
    return text != nullptr && text->text == expected;
  });
}

} // namespace

TEST_CASE("BarChartRecordsBarsLabelsGridAndAccessibleSummary") {
  TestPlatform platform;
  Runtime runtime{BarChartApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});

  const FrameCommit& commit = runtime.BuildCommit();
  const detail::MountedNode* chart = runtime.RootNode();
  REQUIRE(chart != nullptr);
  REQUIRE(chart->kind == detail::NodeKind::Canvas);

  const std::vector<PaintCommand>& commands = chart->render_node.content.Commands();
  REQUIRE(std::ranges::any_of(commands, [](const PaintCommand& command) {
    const auto* rect = std::get_if<DrawRectCommand>(&command);
    return rect != nullptr && rect->color == custom_bar_color && rect->rect.height > 0.0F;
  }));
  REQUIRE(std::ranges::any_of(commands, [](const PaintCommand& command) {
    const auto* text = std::get_if<DrawTextCommand>(&command);
    return text != nullptr && text->text == "Mar";
  }));

  REQUIRE(commit.semantic_frame != nullptr);
  const auto semantic = std::ranges::find(commit.semantic_frame->nodes, "Quarterly revenue", &SemanticNode::label);
  REQUIRE(semantic != commit.semantic_frame->nodes.end());
  REQUIRE(semantic->role == SemanticRole::Image);
  REQUIRE(semantic->value == "Jan: 24, Feb: 42, Mar: 31");
}

TEST_CASE("DonutChartRecordsTrackSegmentsCenterAndLegend") {
  TestPlatform platform;
  Runtime runtime{DonutChartApp, platform};
  runtime.SetWindowMetrics({.viewport = {420.0F, 260.0F}});
  runtime.BuildRenderFrame();

  const detail::MountedNode* chart = runtime.RootNode();
  REQUIRE(chart != nullptr);
  const std::vector<PaintCommand>& commands = chart->render_node.content.Commands();
  const std::size_t arc_count = static_cast<std::size_t>(std::ranges::count_if(commands, [](const PaintCommand& command) {
    return std::holds_alternative<DrawArcCommand>(command);
  }));
  REQUIRE(arc_count == 4);
  REQUIRE(std::ranges::any_of(commands, [](const PaintCommand& command) {
    const auto* arc = std::get_if<DrawArcCommand>(&command);
    return arc != nullptr && arc->color == first_donut_color;
  }));
  REQUIRE(std::ranges::any_of(commands, [](const PaintCommand& command) {
    const auto* text = std::get_if<DrawTextCommand>(&command);
    return text != nullptr && text->text == "Total";
  }));
  REQUIRE(std::ranges::any_of(commands, [](const PaintCommand& command) {
    const auto* text = std::get_if<DrawTextCommand>(&command);
    return text != nullptr && text->text == "Mobile";
  }));
}

TEST_CASE("ChartsUpdateHoverHighlightAndInformationWithoutRecomposition") {
  TestPlatform platform;
  Runtime bar_runtime{BarChartApp, platform};
  bar_runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
  bar_runtime.BuildRenderFrame();

  MoveMouse(bar_runtime, {170.0F, 120.0F});
  bar_runtime.BuildRenderFrame();
  const detail::MountedNode* bar_chart = bar_runtime.RootNode();
  REQUIRE(bar_chart != nullptr);
  REQUIRE(HasForegroundText(*bar_chart, "Feb"));
  REQUIRE(HasForegroundText(*bar_chart, "42  ·  43.3%"));

  MoveMouse(bar_runtime, {400.0F, 300.0F});
  bar_runtime.BuildRenderFrame();
  REQUIRE(bar_chart->render_node.foreground.Commands().empty());

  Runtime donut_runtime{DonutChartApp, platform};
  donut_runtime.SetWindowMetrics({.viewport = {420.0F, 260.0F}});
  donut_runtime.BuildRenderFrame();

  MoveMouse(donut_runtime, {202.0F, 130.0F});
  donut_runtime.BuildRenderFrame();
  const detail::MountedNode* donut_chart = donut_runtime.RootNode();
  REQUIRE(donut_chart != nullptr);
  REQUIRE(HasForegroundText(*donut_chart, "Desktop"));
  REQUIRE(HasForegroundText(*donut_chart, "60  ·  60%"));

  MoveMouse(donut_runtime, {300.0F, 130.0F});
  donut_runtime.BuildRenderFrame();
  REQUIRE(HasForegroundText(*donut_chart, "Mobile"));
  REQUIRE(HasForegroundText(*donut_chart, "30  ·  30%"));
}

TEST_CASE("ChartsRejectInvalidDataAndOptionsBeforeComposition") {
  REQUIRE_THROWS_AS(BarChart({}), std::invalid_argument);
  REQUIRE_THROWS_AS(BarChart({ChartDataPoint{"Invalid", -1.0F}}), std::invalid_argument);
  REQUIRE_THROWS_AS(
      BarChart({ChartDataPoint{"Invalid", std::numeric_limits<float>::infinity()}}),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(
      BarChart({ChartDataPoint{"Value", 5.0F}}, BarChartOptions{.maximum_value = 4.0F}),
      std::invalid_argument
  );
  REQUIRE_THROWS_AS(DonutChart({ChartDataPoint{"Empty", 0.0F}}), std::invalid_argument);
  REQUIRE_THROWS_AS(
      DonutChart({ChartDataPoint{"Value", 1.0F}}, DonutChartOptions{.inner_radius_fraction = 0.9F}),
      std::invalid_argument
  );
}

} // namespace huxerui::test
