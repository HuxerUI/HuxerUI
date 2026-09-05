#include "runtime_test_support.h"

#include <limits>
#include <vector>

#include "external_texture_test_support.h"
#include "image_test_support.h"

namespace huxerui::test {

State<bool> use_column_layout;
State<bool> use_long_cached_text;
State<bool> expand_cached_scope;
State<bool> update_opaque_layout_value;
State<std::size_t> indexed_page_selection;
State<std::size_t> pager_selection;
State<std::size_t> reduced_motion_pager_selection;
ImageAsset layout_test_image;
ImageFit layout_test_image_fit = ImageFit::Contain;
HorizontalAlignment layout_test_image_horizontal_alignment = HorizontalAlignment::Center;
VerticalAlignment layout_test_image_vertical_alignment = VerticalAlignment::Center;
ImageSampling layout_test_image_sampling = ImageSampling::Linear;
Size layout_test_image_frame{100.0F, 100.0F};
VectorAsset layout_test_vector;
std::shared_ptr<ExternalTexture> layout_test_external_texture;

class CountingTextPlatform final : public TestPlatform {
public:
  TextLayoutMetrics MeasureText(
      std::string_view text, const TextStyle& style, float max_width, const TextLayoutOptions& options
  ) override {
    ++measure_text_calls;
    return TestPlatform::MeasureText(text, style, max_width, options);
  }

  std::size_t measure_text_calls = 0;
};

View ImageLayoutApp() {
  return Column {
    Image(layout_test_image)
        .Fit(layout_test_image_fit)
        .Align(layout_test_image_horizontal_alignment, layout_test_image_vertical_alignment)
        .Sampling(layout_test_image_sampling)
        .With(Frame{.width = layout_test_image_frame.width, .height = layout_test_image_frame.height}),
  };
}

View VectorImageLayoutApp() {
  return Image(layout_test_vector)
      .Tint(Color::Rgb(90, 120, 180))
      .With(Frame{.width = 100.0F, .height = 100.0F});
}

View ExternalTextureLayoutApp() {
  return Column {
    Image(layout_test_external_texture)
        .Fit(ImageFit::Cover)
        .Align(HorizontalAlignment::End, VerticalAlignment::Start)
        .Sampling(ImageSampling::Nearest)
        .With(Frame{.width = 100.0F, .height = 100.0F}),
  };
}

struct FlowBreakBefore {
  using Value = bool;
};

struct LayoutBoundaryEnvironment {
  static LayoutBoundaryEnvironment Default() {
    return {};
  }

  bool operator==(const LayoutBoundaryEnvironment&) const = default;
};

struct OpaqueLayoutData {
  int value;
};

struct OpaqueLayoutValue {
  using Value = OpaqueLayoutData;
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

View ScopeGrowContent() {
  return Text("Grow content").With(Frame{.height = 20.0F}, Grow{});
}

View ScopeGrowApp() {
  return Column {
    Text("Fixed").With(Frame{.height = 20.0F}),
    Scope(ScopeGrowContent),
  };
}

View ScopeGrowOverrideApp() {
  return Column {
    Text("Fixed").With(Frame{.height = 20.0F}),
    Scope(ScopeGrowContent).With(Grow{0.0F}),
  };
}

View EnvironmentScopeGrowApp() {
  return Column {
    Text("Fixed").With(Frame{.height = 20.0F}),
    ProvideEnvironment(LayoutBoundaryEnvironment{}, Scope(ScopeGrowContent)),
  };
}

View ScopeLayoutValueApp(bool override_value) {
  View scoped = Scope([] {
    return Text("B").With(Frame{40.0F, 10.0F}).LayoutValue<FlowBreakBefore>(true);
  });
  if (override_value) {
    scoped = std::move(scoped).LayoutValue<FlowBreakBefore>(false);
  }
  return TestFlow {
    Text("A").With(Frame{40.0F, 10.0F}),
    scoped,
  }.With(Frame{.width = 100.0F});
}

View ScopeLayoutValueInheritedApp() {
  return ScopeLayoutValueApp(false);
}

View ScopeLayoutValueOverrideApp() {
  return ScopeLayoutValueApp(true);
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

View NestedStretchLayoutApp() {
  return Column {
    Column {
      Column {
        Text("Measured once"),
        Spacer().With(Frame{.height = 1.0F}),
      }.With(CrossAlign{CrossAxisAlignment::Stretch}),
      Spacer().With(Frame{.height = 1.0F}),
    }.With(CrossAlign{CrossAxisAlignment::Stretch}),
    Spacer().With(Frame{.height = 1.0F}),
  }.With(CrossAlign{CrossAxisAlignment::Stretch});
}

View WrappedTextApp() {
  return Column {
    Text("abcdefghij"),
  };
}

View AlignedTextApp() {
  return Text("aligned")
      .Align(TextAlign::Trailing)
      .VerticalAlign(TextVerticalAlign::Bottom)
      .With(Frame{80.0F, 40.0F});
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

View CachedLayoutApp() {
  auto long_text = UseState(false);
  use_long_cached_text = long_text;
  return Column {
    Text(long_text.Get() ? "Longer label" : "Short"),
    Text("Stable").LayoutValue<FlowBreakBefore>(false),
  };
}

View OpaqueLayoutValueApp() {
  auto update = UseState(false);
  update_opaque_layout_value = update;
  return Text("Opaque")
      .LayoutValue<OpaqueLayoutValue>(OpaqueLayoutData{1})
      .With(Foreground{update.Get() ? Color::Black() : Color::White()});
}

View CachedScope() {
  HUXERUI_SCOPE({
    auto expanded = UseState(false);
    expand_cached_scope = expanded;
    return Text("Scoped").With(Frame{.height = expanded.Get() ? 40.0F : 20.0F});
  });
}

View ScopedCachedLayoutApp() {
  return Column {
    CachedScope(),
    Text("Stable"),
  };
}

View IndexedCounterPage(std::string label) {
  HUXERUI_SCOPE({
    auto count = UseState(0);
    return Column {
      Text::Format("{} {}", label, count),
      Button("Increment " + label).OnClick([count] { count += 1; }),
    };
  });
}

View IndexedPagesApp() {
  auto selected = UseState<std::size_t>(0);
  indexed_page_selection = selected;
  return IndexedPages(
      {
          IndexedCounterPage("First"),
          IndexedCounterPage("Second"),
      },
      selected
  );
}

View PagerLayoutApp() {
  auto selected = UseState<std::size_t>(1);
  pager_selection = selected;
  return Pager(
      {
          Text("First"),
          Text("Second"),
          Text("Third"),
          Text("Fourth").With(Semantics{.selected = false}),
      },
      selected
  );
}

View ReducedMotionPagerApp() {
  auto selected = UseState<std::size_t>(0);
  reduced_motion_pager_selection = selected;
  ThemeSpec theme = FlatLightThemeSpec();
  theme.motion.reduced_motion = true;
  return Theme {
    ThemeDefinition{theme},
    Pager({Text("First"), Text("Second")}, selected),
  };
}

TEST_CASE("FrameAggregateLeavesOmittedConstraintsUnspecified") {
  const Frame frame{.height = 48.0F};
  REQUIRE(frame.height == 48.0F);
  REQUIRE_FALSE(frame.width.has_value());
  REQUIRE_FALSE(frame.min_width.has_value());
  REQUIRE_FALSE(frame.max_width.has_value());
  REQUIRE_FALSE(frame.min_height.has_value());
  REQUIRE_FALSE(frame.max_height.has_value());
}

TEST_CASE("TestMainAndCrossAxisAlignment") {
  TestPlatform platform;
  Runtime runtime{AxisAlignmentApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->bounds.width == 100.0F);
  REQUIRE(root->bounds.height == 100.0F);
  REQUIRE(root->children[0]->layout_offset.x == 40.0F);
  REQUIRE(root->children[0]->layout_offset.y == 0.0F);
  REQUIRE(root->children[1]->layout_offset.x == 40.0F);
  REQUIRE(root->children[1]->layout_offset.y == 80.0F);
}

TEST_CASE("IndexedPages validates its page set and selection") {
  REQUIRE_THROWS_AS(IndexedPages({}, 0), std::invalid_argument);
  REQUIRE_THROWS_AS(IndexedPages({Text("Page")}, 1), std::invalid_argument);
  REQUIRE_THROWS_AS(IndexedPages({View{}}, 0), std::invalid_argument);
}

TEST_CASE("IndexedPages retains every page and presents only the selected page") {
  TestPlatform platform;
  Runtime runtime{IndexedPagesApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 120.0F}});
  const FlattenedScene& initial = runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children.size() == 2);
  const std::uint64_t first_identity = root->children[0]->identity;
  const std::uint64_t second_identity = root->children[1]->identity;
  REQUIRE(root->children[0]->participates_in_layout);
  REQUIRE_FALSE(root->children[1]->participates_in_layout);
  REQUIRE(ContainsText(initial, "First 0"));
  REQUIRE_FALSE(ContainsText(initial, "Second 0"));
  REQUIRE(runtime.LastCommit().semantic_frame != nullptr);
  const auto has_semantic_label = [&runtime](std::string_view label) {
    return std::ranges::any_of(runtime.LastCommit().semantic_frame->nodes, [label](const SemanticNode& node) {
      return node.label == label;
    });
  };
  REQUIRE(has_semantic_label("Increment First"));
  REQUIRE_FALSE(has_semantic_label("Increment Second"));

  const std::uint64_t measure_revision = root->measure_revision;
  const std::uint64_t layout_revision = root->layout_revision;
  runtime.BuildFrame();
  root = runtime.RootNode();
  REQUIRE(root->measure_revision == measure_revision);
  REQUIRE(root->layout_revision == layout_revision);

  const Rect first_button = root->children[0]->children[0]->children[1]->PresentationBounds();
  ClickAt(runtime, {first_button.x + first_button.width * 0.5F, first_button.y + first_button.height * 0.5F});
  REQUIRE(ContainsText(runtime.BuildFrame(), "First 1"));

  indexed_page_selection = 1;
  const FlattenedScene& second = runtime.BuildFrame();
  root = runtime.RootNode();
  REQUIRE(root->children[0]->identity == first_identity);
  REQUIRE(root->children[1]->identity == second_identity);
  REQUIRE_FALSE(root->children[0]->participates_in_layout);
  REQUIRE(root->children[1]->participates_in_layout);
  REQUIRE_FALSE(ContainsText(second, "First 1"));
  REQUIRE(ContainsText(second, "Second 0"));
  REQUIRE_FALSE(has_semantic_label("Increment First"));
  REQUIRE(has_semantic_label("Increment Second"));

  indexed_page_selection = 0;
  const FlattenedScene& restored = runtime.BuildFrame();
  root = runtime.RootNode();
  REQUIRE(root->children[0]->identity == first_identity);
  REQUIRE(root->children[1]->identity == second_identity);
  REQUIRE(ContainsText(restored, "First 1"));
  REQUIRE_FALSE(ContainsText(restored, "Second 0"));
}

TEST_CASE("Pager validates and retains its controlled page set") {
  REQUIRE_THROWS_AS(Pager({}, 0), std::invalid_argument);
  REQUIRE_THROWS_AS(Pager({Text("Page")}, 1), std::invalid_argument);
  REQUIRE_THROWS_AS(Pager({View{}}, 0), std::invalid_argument);

  TestPlatform platform;
  Runtime runtime{PagerLayoutApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 120.0F}});
  const FlattenedScene& initial = runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->kind == detail::NodeKind::ScrollView);
  REQUIRE(root->children.size() == 4);
  REQUIRE_FALSE(root->children[0]->participates_in_layout);
  REQUIRE(root->children[1]->participates_in_layout);
  REQUIRE_FALSE(root->children[2]->participates_in_layout);
  REQUIRE_FALSE(root->children[3]->participates_in_layout);
  REQUIRE(root->scroll_state->offset_x == 240.0F);
  REQUIRE(root->scroll_state->content_width == 720.0F);
  REQUIRE_FALSE(ContainsText(initial, "First"));
  REQUIRE(ContainsText(initial, "Second"));
  REQUIRE_FALSE(ContainsText(initial, "Third"));
  REQUIRE(runtime.LastCommit().semantic_frame != nullptr);
  const auto second_semantics = std::ranges::find(
      runtime.LastCommit().semantic_frame->nodes, std::string{"Second"}, &SemanticNode::label
  );
  REQUIRE(second_semantics != runtime.LastCommit().semantic_frame->nodes.end());
  REQUIRE(second_semantics->selected == std::optional{true});
  REQUIRE(second_semantics->collection_item == std::optional{SemanticCollectionItem{.index = 1}});

  const std::uint64_t measure_revision = root->measure_revision;
  const std::uint64_t layout_revision = root->layout_revision;
  runtime.InvalidateRoot();
  runtime.BuildFrame();
  root = runtime.RootNode();
  REQUIRE(root->measure_revision == measure_revision);
  REQUIRE(root->layout_revision == layout_revision);

  const std::uint64_t first_identity = root->children[0]->identity;
  runtime.SetWindowMetrics({.viewport = {300.0F, 120.0F}});
  runtime.BuildFrame();
  root = runtime.RootNode();
  REQUIRE(root->scroll_state->offset_x == 300.0F);
  REQUIRE(root->children[1]->PresentationBounds().x == Catch::Approx(0.0F));

  pager_selection = 3;
  runtime.BuildFrame();
  platform.AdvanceTime(0.25);
  const FlattenedScene& selected = runtime.BuildFrame();
  runtime.BuildFrame();
  root = runtime.RootNode();
  REQUIRE(root->children[0]->identity == first_identity);
  REQUIRE(root->children[3]->participates_in_layout);
  REQUIRE(ContainsText(selected, "Fourth"));
  const auto fourth_semantics = std::ranges::find(
      runtime.LastCommit().semantic_frame->nodes, std::string{"Fourth"}, &SemanticNode::label
  );
  REQUIRE(fourth_semantics != runtime.LastCommit().semantic_frame->nodes.end());
  REQUIRE(fourth_semantics->selected == std::optional{false});
}

TEST_CASE("Pager resolves programmatic selection immediately under reduced motion") {
  TestPlatform platform;
  Runtime runtime{ReducedMotionPagerApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  runtime.BuildFrame();

  reduced_motion_pager_selection = 1;
  runtime.BuildFrame();
  const FlattenedScene& settled = runtime.BuildFrame();
  REQUIRE_FALSE(ContainsText(settled, "First"));
  REQUIRE(ContainsText(settled, "Second"));
}

TEST_CASE("TestImageMeasuresIntrinsicSizeAndResolvesContainFit") {
  layout_test_image = ImageAsset::FromEncoded(MakeTestPng(40, 20), 2.0F);
  layout_test_image_fit = ImageFit::Contain;
  layout_test_image_horizontal_alignment = HorizontalAlignment::Center;
  layout_test_image_vertical_alignment = VerticalAlignment::Center;
  layout_test_image_sampling = ImageSampling::Linear;
  layout_test_image_frame = {100.0F, 100.0F};

  TestPlatform platform;
  Runtime runtime{ImageLayoutApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 200.0F}});
  const FlattenedScene& scene = runtime.BuildFrame();

  const auto image = std::ranges::find_if(scene.Commands(), [](const PaintCommand& command) {
    return std::holds_alternative<DrawImageCommand>(command);
  });
  REQUIRE(image != scene.Commands().end());
  const auto& command = std::get<DrawImageCommand>(*image);
  REQUIRE(command.source == Rect{0.0F, 0.0F, 20.0F, 10.0F});
  REQUIRE(command.destination == Rect{0.0F, 25.0F, 100.0F, 50.0F});
}

TEST_CASE("TestVectorImageUsesImageLayoutAndSharedPathPainting") {
  layout_test_vector = VectorAsset::Create({20.0F, 10.0F}, [](VectorBuilder& builder) {
    builder.FillPath(
        Path{}.MoveTo({0.0F, 0.0F}).LineTo({20.0F, 0.0F}).LineTo({20.0F, 10.0F}).Close(),
        Color::Black()
    );
  });
  TestPlatform platform;
  Runtime runtime{VectorImageLayoutApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 200.0F}});
  const FlattenedScene& scene = runtime.BuildFrame();

  const auto fill = std::ranges::find_if(scene.Commands(), [](const PaintCommand& command) {
    return std::holds_alternative<FillPathCommand>(command);
  });
  REQUIRE(fill != scene.Commands().end());
  REQUIRE(BrushIsColor(std::get<FillPathCommand>(*fill).brush, Color::Rgb(90, 120, 180)));
  REQUIRE(std::ranges::none_of(scene.Commands(), [](const PaintCommand& command) {
    return std::holds_alternative<DrawImageCommand>(command);
  }));
}

TEST_CASE("TestImageRejectsConfigurationForTheWrongAssetFormat") {
  const ImageAsset raster = ImageAsset::FromEncoded(MakeTestPng(20, 10));
  const VectorAsset vector = VectorAsset::Create({20.0F, 10.0F}, [](VectorBuilder& builder) {
    builder.FillPath(Path{}.MoveTo({0.0F, 0.0F}).LineTo({20.0F, 10.0F}), Color::Black());
  });

  REQUIRE_THROWS_AS(Image(raster).Tint(Color::Black()), std::invalid_argument);
  REQUIRE_THROWS_AS(Image(vector).Sampling(ImageSampling::Linear), std::invalid_argument);
  layout_test_external_texture = MakeTestExternalTexture({20.0F, 10.0F});
  REQUIRE_THROWS_AS(Image(layout_test_external_texture).Tint(Color::Black()), std::invalid_argument);
}

TEST_CASE("TestExternalTextureUsesImageFitAlignmentAndSampling") {
  layout_test_external_texture = MakeTestExternalTexture({20.0F, 10.0F});
  TestPlatform platform;
  Runtime runtime{ExternalTextureLayoutApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 200.0F}});
  const FlattenedScene& scene = runtime.BuildFrame();
  const auto command = std::ranges::find_if(scene.Commands(), [](const PaintCommand& value) {
    return std::holds_alternative<DrawExternalTextureCommand>(value);
  });

  REQUIRE(command != scene.Commands().end());
  const DrawExternalTextureCommand& texture = std::get<DrawExternalTextureCommand>(*command);
  REQUIRE(texture.source == Rect{10.0F, 0.0F, 10.0F, 10.0F});
  REQUIRE(texture.destination == Rect{0.0F, 0.0F, 100.0F, 100.0F});
  REQUIRE(texture.sampling == ImageSampling::Nearest);
}

TEST_CASE("TestImageFitAndAlignmentResolveSourceAndDestinationGeometry") {
  layout_test_image = ImageAsset::FromEncoded(MakeTestPng(40, 20), 2.0F);
  layout_test_image_sampling = ImageSampling::Linear;
  layout_test_image_frame = {100.0F, 100.0F};
  TestPlatform platform;

  const auto render = [&platform](ImageFit fit, HorizontalAlignment horizontal, VerticalAlignment vertical) {
    layout_test_image_fit = fit;
    layout_test_image_horizontal_alignment = horizontal;
    layout_test_image_vertical_alignment = vertical;
    Runtime runtime{ImageLayoutApp, platform};
    runtime.SetWindowMetrics({.viewport = {200.0F, 200.0F}});
    const FlattenedScene& scene = runtime.BuildFrame();
    const auto command = std::ranges::find_if(scene.Commands(), [](const PaintCommand& value) {
      return std::holds_alternative<DrawImageCommand>(value);
    });
    REQUIRE(command != scene.Commands().end());
    return std::get<DrawImageCommand>(*command);
  };

  const DrawImageCommand fill = render(ImageFit::Fill, HorizontalAlignment::Center, VerticalAlignment::Center);
  REQUIRE(fill.source == Rect{0.0F, 0.0F, 20.0F, 10.0F});
  REQUIRE(fill.destination == Rect{0.0F, 0.0F, 100.0F, 100.0F});

  const DrawImageCommand cover = render(ImageFit::Cover, HorizontalAlignment::End, VerticalAlignment::Start);
  REQUIRE(cover.source == Rect{10.0F, 0.0F, 10.0F, 10.0F});
  REQUIRE(cover.destination == Rect{0.0F, 0.0F, 100.0F, 100.0F});

  const DrawImageCommand none = render(ImageFit::None, HorizontalAlignment::End, VerticalAlignment::Start);
  REQUIRE(none.source == Rect{0.0F, 0.0F, 20.0F, 10.0F});
  REQUIRE(none.destination == Rect{80.0F, 0.0F, 20.0F, 10.0F});

  const DrawImageCommand scale_down =
      render(ImageFit::ScaleDown, HorizontalAlignment::Center, VerticalAlignment::Center);
  REQUIRE(scale_down.destination == Rect{40.0F, 45.0F, 20.0F, 10.0F});

  layout_test_image_frame = {10.0F, 10.0F};
  const DrawImageCommand constrained_scale_down =
      render(ImageFit::ScaleDown, HorizontalAlignment::Center, VerticalAlignment::Center);
  REQUIRE(constrained_scale_down.destination == Rect{0.0F, 2.5F, 10.0F, 5.0F});
}

TEST_CASE("TestImagePaintOnlyChangesReuseMeasuredLayout") {
  layout_test_image = ImageAsset::FromEncoded(MakeTestPng(40, 20), 2.0F);
  layout_test_image_fit = ImageFit::Contain;
  layout_test_image_horizontal_alignment = HorizontalAlignment::Center;
  layout_test_image_vertical_alignment = VerticalAlignment::Center;
  layout_test_image_sampling = ImageSampling::Linear;
  layout_test_image_frame = {100.0F, 100.0F};
  TestPlatform platform;
  Runtime runtime{ImageLayoutApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 200.0F}});
  runtime.BuildFrame();

  const detail::MountedNode* image = runtime.RootNode()->children.front().get();
  const std::uint64_t measure_revision = image->measure_revision;
  const std::uint64_t layout_revision = image->layout_revision;
  const std::uint64_t content_revision = image->render_node.content.Revision();

  layout_test_image = ImageAsset::FromEncoded(MakeTestPng(80, 40), 4.0F);
  layout_test_image_fit = ImageFit::Cover;
  layout_test_image_horizontal_alignment = HorizontalAlignment::End;
  layout_test_image_vertical_alignment = VerticalAlignment::Start;
  layout_test_image_sampling = ImageSampling::Nearest;
  runtime.InvalidateRoot();
  runtime.BuildFrame();

  image = runtime.RootNode()->children.front().get();
  REQUIRE(image->measure_revision == measure_revision);
  REQUIRE(image->layout_revision == layout_revision);
  REQUIRE(image->render_node.content.Revision() > content_revision);
}

TEST_CASE("TestSpacerAndGrowLayout") {
  TestPlatform platform;
  Runtime runtime{SpacerLayoutApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 60.0F}});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children[0]->layout_offset.x == 0.0F);
  REQUIRE(root->children[0]->layout_offset.y == 20.0F);
  REQUIRE(root->children[1]->layout_offset.x == 20.0F);
  REQUIRE(root->children[1]->bounds.width == 150.0F);
  REQUIRE(root->children[2]->layout_offset.x == 170.0F);
  REQUIRE(root->children[2]->layout_offset.y == 20.0F);

  Runtime grow_runtime{GrowLayoutApp, platform};
  grow_runtime.SetWindowMetrics({.viewport = {300.0F, 40.0F}});
  grow_runtime.BuildFrame();

  root = grow_runtime.RootNode();
  REQUIRE(root->children[0]->bounds.width == 100.0F);
  REQUIRE(root->children[1]->layout_offset.x == 100.0F);
  REQUIRE(root->children[1]->bounds.width == 200.0F);
}

TEST_CASE("Scope and Environment expose effective parent layout values") {
  TestPlatform platform;

  Runtime scope_runtime{ScopeGrowApp, platform};
  scope_runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  scope_runtime.BuildFrame();
  const auto* root = scope_runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children[1]->kind == detail::NodeKind::Scope);
  REQUIRE(root->children[1]->bounds.height == 80.0F);
  REQUIRE(root->children[1]->children[0]->bounds.height == 80.0F);

  Runtime override_runtime{ScopeGrowOverrideApp, platform};
  override_runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  override_runtime.BuildFrame();
  root = override_runtime.RootNode();
  REQUIRE(root->children[1]->bounds.height == 20.0F);
  REQUIRE(root->children[1]->children[0]->bounds.height == 20.0F);

  Runtime environment_runtime{EnvironmentScopeGrowApp, platform};
  environment_runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  environment_runtime.BuildFrame();
  root = environment_runtime.RootNode();
  REQUIRE(root->children[1]->kind == detail::NodeKind::Environment);
  REQUIRE(root->children[1]->bounds.height == 80.0F);
  REQUIRE(root->children[1]->children[0]->kind == detail::NodeKind::Scope);
  REQUIRE(root->children[1]->children[0]->children[0]->bounds.height == 80.0F);
}

TEST_CASE("Scope layout values use boundary override precedence") {
  TestPlatform platform;

  Runtime inherited_runtime{ScopeLayoutValueInheritedApp, platform};
  inherited_runtime.SetWindowMetrics({.viewport = {100.0F, 40.0F}});
  inherited_runtime.BuildFrame();
  const auto* root = inherited_runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children[1]->layout_offset.x == 0.0F);
  REQUIRE(root->children[1]->layout_offset.y == 10.0F);

  Runtime override_runtime{ScopeLayoutValueOverrideApp, platform};
  override_runtime.SetWindowMetrics({.viewport = {100.0F, 40.0F}});
  override_runtime.BuildFrame();
  root = override_runtime.RootNode();
  REQUIRE(root->children[1]->layout_offset.x == 40.0F);
  REQUIRE(root->children[1]->layout_offset.y == 0.0F);
}

TEST_CASE("TestStackAndStretchAlignment") {
  TestPlatform platform;
  Runtime stack_runtime{StackAlignmentApp, platform};
  stack_runtime.SetWindowMetrics({.viewport = {100.0F, 80.0F}});
  stack_runtime.BuildFrame();

  const auto* root = stack_runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children[0]->layout_offset.x == 80.0F);
  REQUIRE(root->children[0]->layout_offset.y == 35.0F);

  Runtime stretch_runtime{StretchLayoutApp, platform};
  stretch_runtime.SetWindowMetrics({.viewport = {120.0F, 80.0F}});
  stretch_runtime.BuildFrame();

  root = stretch_runtime.RootNode();
  REQUIRE(root->children[0]->bounds.width == 120.0F);
}

TEST_CASE("TestTightCrossAxisAvoidsNestedStretchRemeasurement") {
  CountingTextPlatform platform;
  AppOptions options;
  options.show_debug_overlay = false;
  Runtime runtime{NestedStretchLayoutApp, platform, std::move(options)};
  runtime.SetWindowMetrics({.viewport = {120.0F, 80.0F}});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->bounds.width == 120.0F);
  REQUIRE(root->children.size() == 2);
  REQUIRE(root->children[0]->bounds.width == 120.0F);
  REQUIRE(root->children[0]->children.size() == 2);
  REQUIRE(root->children[0]->children[0]->bounds.width == 120.0F);
  REQUIRE(root->children[0]->children[0]->children.size() == 2);
  REQUIRE(root->children[0]->children[0]->children[0]->bounds.width == 120.0F);
  REQUIRE(platform.measure_text_calls == 1);
}

TEST_CASE("TestWrappedTextMeasurement") {
  TestPlatform platform;
  Runtime runtime{WrappedTextApp, platform};
  runtime.SetWindowMetrics({.viewport = {40.0F, 100.0F}});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children[0]->bounds.width == 40.0F);
  REQUIRE(root->children[0]->bounds.height == 60.0F);
}

TEST_CASE("TextCarriesHorizontalAndVerticalAlignmentIntoParagraphPainting") {
  TestPlatform platform;
  Runtime runtime{AlignedTextApp, platform};
  runtime.SetWindowMetrics({.viewport = {80.0F, 40.0F}});
  const FlattenedScene& scene = runtime.BuildFrame();

  const DrawTextCommand* text = FindText(scene, "aligned");
  REQUIRE(text != nullptr);
  REQUIRE(text->options.align == TextAlign::Trailing);
  REQUIRE(text->options.vertical_align == TextVerticalAlign::Bottom);
}

TEST_CASE("TestAdaptiveFrameConstraints") {
  TestPlatform platform;
  Runtime runtime{AdaptiveFrameApp, platform};
  runtime.SetWindowMetrics({.viewport = {120.0F, 400.0F}});
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
  constrained.SetWindowMetrics({.viewport = {80.0F, 60.0F}});
  constrained.BuildFrame();

  const auto* root = constrained.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->measured_size.width == 80.0F);
  REQUIRE(root->measured_size.height == 60.0F);

  Runtime containers{BoundedContainerFrameApp, platform};
  containers.SetWindowMetrics({.viewport = {120.0F, 200.0F}});
  containers.BuildFrame();

  root = containers.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children[0]->measured_size.height == 40.0F);
  REQUIRE(root->children[1]->measured_size.height == 60.0F);
}

TEST_CASE("TestFrameConstraintValidation") {
  {
    TestPlatform platform;
    Runtime runtime{[] { return Text("A").With(Frame{.width = -1.0F}); }, platform};
    REQUIRE_THROWS_AS(runtime.BuildFrame(), std::invalid_argument);
  }
  {
    TestPlatform platform;
    Runtime runtime{[] { return Text("A").With(Frame{.min_width = 50.0F, .max_width = 40.0F}); }, platform};
    REQUIRE_THROWS_AS(runtime.BuildFrame(), std::invalid_argument);
  }
  {
    TestPlatform platform;
    Runtime runtime{
        [] { return Text("A").With(Frame{.min_height = 50.0F}, Frame{.max_height = 40.0F}); },
        platform,
    };
    REQUIRE_THROWS_AS(runtime.BuildFrame(), std::invalid_argument);
  }
}

TEST_CASE("TestFlowWrapsAndAlignsChildrenWithinLines") {
  TestPlatform platform;
  Runtime runtime{FlowWrapApp, platform};
  runtime.SetWindowMetrics({.viewport = {120.0F, 100.0F}});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  const auto& flow = *root->children[0];
  REQUIRE(flow.layout_descriptor->type == std::type_index(typeid(Flow)));
  REQUIRE(flow.measured_size.width == 90.0F);
  REQUIRE(flow.measured_size.height == 40.0F);
  REQUIRE(flow.children[0]->layout_offset.x == 0.0F);
  REQUIRE(flow.children[0]->layout_offset.y == 5.0F);
  REQUIRE(flow.children[1]->layout_offset.x == 45.0F);
  REQUIRE(flow.children[1]->layout_offset.y == 0.0F);
  REQUIRE(flow.children[2]->layout_offset.x == 0.0F);
  REQUIRE(flow.children[2]->layout_offset.y == 25.0F);
}

TEST_CASE("TestFlowAppliesMainAlignmentPerLine") {
  TestPlatform platform;
  Runtime runtime{FlowAlignmentApp, platform};
  runtime.SetWindowMetrics({.viewport = {120.0F, 100.0F}});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  const auto& flow = *root->children[0];
  REQUIRE(flow.children[0]->layout_offset.x == 15.0F);
  REQUIRE(flow.children[1]->layout_offset.x == 55.0F);
}

TEST_CASE("TestFlowDistributesGrowWithinEachLine") {
  TestPlatform platform;
  Runtime runtime{FlowGrowApp, platform};
  runtime.SetWindowMetrics({.viewport = {120.0F, 100.0F}});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  const auto& flow = *root->children[0];
  REQUIRE(flow.children[0]->measured_size.width == 30.0F);
  REQUIRE(flow.children[0]->layout_offset.y == 5.0F);
  REQUIRE(flow.children[1]->measured_size.width == 60.0F);
  REQUIRE(flow.children[1]->layout_offset.x == 40.0F);
  REQUIRE(flow.children[2]->measured_size.width == 80.0F);
  REQUIRE(flow.children[2]->layout_offset.x == 0.0F);
  REQUIRE(flow.children[2]->layout_offset.y == 30.0F);
}

TEST_CASE("TestFlowKeepsIntrinsicGrowSizesWithUnboundedWidth") {
  TestPlatform platform;
  Runtime runtime{UnboundedFlowApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  const auto& flow = *root->children[0];
  REQUIRE(flow.measured_size.width == 65.0F);
  REQUIRE(flow.children[0]->measured_size.width == 30.0F);
  REQUIRE(flow.children[1]->measured_size.width == 30.0F);
  REQUIRE(flow.children[1]->layout_offset.x == 35.0F);
}

TEST_CASE("TestForEachFlattensChildren") {
  TestPlatform platform;
  Runtime runtime{ForEachLayoutApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 160.0F}});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children.size() == 5);
  REQUIRE(root->children[0]->text == "Header");
  REQUIRE(root->children[1]->text == "First");
  REQUIRE(root->children[2]->text == "Second");
  REQUIRE(root->children[3]->text == "Third");
  REQUIRE(root->children[4]->text == "Footer");
  REQUIRE(root->children[0]->layout_offset.y == 0.0F);
  REQUIRE(root->children[1]->layout_offset.y == 25.0F);
  REQUIRE(root->children[2]->layout_offset.y == 50.0F);
  REQUIRE(root->children[3]->layout_offset.y == 75.0F);
  REQUIRE(root->children[4]->layout_offset.y == 100.0F);
}

TEST_CASE("TestForEachKeyedIdentity") {
  TestPlatform platform;
  Runtime runtime{ForEachIdentityApp, platform};
  runtime.SetWindowMetrics({.viewport = {320.0F, 320.0F}});
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
  runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
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

TEST_CASE("TestLayoutReusesUnchangedMeasurements") {
  TestPlatform platform;
  Runtime runtime{CachedLayoutApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->children.size() == 2);
  const std::uint64_t root_measure_revision = root->measure_revision;
  const std::uint64_t root_layout_revision = root->layout_revision;
  const std::uint64_t changed_measure_revision = root->children[0]->measure_revision;
  const std::uint64_t stable_measure_revision = root->children[1]->measure_revision;
  const std::uint64_t stable_layout_revision = root->children[1]->layout_revision;

  runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(root->measure_revision == root_measure_revision);
  REQUIRE(root->layout_revision == root_layout_revision);
  REQUIRE(root->children[0]->measure_revision == changed_measure_revision);
  REQUIRE(root->children[1]->measure_revision == stable_measure_revision);
  REQUIRE(root->children[1]->layout_revision == stable_layout_revision);

  use_long_cached_text = true;
  runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(root->measure_revision > root_measure_revision);
  REQUIRE(root->children[0]->measure_revision > changed_measure_revision);
  REQUIRE(root->children[1]->measure_revision == stable_measure_revision);
  REQUIRE(root->children[1]->layout_revision == stable_layout_revision);

  runtime.SetWindowMetrics({.viewport = {240.0F, 100.0F}});
  runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(root->children[1]->measure_revision > stable_measure_revision);
}

TEST_CASE("TestScopedLayoutInvalidationPropagatesToAncestors") {
  TestPlatform platform;
  Runtime runtime{ScopedCachedLayoutApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  const std::uint64_t root_measure_revision = root->measure_revision;
  const std::uint64_t scope_measure_revision = root->children[0]->measure_revision;
  const std::uint64_t scoped_text_measure_revision = root->children[0]->children[0]->measure_revision;
  const std::uint64_t stable_measure_revision = root->children[1]->measure_revision;
  const std::uint64_t stable_layout_revision = root->children[1]->layout_revision;

  expand_cached_scope = true;
  runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(root->measure_revision > root_measure_revision);
  REQUIRE(root->children[0]->measure_revision > scope_measure_revision);
  REQUIRE(root->children[0]->children[0]->measure_revision > scoped_text_measure_revision);
  REQUIRE(root->children[1]->measure_revision == stable_measure_revision);
  REQUIRE(root->children[1]->layout_revision > stable_layout_revision);
  REQUIRE(root->children[1]->layout_offset.y == 40.0F);
}

TEST_CASE("TestNonComparableLayoutValueInvalidatesConservatively") {
  TestPlatform platform;
  Runtime runtime{OpaqueLayoutValueApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  const std::uint64_t measure_revision = root->measure_revision;

  update_opaque_layout_value = true;
  runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(root->measure_revision > measure_revision);
}

TEST_CASE("TestCustomLayoutProtocol") {
  TestPlatform platform;
  Runtime runtime{CustomLayoutApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->kind == huxerui::detail::NodeKind::Layout);
  REQUIRE(root->layout_descriptor->type == std::type_index(typeid(TestFlow)));
  REQUIRE(root->children.size() == 3);
  REQUIRE(root->children[0]->bounds.x == 0.0F);
  REQUIRE(root->children[0]->bounds.y == 0.0F);
  REQUIRE(root->children[0]->layout_offset.x == 5.0F);
  REQUIRE(root->children[0]->layout_offset.y == 5.0F);
  REQUIRE(root->children[0]->PresentationBounds().x == 5.0F);
  REQUIRE(root->children[0]->PresentationBounds().y == 5.0F);
  REQUIRE(root->children[1]->layout_offset.x == 5.0F);
  REQUIRE(root->children[1]->layout_offset.y == 20.0F);
  REQUIRE(root->children[2]->layout_offset.x == 50.0F);
  REQUIRE(root->children[2]->layout_offset.y == 20.0F);
}

TEST_CASE("TestLayoutTypeParticipatesInIdentity") {
  TestPlatform platform;
  Runtime runtime{LayoutIdentityApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  const std::uint64_t row_identity = root->identity;
  REQUIRE(root->layout_descriptor->type == std::type_index(typeid(Row)));

  use_column_layout = true;
  runtime.BuildFrame();
  root = runtime.RootNode();
  REQUIRE(root->identity != row_identity);
  REQUIRE(root->layout_descriptor->type == std::type_index(typeid(Column)));
}

} // namespace huxerui::test
