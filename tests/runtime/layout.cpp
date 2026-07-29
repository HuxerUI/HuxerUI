#include "runtime_test_support.h"

#include <limits>

namespace huxerui::test {

State<bool> use_column_layout;

struct FlowBreakBefore {
  using Value = bool;
};

class TestFlow final : public Layout<TestFlow> {
public:
  using Layout::Layout;

  TestFlow Gap(float value) && {
    return std::move(*this).With(huxerui::Spacing{value});
  }

  static LayoutResult Measure(LayoutContext& context, MountedNode& node, huxerui::Constraints constraints) {
    LayoutResult result;
    float x = 0.0F;
    float y = 0.0F;
    float line_height = 0.0F;
    float measured_width = 0.0F;

    for (MountedNode& child : node.Children()) {
      const Size child_size = context.Measure(child, constraints.Loose());
      const bool break_before = child.LayoutValueOr<FlowBreakBefore>(false);
      if (x > 0.0F && (break_before || x + child_size.width > constraints.max_width)) {
        measured_width = std::max(measured_width, x - node.Spacing());
        x = 0.0F;
        y += line_height + node.Spacing();
        line_height = 0.0F;
      }

      result.Place(child, {x, y});
      x += child_size.width + node.Spacing();
      line_height = std::max(line_height, child_size.height);
    }

    if (x > 0.0F) {
      measured_width = std::max(measured_width, x - node.Spacing());
    }
    result.SetSize(constraints.Constrain({
        measured_width,
        y + line_height,
    }));
    return result;
  }
};

class UnboundedWidth final : public Layout<UnboundedWidth> {
public:
  using Layout::Layout;

  static LayoutResult Measure(LayoutContext& context, MountedNode& node, Constraints constraints) {
    LayoutResult result;
    if (node.ChildCount() == 0) {
      return result.SetSize(constraints.Constrain({}));
    }
    MountedNode& child = node.ChildAt(0);
    const Size size = context.Measure(
        child,
        {
            0.0F,
            std::numeric_limits<float>::infinity(),
            0.0F,
            constraints.max_height,
        }
    );
    result.Place(child, {});
    return result.SetSize(constraints.Constrain(size));
  }
};

View CustomLayoutApp() {
  return TestFlow {
    Text("A").With(huxerui::Frame{40.0F, 10.0F}),
    Text("B").With(huxerui::Frame{40.0F, 10.0F}).LayoutValue<FlowBreakBefore>(true),
    Text("C").With(huxerui::Frame{40.0F, 10.0F}),
  }.With(huxerui::Padding{5.0F})
      .Gap(5.0F);
}

View LayoutIdentityApp() {
  auto use_column = UseState(false);
  use_column_layout = use_column;
  if (use_column.Get()) {
    return Column {
      Text("Content"),
    };
  }
  return Row {
    Text("Content"),
  };
}

View AxisAlignmentApp() {
  return Column {
    Text("A").With(huxerui::Frame{20.0F, 20.0F}),
    Text("B").With(huxerui::Frame{20.0F, 20.0F}),
  }.With(huxerui::MainAlign{MainAxisAlignment::SpaceBetween}, huxerui::CrossAlign{CrossAxisAlignment::Center});
}

View SpacerLayoutApp() {
  return Row {
    Text("L").With(huxerui::Frame{20.0F, 20.0F}),
    Spacer(),
    Text("R").With(huxerui::Frame{30.0F, 20.0F}),
  }.With(huxerui::CrossAlign{CrossAxisAlignment::Center});
}

View GrowLayoutApp() {
  return Row {
    Spacer().With(huxerui::Grow{1.0F}),
    Spacer().With(huxerui::Grow{2.0F}),
  };
}

View StackAlignmentApp() {
  return Stack {
    Text("A").With(huxerui::Frame{20.0F, 10.0F}),
  }.With(
      huxerui::Align{
          HorizontalAlignment::End,
          VerticalAlignment::Center,
      }
  );
}

View StretchLayoutApp() {
  return Column {
    Text("A").With(huxerui::Frame{20.0F, 20.0F}),
  }.With(huxerui::CrossAlign{CrossAxisAlignment::Stretch});
}

View WrappedTextApp() {
  return Column {
    Text("abcdefghij"),
  };
}

View AdaptiveFrameApp() {
  return Column {
    Text("Wide").With(Frame{.width = 80.0F}),
    Text("Tall").With(Frame{.height = 35.0F}),
    Text("A").With(Frame{.min_width = 60.0F, .min_height = 25.0F}),
    Text("Maximum").With(Frame{.max_width = 30.0F, .max_height = 15.0F}),
    Text("Preferred").With(Frame{.width = 90.0F, .max_width = 70.0F}),
    Text("Merged").With(Frame{.width = 55.0F}, Frame{.height = 30.0F}),
    Text("Padded").With(Frame{.width = 80.0F}, Padding{10.0F}),
  };
}

View ParentConstrainedFrameApp() {
  return Text("A").With(Frame{200.0F, 200.0F});
}

View BoundedContainerFrameApp() {
  return Column {
    ScrollView {
      Text("Scroll content").With(Frame{100.0F, 100.0F}),
    }.With(Frame{.max_height = 40.0F}),
    VirtualList(20, [](std::size_t index) { return Text::Format("Item {}", index); })
        .ItemExtent(20.0F)
        .With(Frame{.max_height = 60.0F}),
  };
}

View FlowWrapApp() {
  return Column {
    Flow {
      Text("A").With(Frame{40.0F, 10.0F}),
      Text("B").With(Frame{40.0F, 20.0F}),
      Text("C").With(Frame{40.0F, 15.0F}),
    }.With(Frame{.width = 90.0F}, Spacing{5.0F}, CrossAlign{CrossAxisAlignment::Center}),
  };
}

View FlowAlignmentApp() {
  return Column {
    Flow {
      Text("A").With(Frame{30.0F, 10.0F}),
      Text("B").With(Frame{30.0F, 10.0F}),
    }.With(Frame{.width = 100.0F}, Spacing{10.0F}, MainAlign{MainAxisAlignment::Center}),
  };
}

View FlowGrowApp() {
  return Column {
    Flow {
      Text("A").With(Frame{30.0F, 10.0F}, Grow{1.0F}),
      Text("B").With(Frame{30.0F, 20.0F}, Grow{2.0F}),
      Text("C").With(Frame{80.0F, 15.0F}),
    }.With(Frame{.width = 100.0F}, Spacing{10.0F}, CrossAlign{CrossAxisAlignment::Center}),
  };
}

View UnboundedFlowApp() {
  return UnboundedWidth {
    Flow {
      Text("A").With(Frame{30.0F, 10.0F}, Grow{}),
      Text("B").With(Frame{30.0F, 10.0F}, Grow{}),
    }.With(Spacing{5.0F}),
  };
}

View ForEachLayoutApp() {
  const std::vector<std::string> items{
      "First",
      "Second",
      "Third",
  };
  const std::vector<std::string> empty;
  return Column {
    Text("Header"),
    ForEach(items, [](const std::string& item) { return Text(item); }),
    ForEach(empty, [](const std::string& item) { return Text(item); }),
    Text("Footer"),
  }.With(huxerui::Spacing{5.0F});
}

View LayoutCounter() {
  HUXERUI_SCOPE({
    auto count = UseState(0);
    return Column {
      Text(count),
      Button("+1").OnClick([count] { count += 1; }),
    };
  });
}

View ForEachIdentityApp() {
  auto expanded = UseState(false);
  const std::vector<std::string> items = expanded.Get()
                                             ? std::vector<std::string>{
                                                   "new",
                                                   "second",
                                                   "first",
                                               }
                                             : std::vector<std::string>{
                                                   "first",
                                                   "second",
                                               };
  return Column {
    ForEach(items, [](const std::string& item) { return LayoutCounter().Key(item); }),
    Button("Toggle").OnClick([expanded] { expanded = !expanded; }),
  };
}

View ReactiveStateApiApp() {
  auto taps = UseState(2);
  auto items = UseState(std::vector<std::string>{
      "Alpha",
      "Bravo",
  });

  return Column {
    Text::Format("Taps {}", taps),
    ForEach(items, [](const std::string& item) { return Text(item); }),
    Button("Update").OnClick([taps, items] {
      taps += 1;
      items.Update([](auto& values) { values.push_back("Charlie"); });
    }),
  };
}

TEST_CASE("TestMainAndCrossAxisAlignment") {
  TestPlatform platform;
  Runtime runtime{AxisAlignmentApp, platform};
  runtime.SetViewport({100.0F, 100.0F});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->frame.width == 100.0F);
  REQUIRE(root->frame.height == 100.0F);
  REQUIRE(root->children[0]->frame.x == 40.0F);
  REQUIRE(root->children[0]->frame.y == 0.0F);
  REQUIRE(root->children[1]->frame.x == 40.0F);
  REQUIRE(root->children[1]->frame.y == 80.0F);
}

TEST_CASE("TestSpacerAndGrowLayout") {
  TestPlatform platform;
  Runtime runtime{SpacerLayoutApp, platform};
  runtime.SetViewport({200.0F, 60.0F});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children[0]->frame.x == 0.0F);
  REQUIRE(root->children[0]->frame.y == 20.0F);
  REQUIRE(root->children[1]->frame.x == 20.0F);
  REQUIRE(root->children[1]->frame.width == 150.0F);
  REQUIRE(root->children[2]->frame.x == 170.0F);
  REQUIRE(root->children[2]->frame.y == 20.0F);

  Runtime grow_runtime{GrowLayoutApp, platform};
  grow_runtime.SetViewport({300.0F, 40.0F});
  grow_runtime.BuildFrame();

  root = grow_runtime.RootNode();
  REQUIRE(root->children[0]->frame.width == 100.0F);
  REQUIRE(root->children[1]->frame.x == 100.0F);
  REQUIRE(root->children[1]->frame.width == 200.0F);
}

TEST_CASE("TestStackAndStretchAlignment") {
  TestPlatform platform;
  Runtime stack_runtime{StackAlignmentApp, platform};
  stack_runtime.SetViewport({100.0F, 80.0F});
  stack_runtime.BuildFrame();

  const auto* root = stack_runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children[0]->frame.x == 80.0F);
  REQUIRE(root->children[0]->frame.y == 35.0F);

  Runtime stretch_runtime{StretchLayoutApp, platform};
  stretch_runtime.SetViewport({120.0F, 80.0F});
  stretch_runtime.BuildFrame();

  root = stretch_runtime.RootNode();
  REQUIRE(root->children[0]->frame.width == 120.0F);
}

TEST_CASE("TestWrappedTextMeasurement") {
  TestPlatform platform;
  Runtime runtime{WrappedTextApp, platform};
  runtime.SetViewport({40.0F, 100.0F});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children[0]->frame.width == 40.0F);
  REQUIRE(root->children[0]->frame.height == 60.0F);
}

TEST_CASE("TestAdaptiveFrameConstraints") {
  TestPlatform platform;
  Runtime runtime{AdaptiveFrameApp, platform};
  runtime.SetViewport({120.0F, 400.0F});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children.size() == 7);
  REQUIRE(root->children[0]->measured_size.width == 80.0F);
  REQUIRE(root->children[0]->measured_size.height == 20.0F);
  REQUIRE(root->children[1]->measured_size.width == 40.0F);
  REQUIRE(root->children[1]->measured_size.height == 35.0F);
  REQUIRE(root->children[2]->measured_size.width == 60.0F);
  REQUIRE(root->children[2]->measured_size.height == 25.0F);
  REQUIRE(root->children[3]->measured_size.width == 30.0F);
  REQUIRE(root->children[3]->measured_size.height == 15.0F);
  REQUIRE(root->children[4]->measured_size.width == 70.0F);
  REQUIRE(root->children[4]->measured_size.height == 40.0F);
  REQUIRE(root->children[5]->measured_size.width == 55.0F);
  REQUIRE(root->children[5]->measured_size.height == 30.0F);
  REQUIRE(root->children[6]->measured_size.width == 80.0F);
  REQUIRE(root->children[6]->measured_size.height == 40.0F);
}

TEST_CASE("TestFrameConstraintsRespectParentAndBoundContainers") {
  TestPlatform platform;
  Runtime constrained{ParentConstrainedFrameApp, platform};
  constrained.SetViewport({80.0F, 60.0F});
  constrained.BuildFrame();

  const auto* root = constrained.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->measured_size.width == 80.0F);
  REQUIRE(root->measured_size.height == 60.0F);

  Runtime containers{BoundedContainerFrameApp, platform};
  containers.SetViewport({120.0F, 200.0F});
  containers.BuildFrame();

  root = containers.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children[0]->measured_size.height == 40.0F);
  REQUIRE(root->children[1]->measured_size.height == 60.0F);
}

TEST_CASE("TestFrameConstraintValidation") {
  REQUIRE_THROWS_AS(Text("A").With(Frame{.width = -1.0F}), std::invalid_argument);
  REQUIRE_THROWS_AS(Text("A").With(Frame{.min_width = 50.0F, .max_width = 40.0F}), std::invalid_argument);
  REQUIRE_THROWS_AS(Text("A").With(Frame{.min_height = 50.0F}, Frame{.max_height = 40.0F}), std::invalid_argument);
}

TEST_CASE("TestFlowWrapsAndAlignsChildrenWithinLines") {
  TestPlatform platform;
  Runtime runtime{FlowWrapApp, platform};
  runtime.SetViewport({120.0F, 100.0F});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  const auto& flow = *root->children[0];
  REQUIRE(flow.layout->type == std::type_index(typeid(Flow)));
  REQUIRE(flow.measured_size.width == 90.0F);
  REQUIRE(flow.measured_size.height == 40.0F);
  REQUIRE(flow.children[0]->frame.x == 0.0F);
  REQUIRE(flow.children[0]->frame.y == 5.0F);
  REQUIRE(flow.children[1]->frame.x == 45.0F);
  REQUIRE(flow.children[1]->frame.y == 0.0F);
  REQUIRE(flow.children[2]->frame.x == 0.0F);
  REQUIRE(flow.children[2]->frame.y == 25.0F);
}

TEST_CASE("TestFlowAppliesMainAlignmentPerLine") {
  TestPlatform platform;
  Runtime runtime{FlowAlignmentApp, platform};
  runtime.SetViewport({120.0F, 100.0F});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  const auto& flow = *root->children[0];
  REQUIRE(flow.children[0]->frame.x == 15.0F);
  REQUIRE(flow.children[1]->frame.x == 55.0F);
}

TEST_CASE("TestFlowDistributesGrowWithinEachLine") {
  TestPlatform platform;
  Runtime runtime{FlowGrowApp, platform};
  runtime.SetViewport({120.0F, 100.0F});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  const auto& flow = *root->children[0];
  REQUIRE(flow.children[0]->measured_size.width == 30.0F);
  REQUIRE(flow.children[0]->frame.y == 5.0F);
  REQUIRE(flow.children[1]->measured_size.width == 60.0F);
  REQUIRE(flow.children[1]->frame.x == 40.0F);
  REQUIRE(flow.children[2]->measured_size.width == 80.0F);
  REQUIRE(flow.children[2]->frame.x == 0.0F);
  REQUIRE(flow.children[2]->frame.y == 30.0F);
}

TEST_CASE("TestFlowKeepsIntrinsicGrowSizesWithUnboundedWidth") {
  TestPlatform platform;
  Runtime runtime{UnboundedFlowApp, platform};
  runtime.SetViewport({200.0F, 100.0F});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  const auto& flow = *root->children[0];
  REQUIRE(flow.measured_size.width == 65.0F);
  REQUIRE(flow.children[0]->measured_size.width == 30.0F);
  REQUIRE(flow.children[1]->measured_size.width == 30.0F);
  REQUIRE(flow.children[1]->frame.x == 35.0F);
}

TEST_CASE("TestForEachFlattensChildren") {
  TestPlatform platform;
  Runtime runtime{ForEachLayoutApp, platform};
  runtime.SetViewport({200.0F, 160.0F});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children.size() == 5);
  REQUIRE(root->children[0]->text == "Header");
  REQUIRE(root->children[1]->text == "First");
  REQUIRE(root->children[2]->text == "Second");
  REQUIRE(root->children[3]->text == "Third");
  REQUIRE(root->children[4]->text == "Footer");
  REQUIRE(root->children[0]->frame.y == 0.0F);
  REQUIRE(root->children[1]->frame.y == 25.0F);
  REQUIRE(root->children[2]->frame.y == 50.0F);
  REQUIRE(root->children[3]->frame.y == 75.0F);
  REQUIRE(root->children[4]->frame.y == 100.0F);
}

TEST_CASE("TestForEachKeyedIdentity") {
  TestPlatform platform;
  Runtime runtime{ForEachIdentityApp, platform};
  runtime.SetViewport({320.0F, 320.0F});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children.size() == 3);
  const std::uint64_t first_identity = root->children[0]->identity;

  InvokeClick(*root->children[0]->children[0]->children[1]);
  runtime.BuildFrame();
  root = runtime.RootNode();
  REQUIRE(root->children[0]->children[0]->children[0]->text == "1");

  InvokeClick(*root->children[2]);
  runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(root->children.size() == 4);
  REQUIRE(root->children[2]->identity == first_identity);
  REQUIRE(root->children[2]->children[0]->children[0]->text == "1");

  InvokeClick(*root->children[3]);
  runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(root->children.size() == 3);
  REQUIRE(root->children[0]->identity == first_identity);
  REQUIRE(root->children[0]->children[0]->children[0]->text == "1");
}

TEST_CASE("TestReactiveStateApis") {
  State<int> empty;
  REQUIRE(!empty.IsValid());

  TestPlatform platform;
  Runtime runtime{ReactiveStateApiApp, platform};
  runtime.SetViewport({320.0F, 240.0F});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children.size() == 4);
  REQUIRE(root->children[0]->text == "Taps 2");
  REQUIRE(root->children[1]->text == "Alpha");
  REQUIRE(root->children[2]->text == "Bravo");

  InvokeClick(*root->children[3]);
  runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(root->children.size() == 5);
  REQUIRE(root->children[0]->text == "Taps 3");
  REQUIRE(root->children[3]->text == "Charlie");
}

TEST_CASE("TestCustomLayoutProtocol") {
  TestPlatform platform;
  Runtime runtime{CustomLayoutApp, platform};
  runtime.SetViewport({100.0F, 100.0F});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->kind == huxerui::detail::NodeKind::Layout);
  REQUIRE(root->layout->type == std::type_index(typeid(TestFlow)));
  REQUIRE(root->children.size() == 3);
  REQUIRE(root->children[0]->frame.x == 5.0F);
  REQUIRE(root->children[0]->frame.y == 5.0F);
  REQUIRE(root->children[1]->frame.x == 5.0F);
  REQUIRE(root->children[1]->frame.y == 20.0F);
  REQUIRE(root->children[2]->frame.x == 50.0F);
  REQUIRE(root->children[2]->frame.y == 20.0F);
}

TEST_CASE("TestLayoutTypeParticipatesInIdentity") {
  TestPlatform platform;
  Runtime runtime{LayoutIdentityApp, platform};
  runtime.SetViewport({100.0F, 100.0F});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  const std::uint64_t row_identity = root->identity;
  REQUIRE(root->layout->type == std::type_index(typeid(Row)));

  use_column_layout = true;
  runtime.BuildFrame();
  root = runtime.RootNode();
  REQUIRE(root->identity != row_identity);
  REQUIRE(root->layout->type == std::type_index(typeid(Column)));
}

} // namespace huxerui::test
