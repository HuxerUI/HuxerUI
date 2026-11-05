#include "runtime_test_support.h"

#include <algorithm>
#include <limits>

namespace huxerui::test {
namespace {

struct SplitFixture;
SplitFixture* current_fixture = nullptr;
State<int> retained_pane_values[3];

View StatefulSplitPane(std::size_t index) {
  const auto value = UseState(static_cast<int>(index));
  retained_pane_values[index] = value;
  return Column {
    Text(std::to_string(index)),
    Text::Format("Value {}", value.Get()),
  };
}

struct SplitFixture {
  SplitFixture() { current_fixture = this; }
  std::vector<Frame> frames{{.width = 120.0F}, {}};
  std::vector<Grow> grows{Grow(0.0F), Grow(1.0F)};
  std::vector<std::size_t> order{0, 1};
  std::vector<std::vector<SplitChange>> proposals;
  Axis axis = Axis::Horizontal;
  bool accept = true;
  bool enabled = true;
  bool visible = true;
  bool unbounded = false;
  bool stateful = false;
  float scale = 1.0F;
  Point offset;

  View Compose() {
    if (!visible) {
      return Text("Removed");
    }
    std::vector<View> children{View{}};
    for (const auto index : order) {
      View pane = stateful ? View(Scope(StatefulSplitPane, index)) : View(Text(std::to_string(index)));
      children.push_back(std::move(pane).With(frames[index], grows[index]).Key(index));
      children.push_back({});
    }
    View split = SplitLayout(std::move(children)).Orientation(axis).OnChanged([this](std::vector<SplitChange> changes) {
      proposals.push_back(changes);
      if (accept) {
        for (const auto& change : changes) {
          frames[order[change.index]] = change.frame;
          grows[order[change.index]] = change.grow;
        }
      }
    }).With(Enabled(enabled), Scale(scale), Offset(offset));
    return unbounded ? View(ScrollView(std::move(split)).ScrollAxis(axis)) : split;
  }
};

View SplitApp() { return current_fixture->Compose(); }

int overflow_clicks = 0;
bool transform_pane_root = false;

View ClippedSplitApp() {
  return SplitLayout {
    Column {
      Button("Overflow").OnClick([] { ++overflow_clicks; })
          .With(Offset(Point{transform_pane_root ? 0.0F : 150.0F, 0.0F})),
    }.With(Frame{.width = 100.0F}, Offset(Point{transform_pane_root ? 150.0F : 0.0F, 0.0F})),
    Text("Neighbor").With(Grow()),
  };
}

View ControlledSplitApp() {
  const auto frames = UseState(std::vector<Frame>{{.width = 100.0F}, {}});
  const auto grows = UseState(std::vector<Grow>{Grow(0.0F), Grow(1.0F)});
  SplitLayout declaration {
    Text("0").With(frames.Get()[0], grows.Get()[0]),
    SplitLayout {Text("nested one"), Text("nested two")}.With(frames.Get()[1], grows.Get()[1]),
  };
  SplitLayout copy(declaration);
  return std::move(copy).OnChanged([=](const std::vector<SplitChange>& changes) {
    auto next_frames = frames.Get();
    auto next_grows = grows.Get();
    for (const auto& change : changes) {
      next_frames[change.index] = change.frame;
      next_grows[change.index] = change.grow;
    }
    frames = std::move(next_frames);
    grows = std::move(next_grows);
  });
}

bool fixed_wrapping_panes = false;
float wrapping_split_scale = 1.0F;
float vertical_split_scale = 1.0F;

View WrappingSplitApp() {
  const std::string text =
      "A long paragraph needs several lines when its pane becomes narrow. More text makes wrapping visible.";
  const auto frames = UseState(fixed_wrapping_panes
      ? std::vector<Frame>{{.width = 196.0F}, {.width = 196.0F}} : std::vector<Frame>{{}, {}});
  const auto grows = UseState(fixed_wrapping_panes
      ? std::vector<Grow>{Grow(0.0F), Grow(0.0F)} : std::vector<Grow>{Grow(), Grow()});
  return ScrollView(Column {
    SplitLayout {
      Text(text).With(frames.Get()[0], grows.Get()[0]),
      Text("Peer").With(frames.Get()[1], grows.Get()[1]),
    }.OnChanged([=](const std::vector<SplitChange>& changes) {
      auto next_frames = frames.Get();
      auto next_grows = grows.Get();
      for (const auto& change : changes) {
        next_frames[change.index] = change.frame;
        next_grows[change.index] = change.grow;
      }
      frames = std::move(next_frames);
      grows = std::move(next_grows);
    }).With(Scale(wrapping_split_scale)),
    Text(text).With(Frame{.width = 196.0F}),
  });
}

class AspectPane final : public Layout<AspectPane> {
public:
  using Layout::Layout;
  static LayoutResult Measure(LayoutContext&, ViewNode&, Constraints constraints) {
    const float height = std::isfinite(constraints.max_height) ? constraints.max_height : 40.0F;
    return LayoutResult{}.SetSize(constraints.Constrain({height * 2.0F, height}));
  }
};

View VerticalIntrinsicSplitApp() {
  const auto frames = UseState(std::vector<Frame>{{}, {}});
  const auto grows = UseState(std::vector<Grow>{Grow(), Grow()});
  return ScrollView(SplitLayout {
    AspectPane{}.With(frames.Get()[0], grows.Get()[0]),
    AspectPane{}.With(frames.Get()[1], grows.Get()[1]),
  }.Orientation(Axis::Vertical)
      .OnChanged([=](const std::vector<SplitChange>& changes) {
        auto next_frames = frames.Get();
        auto next_grows = grows.Get();
        for (const auto& change : changes) {
          next_frames[change.index] = change.frame;
          next_grows[change.index] = change.grow;
        }
        frames = std::move(next_frames);
        grows = std::move(next_grows);
      })
      .With(Scale(vertical_split_scale))
  ).ScrollAxis(Axis::Horizontal);
}

const detail::MountedNode& PaneNode(const UiWindow& window, std::size_t index) {
  const auto* node = FindMountedText(*window.RootNode(), std::to_string(index));
  REQUIRE(node != nullptr);
  return *node;
}

float Width(const UiWindow& window, std::size_t index) { return PaneNode(window, index).bounds.width; }

std::vector<SemanticNode> Separators(UiWindow& window) {
  std::vector<SemanticNode> nodes;
  for (const auto& node : window.LastCommit().semantic_frame->nodes) {
    if (node.role == SemanticRole::Separator) {
      nodes.push_back(node);
    }
  }
  return nodes;
}

Point SeparatorPoint(UiWindow& window, std::size_t index = 0) {
  const auto nodes = Separators(window);
  REQUIRE(index < nodes.size());
  const Rect bounds = nodes[index].bounds;
  return {bounds.x + bounds.width * 0.5F, bounds.y + bounds.height * 0.5F};
}

void Rebuild(UiWindow& window) {
  window.InvalidateRoot();
  window.BuildFrame();
}

} // namespace

TEST_CASE("SplitLayout allocates preferred and bounded weighted panes", "[split]") {
  SplitFixture fixture;
  fixture.frames = {{.width = 120.0F}, {.width = 900.0F, .min_width = 200.0F}, {.max_width = 50.0F}};
  fixture.grows = {Grow(0.0F), Grow(1.0F), Grow(1.0F)};
  fixture.order = {0, 1, 2};
  TestPlatform platform;
  UiWindow window{SplitApp, platform};
  window.SetWindowMetrics({.viewport = {600.0F, 200.0F}});
  window.BuildFrame();
  REQUIRE(Width(window, 0) == Catch::Approx(120.0F));
  REQUIRE(Width(window, 1) == Catch::Approx(414.0F));
  REQUIRE(Width(window, 2) == Catch::Approx(50.0F));
  REQUIRE(Separators(window).size() == 2);
  REQUIRE(fixture.proposals.empty());

  fixture.frames = {{.min_width = 300.0F}, {.min_width = 300.0F}, {.min_width = 300.0F}};
  Rebuild(window);
  for (std::size_t index = 0; index < 3; ++index) {
    REQUIRE(Width(window, index) == Catch::Approx(584.0F / 3.0F));
  }
  REQUIRE(fixture.proposals.empty());
}

TEST_CASE("SplitLayout handles empty single vertical and unbounded content", "[split]") {
  SplitFixture fixture;
  fixture.order.clear();
  TestPlatform platform;
  UiWindow window{SplitApp, platform};
  window.SetWindowMetrics({.viewport = {400.0F, 300.0F}});
  window.BuildFrame();
  REQUIRE(Separators(window).empty());
  fixture.order = {0};
  Rebuild(window);
  REQUIRE(Separators(window).empty());
  REQUIRE(Width(window, 0) == Catch::Approx(120.0F));
  fixture.order = {0, 1};
  fixture.axis = Axis::Vertical;
  fixture.frames[0] = {.height = 100.0F};
  Rebuild(window);
  REQUIRE(PaneNode(window, 0).bounds.height == Catch::Approx(100.0F));
  REQUIRE(PaneNode(window, 1).bounds.height == Catch::Approx(192.0F));
  fixture.unbounded = true;
  fixture.frames[1] = {.height = 70.0F};
  Rebuild(window);
  REQUIRE(PaneNode(window, 1).bounds.height == Catch::Approx(70.0F));
  const auto separator = Separators(window).front();
  REQUIRE((separator.actions & SemanticActionMask(SemanticActionKind::Increment)) == 0);
  REQUIRE(fixture.proposals.empty());
}

TEST_CASE("SplitLayout drag uses a stable origin across accepted writeback", "[split]") {
  SplitFixture fixture;
  TestPlatform platform;
  UiWindow window{SplitApp, platform};
  window.SetWindowMetrics({.viewport = {500.0F, 200.0F}});
  window.BuildFrame();
  const Point start = SeparatorPoint(window);
  window.HandlePointerEvent({PointerEventType::Down, 71, start});
  for (const float delta : {20.0F, 40.0F, 60.0F}) {
    window.HandlePointerEvent({PointerEventType::Move, 71, {start.x + delta, start.y}});
    Rebuild(window);
    REQUIRE(Width(window, 0) == Catch::Approx(120.0F + delta));
    REQUIRE(Width(window, 1) == Catch::Approx(372.0F - delta));
  }
  window.HandlePointerEvent({PointerEventType::Up, 71, {start.x + 60.0F, start.y}});
  REQUIRE(fixture.proposals.size() == 3);
  REQUIRE(fixture.proposals.front().size() == 1);
  REQUIRE(fixture.proposals.front().front().index == 0);
  REQUIRE(fixture.grows[1].factor == 1.0F);
  window.SetWindowMetrics({.viewport = {700.0F, 200.0F}});
  window.BuildFrame();
  REQUIRE(Width(window, 0) == Catch::Approx(180.0F));
  REQUIRE(fixture.proposals.size() == 3);
}

TEST_CASE("SplitLayout rejection cancellation and incompatible updates do not mutate allocation", "[split]") {
  SplitFixture fixture;
  fixture.accept = false;
  TestPlatform platform;
  UiWindow window{SplitApp, platform};
  window.SetWindowMetrics({.viewport = {500.0F, 200.0F}});
  window.BuildFrame();
  const Point start = SeparatorPoint(window);
  window.HandlePointerEvent({PointerEventType::Down, 71, start});
  window.HandlePointerEvent({PointerEventType::Move, 72, {start.x + 20.0F, start.y}});
  REQUIRE(fixture.proposals.empty());
  window.HandlePointerEvent({PointerEventType::Move, 71, {start.x + 20.0F, start.y}});
  Rebuild(window);
  REQUIRE(Width(window, 0) == Catch::Approx(120.0F));
  REQUIRE(fixture.proposals.size() == 1);
  SECTION("Cancel") {
    window.HandlePointerEvent({PointerEventType::Cancel, 71, start});
  }
  SECTION("Escape") {
    window.HandleKeyEvent({.type = KeyEventType::Down, .key = Key::Escape});
  }
  SECTION("Disabled") {
    fixture.enabled = false;
    Rebuild(window);
  }
  SECTION("Reordered") {
    fixture.order = {1, 0};
    Rebuild(window);
  }
  SECTION("Orientation") {
    fixture.axis = Axis::Vertical;
    Rebuild(window);
  }
  SECTION("Programmatic sizing") {
    fixture.frames[0].width = 150.0F;
    Rebuild(window);
  }
  SECTION("Main-axis translation") {
    fixture.offset.x = 20.0F;
    Rebuild(window);
  }
  SECTION("Scale") {
    fixture.scale = 1.1F;
    Rebuild(window);
  }
  SECTION("Window resize") {
    window.SetWindowMetrics({.viewport = {600.0F, 200.0F}});
    window.BuildFrame();
  }
  SECTION("Unmount") {
    fixture.visible = false;
    Rebuild(window);
  }
  window.HandlePointerEvent({PointerEventType::Move, 71, {start.x + 40.0F, start.y}});
  window.HandlePointerEvent({PointerEventType::Up, 71, {start.x + 40.0F, start.y}});
  REQUIRE(fixture.proposals.size() == 1);
}

TEST_CASE("SplitLayout keyboard and semantic resizing share controlled proposals", "[split]") {
  SplitFixture fixture;
  fixture.frames[0] = {.width = 120.0F, .height = 25.0F, .min_width = 100.0F, .max_width = 180.0F};
  TestPlatform platform;
  UiWindow window{SplitApp, platform};
  window.SetWindowMetrics({.viewport = {500.0F, 200.0F}});
  window.BuildFrame();
  window.HandleKeyEvent({.type = KeyEventType::Down, .key = Key::Tab});
  REQUIRE(window.HandleKeyEvent({.type = KeyEventType::Down, .key = Key::ArrowRight}));
  Rebuild(window);
  REQUIRE(Width(window, 0) == Catch::Approx(130.0F));
  auto separator = Separators(window).front();
  REQUIRE(separator.range->minimum == 100.0);
  REQUIRE(separator.range->maximum == 180.0);
  REQUIRE(window.CoreRuntime().PerformSemanticAction(separator.id, {SemanticActionKind::SetValue, 170.0}));
  Rebuild(window);
  REQUIRE(Width(window, 0) == Catch::Approx(170.0F));
  REQUIRE(fixture.frames[0].height == 25.0F);
  REQUIRE(fixture.frames[0].min_width == 100.0F);
  REQUIRE(fixture.frames[0].max_width == 180.0F);
  REQUIRE(window.CoreRuntime().PerformSemanticAction(separator.id, {SemanticActionKind::Decrement, {}}));
  Rebuild(window);
  REQUIRE(Width(window, 0) == Catch::Approx(160.0F));
}

TEST_CASE("SplitLayout proposals preserve peers with bounded weights and clamped preferences", "[split]") {
  SplitFixture fixture;
  fixture.order = {0, 1, 2};
  SECTION("Growing panes") {
    fixture.frames = {{.min_width = 200.0F}, {}, {}};
    fixture.grows = {Grow(1.0F), Grow(2.0F), Grow(1.0F)};
  }
  SECTION("Oversubscribed fixed panes") {
    fixture.frames = {{.width = 1500.0F}, {.width = 1500.0F}, {.width = 1500.0F}};
    fixture.grows = {Grow(0.0F), Grow(0.0F), Grow(0.0F)};
  }
  TestPlatform platform;
  UiWindow window{SplitApp, platform};
  window.SetWindowMetrics({.viewport = {616.0F, 200.0F}});
  window.BuildFrame();
  const float first = Width(window, 0);
  const float second = Width(window, 1);
  const float third = Width(window, 2);
  const Point start = SeparatorPoint(window);
  window.HandlePointerEvent({PointerEventType::Down, 71, start});
  window.HandlePointerEvent({PointerEventType::Move, 71, {start.x + 25.0F, start.y}});
  Rebuild(window);
  REQUIRE(Width(window, 0) == Catch::Approx(first + 25.0F));
  REQUIRE(Width(window, 1) == Catch::Approx(second - 25.0F));
  REQUIRE(Width(window, 2) == Catch::Approx(third));
}

TEST_CASE("SplitLayout keyed panes keep identities after declaration changes", "[split]") {
  SplitFixture fixture;
  fixture.stateful = true;
  fixture.order = {0, 1};
  TestPlatform platform;
  UiWindow window{SplitApp, platform};
  window.SetWindowMetrics({.viewport = {500.0F, 200.0F}});
  window.BuildFrame();
  const auto first = PaneNode(window, 0).identity;
  const auto second = PaneNode(window, 1).identity;
  retained_pane_values[0] = 42;
  retained_pane_values[1] = 73;
  fixture.order = {1, 0};
  Rebuild(window);
  REQUIRE(PaneNode(window, 0).identity == first);
  REQUIRE(PaneNode(window, 1).identity == second);
  REQUIRE(retained_pane_values[0].Get() == 42);
  REQUIRE(retained_pane_values[1].Get() == 73);
  fixture.order = {1};
  Rebuild(window);
  REQUIRE(PaneNode(window, 1).identity == second);
  REQUIRE(Separators(window).empty());
}

TEST_CASE("SplitLayout handles natural sizes and transformed pointer coordinates", "[split]") {
  SplitFixture fixture;
  fixture.frames[0] = {};
  fixture.scale = 1.5F;
  TestPlatform platform;
  UiWindow window{SplitApp, platform};
  window.SetWindowMetrics({.viewport = {300.0F, 160.0F}});
  window.BuildFrame();
  const float initial = Width(window, 0);
  const Point start = SeparatorPoint(window);
  ClickAt(window, start);
  REQUIRE(fixture.proposals.empty());
  window.HandlePointerEvent({PointerEventType::Down, 71, start});
  for (float delta : {15.0F, 30.0F}) {
    window.HandlePointerEvent({PointerEventType::Move, 71, {start.x + delta, start.y}});
    Rebuild(window);
    REQUIRE(Width(window, 0) == Catch::Approx(initial + delta / fixture.scale));
  }
}

TEST_CASE("SplitLayout can resize capped growth without consuming unused space", "[split]") {
  SplitFixture fixture;
  fixture.frames = {{.max_width = 100.0F}, {.width = 100.0F}};
  fixture.grows = {Grow(1.0F), Grow(0.0F)};
  TestPlatform platform;
  UiWindow window{SplitApp, platform};
  window.SetWindowMetrics({.viewport = {500.0F, 200.0F}});
  window.BuildFrame();
  const auto separator = Separators(window).front();
  REQUIRE(window.CoreRuntime().PerformSemanticAction(separator.id, {SemanticActionKind::SetValue, 80.0}));
  Rebuild(window);
  REQUIRE(Width(window, 0) == Catch::Approx(80.0F));
  REQUIRE(Width(window, 1) == Catch::Approx(120.0F));
  REQUIRE(fixture.frames[0].max_width == 100.0F);
}

TEST_CASE("SplitLayout preserves finite geometry for extreme valid weights and tiny viewports", "[split]") {
  SplitFixture fixture;
  fixture.frames = {{.max_width = 100.0F}, {}};
  fixture.grows = {Grow(std::numeric_limits<float>::max()), Grow(std::numeric_limits<float>::denorm_min())};
  TestPlatform platform;
  UiWindow window{SplitApp, platform};
  window.SetWindowMetrics({.viewport = {408.0F, 200.0F}});
  window.BuildFrame();
  REQUIRE(Width(window, 0) == Catch::Approx(100.0F));
  REQUIRE(Width(window, 1) == Catch::Approx(300.0F));
  window.SetWindowMetrics({.viewport = {4.0F, 200.0F}});
  window.BuildFrame();
  REQUIRE(Width(window, 0) == 0.0F);
  REQUIRE(Width(window, 1) == 0.0F);
  REQUIRE(fixture.proposals.empty());
}

TEST_CASE("SplitLayout resize proposals replay mixed constraints", "[split]") {
  SplitFixture fixture;
  fixture.order = {0, 1, 2};
  TestPlatform platform;
  UiWindow window{SplitApp, platform};
  window.SetWindowMetrics({.viewport = {616.0F, 200.0F}});
  for (std::size_t sample = 0; sample < 48; ++sample) {
    INFO(sample);
    fixture.frames = {
        {.width = 120.0F + sample * 17.0F, .min_width = 25.0F, .max_width = 250.0F},
        {.width = 150.0F, .min_width = 40.0F, .max_width = 400.0F},
        {.width = 1500.0F, .min_width = 80.0F, .max_width = sample % 3 == 0 ? 200.0F : 2000.0F},
    };
    fixture.grows = {Grow(sample % 2 ? 0.0F : 1.0F), Grow(sample % 3 ? 2.0F : 0.0F),
                    Grow(sample % 4 ? 0.0F : 0.5F)};
    Rebuild(window);
    const auto separators = Separators(window);
    const auto separator = separators[sample % 2];
    const auto& range = *separator.range;
    if (range.maximum <= range.minimum) {
      continue;
    }
    const float other = Width(window, sample % 2 == 0 ? 2 : 0);
    const float pair = Width(window, sample % 2) + Width(window, sample % 2 + 1);
    const float requested = static_cast<float>((range.minimum + range.maximum) * 0.5);
    REQUIRE(window.CoreRuntime().PerformSemanticAction(
        separator.id, {SemanticActionKind::SetValue, static_cast<double>(requested)}));
    Rebuild(window);
    REQUIRE(Width(window, sample % 2) == Catch::Approx(requested).margin(0.001F));
    REQUIRE(Width(window, sample % 2 + 1) == Catch::Approx(pair - requested).margin(0.001F));
    REQUIRE(Width(window, sample % 2 == 0 ? 2 : 0) == Catch::Approx(other).margin(0.001F));
  }
}

TEST_CASE("SplitLayout validates orientation and uses ordinary child collection", "[split]") {
  REQUIRE_THROWS_AS(SplitLayout{}.Orientation(static_cast<Axis>(99)), std::invalid_argument);
  REQUIRE_NOTHROW(SplitLayout{Views(std::vector<View>{Text("one"), {}, Text("two")}), Text("three")});
}

TEST_CASE("SplitLayout nested declarations copy normally and State writeback recomposes", "[split]") {
  TestPlatform platform;
  UiWindow window{ControlledSplitApp, platform};
  window.SetWindowMetrics({.viewport = {500.0F, 200.0F}});
  window.BuildFrame();
  const auto separators = Separators(window);
  REQUIRE(separators.size() == 2);
  const auto outer = std::ranges::find_if(separators, [](const SemanticNode& node) {
    return node.bounds.x == 100.0F;
  });
  REQUIRE(outer != separators.end());
  REQUIRE(window.CoreRuntime().PerformSemanticAction(outer->id, {SemanticActionKind::SetValue, 150.0}));
  window.BuildFrame();
  REQUIRE(Width(window, 0) == Catch::Approx(150.0F));
  REQUIRE(Separators(window).size() == 2);
}

TEST_CASE("SplitLayout can collapse a growing pane and continue the same drag", "[split]") {
  SplitFixture fixture;
  fixture.frames = {{}, {}};
  fixture.grows = {Grow(1.0F), Grow(1.0F)};
  TestPlatform platform;
  UiWindow window{SplitApp, platform};
  window.SetWindowMetrics({.viewport = {408.0F, 100.0F}});
  window.BuildFrame();
  const Point start = SeparatorPoint(window);
  window.HandlePointerEvent({PointerEventType::Down, 71, start});
  window.HandlePointerEvent({PointerEventType::Move, 71, {start.x - 200.0F, start.y}});
  Rebuild(window);
  REQUIRE(Width(window, 0) == 0.0F);
  window.HandlePointerEvent({PointerEventType::Up, 71, {start.x - 100.0F, start.y}});
  Rebuild(window);
  REQUIRE(Width(window, 0) == Catch::Approx(100.0F));
  REQUIRE(Width(window, 1) == Catch::Approx(300.0F));
  REQUIRE(fixture.proposals.size() == 2);
}

TEST_CASE("SplitLayout clips transformed pane content away from its neighbors", "[split]") {
  SECTION("Descendant transformation") { transform_pane_root = false; }
  SECTION("Pane root transformation") { transform_pane_root = true; }
  overflow_clicks = 0;
  TestPlatform platform;
  UiWindow window{ClippedSplitApp, platform};
  window.SetWindowMetrics({.viewport = {400.0F, 100.0F}});
  window.BuildFrame();
  const auto* button = FindMountedKind(*window.RootNode(), detail::NodeKind::Button);
  REQUIRE(button != nullptr);
  const Rect bounds = button->PresentationBounds();
  REQUIRE(bounds.x >= 150.0F);
  ClickAt(window, {bounds.x + bounds.width * 0.5F, bounds.y + bounds.height * 0.5F});
  REQUIRE(overflow_clicks == 0);
}

TEST_CASE("SplitLayout measures unbounded cross size after main allocation and preserves dragging", "[split]") {
  wrapping_split_scale = 1.0F;
  SECTION("Growing panes") { fixed_wrapping_panes = false; }
  SECTION("Preferred panes") { fixed_wrapping_panes = true; }
  TestPlatform platform;
  UiWindow window{WrappingSplitApp, platform};
  window.SetWindowMetrics({.viewport = {400.0F, 600.0F}});
  window.BuildFrame();
  const auto* column = window.RootNode()->children.front().get();
  REQUIRE(column->children.size() == 2);
  const auto* split = column->children.front()->children.front().get();
  const auto* pane = split->children.front().get();
  const auto* peer = split->children[1].get();
  const auto* reference = column->children.back().get();
  REQUIRE(pane->bounds.width == Catch::Approx(reference->bounds.width));
  REQUIRE(pane->bounds.height == Catch::Approx(reference->bounds.height));
  REQUIRE(peer->bounds.height == Catch::Approx(reference->bounds.height));
  const float initial_height = pane->bounds.height;
  const Point start = SeparatorPoint(window);
  window.HandlePointerEvent({PointerEventType::Down, 71, start});
  for (float delta : {50.0F, 100.0F}) {
    window.HandlePointerEvent({PointerEventType::Move, 71, {start.x + delta, start.y}});
    window.BuildFrame();
    REQUIRE(pane->bounds.width == Catch::Approx(196.0F + delta));
    REQUIRE(pane->bounds.height < initial_height);
  }
  window.HandlePointerEvent({PointerEventType::Up, 71, {start.x + 100.0F, start.y}});
}

TEST_CASE("SplitLayout vertical allocation resolves intrinsic widths from allocated heights", "[split]") {
  vertical_split_scale = 1.0F;
  TestPlatform platform;
  UiWindow window{VerticalIntrinsicSplitApp, platform};
  window.SetWindowMetrics({.viewport = {500.0F, 400.0F}});
  window.BuildFrame();
  const auto* split = window.RootNode()->children.front()->children.front().get();
  REQUIRE(split->bounds.width == Catch::Approx(392.0F));
  REQUIRE(split->children[0]->bounds.height == Catch::Approx(196.0F));
  REQUIRE(split->children[0]->bounds.width == Catch::Approx(392.0F));
  REQUIRE(split->children[1]->bounds.width == Catch::Approx(392.0F));
}

TEST_CASE("SplitLayout accepts transformed cross-size changes during horizontal dragging", "[split]") {
  fixed_wrapping_panes = false;
  wrapping_split_scale = 1.5F;
  TestPlatform platform;
  UiWindow window{WrappingSplitApp, platform};
  window.SetWindowMetrics({.viewport = {400.0F, 600.0F}});
  window.BuildFrame();
  const auto* split = window.RootNode()->children.front()->children.front()->children.front().get();
  const Point start = SeparatorPoint(window);
  window.HandlePointerEvent({PointerEventType::Down, 71, start});
  for (float delta : {50.0F, 100.0F}) {
    window.HandlePointerEvent({PointerEventType::Move, 71, {start.x + delta * wrapping_split_scale, start.y}});
    window.BuildFrame();
    REQUIRE(split->children.front()->bounds.width == Catch::Approx(196.0F + delta));
  }
}

TEST_CASE("SplitLayout accepts transformed cross-size changes during vertical dragging", "[split]") {
  vertical_split_scale = 1.5F;
  TestPlatform platform;
  UiWindow window{VerticalIntrinsicSplitApp, platform};
  window.SetWindowMetrics({.viewport = {600.0F, 400.0F}});
  window.BuildFrame();
  const auto* split = window.RootNode()->children.front()->children.front().get();
  const Point start = SeparatorPoint(window);
  window.HandlePointerEvent({PointerEventType::Down, 71, start});
  for (float delta : {50.0F, 100.0F}) {
    window.HandlePointerEvent({PointerEventType::Move, 71, {start.x, start.y + delta * vertical_split_scale}});
    window.BuildFrame();
    REQUIRE(split->children.front()->bounds.height == Catch::Approx(196.0F + delta));
  }
}

} // namespace huxerui::test
