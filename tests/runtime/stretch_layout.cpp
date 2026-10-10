#include "runtime_test_support.h"

#include <array>
#include <cstddef>

namespace huxerui::test {
namespace {

struct ProbeId {
  using Value = std::size_t;
};

std::array<std::size_t, 3> measured{};
std::array<Size, 3> measured_sizes{};
bool horizontal = false;
bool loose_cross = false;

class StretchMeasureProbe final : public Layout<StretchMeasureProbe> {
public:
  using Layout::Layout;

  static LayoutResult Measure(LayoutContext&, ViewNode& node, Constraints constraints) {
    const auto id = node.LayoutValueOr<ProbeId>(0);
    ++measured[id];
    const auto size = constraints.Constrain({110.0F, 40.0F});
    measured_sizes[id] = size;
    return LayoutResult{}.SetSize(size);
  }
};

View DeepStretchLayout() {
  View child = StretchMeasureProbe {}.With(Grow(1.0F));
  for (int depth = 0; depth < 12; ++depth) {
    child = horizontal
        ? View{Row {child}.With(Grow(1.0F), CrossAlign(CrossAxisAlignment::Stretch))}
        : View{Column {child}.With(Grow(1.0F), CrossAlign(CrossAxisAlignment::Stretch))};
  }
  const Frame frame = loose_cross
      ? (horizontal ? Frame{.width = 400.0F} : Frame{.height = 600.0F})
      : Frame{.width = 400.0F, .height = 600.0F};
  View content = View{child}.With(frame);
  return loose_cross
      ? View{Stack {content}.With(Align(HorizontalAlignment::Start, VerticalAlignment::Start))}
      : content;
}

View WeightedStretchLayout() {
  View fixed = StretchMeasureProbe {}.LayoutValue<ProbeId>(0).With(
      horizontal ? Frame{.width = 32.0F} : Frame{.height = 32.0F});
  View first = StretchMeasureProbe {}.LayoutValue<ProbeId>(1).With(Grow(1.0F));
  View second = StretchMeasureProbe {}.LayoutValue<ProbeId>(2).With(Grow(3.0F));
  View content = horizontal ? View{Row {fixed, first, second}} : View{Column {fixed, first, second}};
  return Stack {
    View{content}.With(
        Frame{.width = horizontal ? 240.0F : 300.0F, .height = horizontal ? 300.0F : 240.0F},
        Padding(16.0F), Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch)),
  }.With(Align(HorizontalAlignment::Start, VerticalAlignment::Start));
}

View UnboundedStretchLayout() {
  const View child = StretchMeasureProbe {}.With(Grow(1.0F));
  View content = horizontal
      ? View{Row {child}.With(CrossAlign(CrossAxisAlignment::Stretch))}
      : View{Column {child}.With(CrossAlign(CrossAxisAlignment::Stretch))};
  return ScrollView(content).ScrollAxis(horizontal ? Axis::Horizontal : Axis::Vertical)
      .With(Frame{.width = 240.0F, .height = 400.0F});
}

View DeepStretchVirtualList() {
  View child = VirtualList(100000, [](std::size_t index) {
    return StretchMeasureProbe {}.Key(index);
  }).ItemExtent(42.0F).With(Grow(1.0F));
  for (int depth = 0; depth < 12; ++depth) {
    child = Column {child}.With(Grow(1.0F), CrossAlign(CrossAxisAlignment::Stretch));
  }
  return View{child}.With(Frame{.width = 400.0F, .height = 600.0F});
}

} // namespace

TEST_CASE("BoundedStretchGrowMeasuresDeepSubtreesOnce") {
  for (const bool row : {false, true}) {
    CAPTURE(row);
    horizontal = row;
    loose_cross = false;
    measured.fill(0);
    TestPlatform platform;
    UiWindow runtime{DeepStretchLayout, platform};
    runtime.SetWindowMetrics({.viewport = {400.0F, 600.0F}});
    runtime.BuildFrame();
    REQUIRE(measured[0] == 1);
    REQUIRE(measured_sizes[0] == Size{400.0F, 600.0F});

    runtime.BuildFrame();
    REQUIRE(measured[0] == 1);
    runtime.SetWindowMetrics({.viewport = {300.0F, 500.0F}});
    runtime.BuildFrame();
    REQUIRE(measured[0] == 2);
    REQUIRE(measured_sizes[0] == Size{300.0F, 500.0F});
  }
}

TEST_CASE("BoundedStretchGrowKeepsWeightedAllocationAndFixedSiblings") {
  for (const bool row : {false, true}) {
    CAPTURE(row);
    horizontal = row;
    measured.fill(0);
    TestPlatform platform;
    UiWindow runtime{WeightedStretchLayout, platform};
    runtime.SetWindowMetrics({.viewport = {400.0F, 600.0F}});
    runtime.BuildFrame();
    REQUIRE(measured == std::array<std::size_t, 3>{1, 1, 1});
    CAPTURE(measured_sizes[0].width, measured_sizes[0].height);
    CAPTURE(measured_sizes[1].width, measured_sizes[1].height);
    CAPTURE(measured_sizes[2].width, measured_sizes[2].height);
    REQUIRE(measured_sizes[0] == (row ? Size{32.0F, 268.0F} : Size{268.0F, 32.0F}));
    REQUIRE(measured_sizes[1] == (row ? Size{40.0F, 268.0F} : Size{268.0F, 40.0F}));
    REQUIRE(measured_sizes[2] == (row ? Size{120.0F, 268.0F} : Size{268.0F, 120.0F}));
  }
}

TEST_CASE("StretchGrowRetainsIntrinsicCrossSizeWhenCrossAxisIsLoose") {
  for (const bool row : {false, true}) {
    CAPTURE(row);
    horizontal = row;
    loose_cross = true;
    measured.fill(0);
    TestPlatform platform;
    UiWindow runtime{DeepStretchLayout, platform};
    runtime.SetWindowMetrics({.viewport = {400.0F, 600.0F}});
    runtime.BuildFrame();
    CAPTURE(measured_sizes[0].width, measured_sizes[0].height);
    REQUIRE(measured_sizes[0] == (row ? Size{400.0F, 40.0F} : Size{110.0F, 600.0F}));
  }
}

TEST_CASE("StretchGrowRetainsNaturalMainSizeUnderUnboundedConstraints") {
  for (const bool row : {false, true}) {
    CAPTURE(row);
    horizontal = row;
    measured.fill(0);
    TestPlatform platform;
    UiWindow runtime{UnboundedStretchLayout, platform};
    runtime.SetWindowMetrics({.viewport = {400.0F, 600.0F}});
    runtime.BuildFrame();
    REQUIRE(measured[0] > 0);
    REQUIRE((row ? measured_sizes[0].width : measured_sizes[0].height) == (row ? 110.0F : 40.0F));
  }
}

TEST_CASE("DeepStretchGrowKeepsLargeVirtualListsBoundedDuringScroll") {
  measured.fill(0);
  TestPlatform platform;
  UiWindow runtime{DeepStretchVirtualList, platform};
  runtime.SetWindowMetrics({.viewport = {400.0F, 600.0F}});
  runtime.BuildFrame();
  REQUIRE(measured[0] < 100);
  runtime.HandleScrollInput(ScrollInputEvent{{200.0F, 300.0F}, 0.0F, 1000.0F});
  runtime.BuildFrame();
  REQUIRE(measured[0] < 200);
}

} // namespace huxerui::test
