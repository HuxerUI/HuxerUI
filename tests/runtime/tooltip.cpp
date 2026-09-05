#include "runtime_test_support.h"

namespace huxerui::test {

namespace {

int tooltip_clicks = 0;
bool first_hovered = false;
bool second_hovered = false;
State<bool> tooltip_target_visible;
State<bool> tooltip_use_updated_message;
State<int> tooltip_style_mode;

TooltipStyle TestTooltipStyle() {
  TooltipStyle style = TooltipStyle::Default();
  style.hover_delay = 0.5;
  style.exit_delay = 0.1;
  style.long_press_delay = 0.5;
  style.touch_show_duration = 1.5;
  return style;
}

View TestTooltipTheme(View content) {
  ThemeDefinition definition;
  definition.Set(TestTooltipStyle());
  return Theme {std::move(definition), std::move(content)};
}

View TooltipApp() {
  return TestTooltipTheme(Scope([] {
    return Column {
      Button("Target")
          .With(Frame{.width = 100.0F, .height = 40.0F}, Tooltip("Tooltip message"))
          .OnClick([] { ++tooltip_clicks; }),
    };
  }));
}

View DisabledTooltipApp() {
  return TestTooltipTheme(Scope([] {
    return Column {
      Text("Disabled target")
          .With(Frame{.width = 120.0F, .height = 40.0F}, Enabled{false}, Tooltip("Disabled explanation")),
    };
  }));
}

View PlainTooltipApp() {
  return TestTooltipTheme(Scope([] {
    return Text("Plain target").With(Frame{.width = 100.0F, .height = 40.0F}, Tooltip("Plain tooltip"));
  }));
}

View TooltipLifecycleApp() {
  auto target_visible = UseState(true);
  auto use_updated_message = UseState(false);
  tooltip_target_visible = target_visible;
  tooltip_use_updated_message = use_updated_message;
  return TestTooltipTheme(Scope([target_visible, use_updated_message]() -> View {
    if (!target_visible.Get()) {
      return Text("Target removed");
    }
    return Text("Lifecycle target")
        .With(
            Frame{.width = 120.0F, .height = 40.0F},
            Tooltip(use_updated_message.Get() ? "Updated tooltip" : "Initial tooltip")
        );
  }));
}

View MultipleTooltipTargetsApp() {
  return TestTooltipTheme(Scope([] {
    return Column {
      Button("First target")
          .With(Frame{.width = 120.0F, .height = 40.0F}, Tooltip("First tooltip"))
          .OnClick([] {}),
      Button("Second target")
          .With(Frame{.width = 120.0F, .height = 40.0F}, Tooltip("Second tooltip"))
          .OnClick([] {}),
    };
  }));
}

TooltipStyle DynamicTooltipStyle(int mode) {
  TooltipStyle style = TestTooltipStyle();
  style.hover_delay = 0.0;
  style.background = mode == 0 ? Color::Rgb(180, 30, 40) : Color::Rgb(20, 100, 180);
  if (mode == 2) {
    style.maximum_width = 0.0F;
  }
  return style;
}

View DynamicTooltipStyleApp() {
  auto mode = UseState(0);
  tooltip_style_mode = mode;
  ThemeDefinition definition;
  definition.Set(DynamicTooltipStyle(mode.Get()));
  return Theme {
    std::move(definition),
    Text("Dynamic target").With(Frame{.width = 120.0F, .height = 40.0F}, Tooltip("Dynamic tooltip")),
  };
}

struct HoverProbe {
  bool* hovered = nullptr;

  static const detail::ModifierDescriptor& Descriptor();

  bool operator==(const HoverProbe&) const = default;
};

class HoverProbeExtension final : public NodeExtension {
public:
  HoverProbeExtension(ViewNode& node, const HoverProbe& modifier) {
    Update(node, modifier);
  }

  void Update(ViewNode& node, const HoverProbe& modifier) {
    static_cast<void>(node);
    hovered_ = modifier.hovered;
  }

  bool HoverHitTest(ViewNode& node, Point position) const override {
    return node.Bounds().Contains(position);
  }

  void OnHover(ViewNode& node, const HoverEvent& event) override {
    static_cast<void>(node);
    if (hovered_) {
      *hovered_ = event.type != HoverEventType::Leave;
    }
  }

private:
  bool* hovered_ = nullptr;
};

const detail::ModifierDescriptor& HoverProbe::Descriptor() {
  return detail::ModifierDescriptorFor<HoverProbe, HoverProbeExtension>();
}

View MultipleHoverApp() {
  return Column {
    Text("Hover target")
        .With(
            Frame{.width = 100.0F, .height = 40.0F}, HoverProbe{&first_hovered}, HoverProbe{&second_hovered}
        ),
  };
}

void MovePointer(Runtime& runtime, Point position) {
  runtime.HandlePointerEvent({PointerEventType::Move, 1, position, huxerui::PointerDeviceKind::Mouse});
}

} // namespace

TEST_CASE("TestTooltipShowsAfterHoverDelayAndDismissesAfterExitDelay") {
  TestPlatform platform;
  Runtime runtime{TooltipApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 160.0F}});
  REQUIRE(FindText(runtime.BuildFrame(), "Tooltip message") == nullptr);

  MovePointer(runtime, {40.0F, 20.0F});
  REQUIRE(FindText(runtime.BuildFrame(), "Tooltip message") == nullptr);
  platform.AdvanceTime(0.49);
  REQUIRE(FindText(runtime.BuildFrame(), "Tooltip message") == nullptr);
  platform.AdvanceTime(0.02);
  runtime.BuildFrame();
  REQUIRE(FindText(runtime.BuildFrame(), "Tooltip message") != nullptr);

  MovePointer(runtime, {220.0F, 140.0F});
  REQUIRE(FindText(runtime.BuildFrame(), "Tooltip message") != nullptr);
  platform.AdvanceTime(0.11);
  runtime.BuildFrame();
  runtime.BuildFrame();
  REQUIRE(FindText(runtime.BuildFrame(), "Tooltip message") == nullptr);
}

TEST_CASE("TestTooltipRequiresStationaryHoverAndRestartsAfterMovement") {
  TestPlatform platform;
  Runtime runtime{TooltipApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 160.0F}});
  runtime.BuildFrame();

  MovePointer(runtime, {40.0F, 20.0F});
  runtime.BuildFrame();
  platform.AdvanceTime(0.3);
  runtime.BuildFrame();
  MovePointer(runtime, {50.0F, 20.0F});
  runtime.BuildFrame();
  platform.AdvanceTime(0.49);
  REQUIRE(FindText(runtime.BuildFrame(), "Tooltip message") == nullptr);
  platform.AdvanceTime(0.02);
  runtime.BuildFrame();
  REQUIRE(FindText(runtime.BuildFrame(), "Tooltip message") != nullptr);

  MovePointer(runtime, {60.0F, 20.0F});
  runtime.BuildFrame();
  REQUIRE(FindText(runtime.BuildFrame(), "Tooltip message") == nullptr);
  platform.AdvanceTime(0.49);
  REQUIRE(FindText(runtime.BuildFrame(), "Tooltip message") == nullptr);
  platform.AdvanceTime(0.02);
  runtime.BuildFrame();
  REQUIRE(FindText(runtime.BuildFrame(), "Tooltip message") != nullptr);
}

TEST_CASE("TestTooltipRemainsVisibleWhileItsSurfaceIsHovered") {
  TestPlatform platform;
  Runtime runtime{TooltipApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 160.0F}});
  runtime.BuildFrame();

  MovePointer(runtime, {40.0F, 20.0F});
  runtime.BuildFrame();
  platform.AdvanceTime(0.51);
  runtime.BuildFrame();
  const std::optional<Rect> surface = FindPresentedRectWithColor(runtime.BuildFrame(), TestTooltipStyle().background);
  REQUIRE(surface.has_value());

  MovePointer(runtime, {surface->x + surface->width * 0.5F, surface->y + surface->height * 0.5F});
  runtime.BuildFrame();
  platform.AdvanceTime(0.11);
  runtime.BuildFrame();
  REQUIRE(FindText(runtime.BuildFrame(), "Tooltip message") != nullptr);

  MovePointer(runtime, {220.0F, 140.0F});
  runtime.BuildFrame();
  platform.AdvanceTime(0.11);
  runtime.BuildFrame();
  runtime.BuildFrame();
  REQUIRE(FindText(runtime.BuildFrame(), "Tooltip message") == nullptr);
}

TEST_CASE("TestTooltipShowsForKeyboardFocusAndContributesTargetHint") {
  TestPlatform platform;
  Runtime runtime{TooltipApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 160.0F}});
  runtime.BuildFrame();

  runtime.HandleKeyEvent({KeyEventType::Down, Key::Tab});
  const FrameCommit& commit = runtime.BuildCommit();
  bool found_target = false;
  bool found_tooltip_node = false;
  for (const SemanticNode& node : commit.semantic_frame->nodes) {
    if (node.label == "Target") {
      found_target = true;
      REQUIRE(node.hint == "Tooltip message");
    }
    found_tooltip_node = found_tooltip_node || node.label == "Tooltip message";
  }
  REQUIRE(found_target);
  REQUIRE_FALSE(found_tooltip_node);
  REQUIRE(FindText(runtime.BuildFrame(), "Tooltip message") != nullptr);

  runtime.HandleKeyEvent({KeyEventType::Down, Key::Escape});
  REQUIRE(FindText(runtime.BuildFrame(), "Tooltip message") == nullptr);
}

TEST_CASE("TestTooltipLongPressCancelsTargetClick") {
  tooltip_clicks = 0;
  TestPlatform platform;
  Runtime runtime{TooltipApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 160.0F}});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({PointerEventType::Down, 8, {40.0F, 20.0F}, huxerui::PointerDeviceKind::Touch});
  runtime.BuildFrame();
  platform.AdvanceTime(0.51);
  runtime.BuildFrame();
  REQUIRE(FindText(runtime.BuildFrame(), "Tooltip message") != nullptr);
  runtime.HandlePointerEvent({PointerEventType::Move, 8, {60.0F, 20.0F}, huxerui::PointerDeviceKind::Touch});
  runtime.HandlePointerEvent({PointerEventType::Up, 8, {60.0F, 20.0F}, huxerui::PointerDeviceKind::Touch});
  runtime.BuildFrame();
  REQUIRE(tooltip_clicks == 0);

  platform.AdvanceTime(1.51);
  runtime.BuildFrame();
  platform.AdvanceTime(0.11);
  runtime.BuildFrame();
  runtime.BuildFrame();
  REQUIRE(FindText(runtime.BuildFrame(), "Tooltip message") == nullptr);
}

TEST_CASE("TestPlainTooltipSchedulesTouchReleaseAndDismissal") {
  TestPlatform platform;
  Runtime runtime{PlainTooltipApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 160.0F}});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({PointerEventType::Down, 10, {40.0F, 20.0F}, PointerDeviceKind::Touch});
  runtime.BuildFrame();
  platform.AdvanceTime(0.51);
  runtime.BuildFrame();
  REQUIRE(FindText(runtime.BuildFrame(), "Plain tooltip") != nullptr);

  const int requests_before_release = platform.requested_frames;
  runtime.HandlePointerEvent({PointerEventType::Up, 10, {40.0F, 20.0F}, PointerDeviceKind::Touch});
  REQUIRE(platform.requested_frames > requests_before_release);
  runtime.BuildFrame();

  platform.AdvanceTime(1.51);
  runtime.BuildFrame();
  platform.AdvanceTime(0.11);
  runtime.BuildFrame();
  runtime.BuildFrame();
  REQUIRE(FindText(runtime.BuildFrame(), "Plain tooltip") == nullptr);
}

TEST_CASE("TestTooltipTouchCancellationDismissesVisibleSurface") {
  TestPlatform platform;
  Runtime runtime{PlainTooltipApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 160.0F}});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({PointerEventType::Down, 11, {40.0F, 20.0F}, PointerDeviceKind::Touch});
  runtime.BuildFrame();
  platform.AdvanceTime(0.51);
  runtime.BuildFrame();
  REQUIRE(FindText(runtime.BuildFrame(), "Plain tooltip") != nullptr);

  const int requests_before_cancel = platform.requested_frames;
  runtime.HandlePointerEvent({PointerEventType::Cancel, 11, {40.0F, 20.0F}, PointerDeviceKind::Touch});
  REQUIRE(platform.requested_frames > requests_before_cancel);
  REQUIRE(FindText(runtime.BuildFrame(), "Plain tooltip") == nullptr);
}

TEST_CASE("TestTooltipRetainsTouchOwnershipUntilEveryAcceptedPointerEnds") {
  TestPlatform platform;
  Runtime runtime{PlainTooltipApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 160.0F}});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({PointerEventType::Down, 12, {40.0F, 20.0F}, PointerDeviceKind::Touch});
  runtime.HandlePointerEvent({PointerEventType::Down, 13, {60.0F, 20.0F}, PointerDeviceKind::Touch});
  platform.AdvanceTime(0.51);
  runtime.BuildFrame();
  REQUIRE(FindText(runtime.BuildFrame(), "Plain tooltip") != nullptr);

  runtime.HandlePointerEvent({PointerEventType::Up, 12, {40.0F, 20.0F}, PointerDeviceKind::Touch});
  REQUIRE(FindText(runtime.BuildFrame(), "Plain tooltip") != nullptr);

  runtime.HandlePointerEvent({PointerEventType::Cancel, 13, {60.0F, 20.0F}, PointerDeviceKind::Touch});
  REQUIRE(FindText(runtime.BuildFrame(), "Plain tooltip") == nullptr);
}

TEST_CASE("TestTooltipPreservesOrdinaryTouchActivation") {
  tooltip_clicks = 0;
  TestPlatform platform;
  Runtime runtime{TooltipApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 160.0F}});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({PointerEventType::Down, 9, {40.0F, 20.0F}, huxerui::PointerDeviceKind::Touch});
  runtime.HandlePointerEvent({PointerEventType::Up, 9, {40.0F, 20.0F}, huxerui::PointerDeviceKind::Touch});
  REQUIRE(tooltip_clicks == 1);
  REQUIRE(FindText(runtime.BuildFrame(), "Tooltip message") == nullptr);
}

TEST_CASE("TestTooltipCanHoverDisabledTarget") {
  TestPlatform platform;
  Runtime runtime{DisabledTooltipApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 160.0F}});
  runtime.BuildFrame();

  MovePointer(runtime, {40.0F, 20.0F});
  runtime.BuildFrame();
  platform.AdvanceTime(0.51);
  runtime.BuildFrame();
  REQUIRE(FindText(runtime.BuildFrame(), "Disabled explanation") != nullptr);
}

TEST_CASE("TestTooltipUpdatesCompatiblyAndDismissesWhenTargetUnmounts") {
  tooltip_target_visible = State<bool>{};
  tooltip_use_updated_message = State<bool>{};
  TestPlatform platform;
  Runtime runtime{TooltipLifecycleApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 160.0F}});
  runtime.BuildFrame();

  MovePointer(runtime, {40.0F, 20.0F});
  runtime.BuildFrame();
  platform.AdvanceTime(0.51);
  runtime.BuildFrame();
  REQUIRE(FindText(runtime.BuildFrame(), "Initial tooltip") != nullptr);

  tooltip_use_updated_message = true;
  runtime.BuildFrame();
  REQUIRE(FindText(runtime.BuildFrame(), "Updated tooltip") != nullptr);
  REQUIRE(FindText(runtime.BuildFrame(), "Initial tooltip") == nullptr);

  tooltip_target_visible = false;
  runtime.BuildFrame();
  REQUIRE(FindText(runtime.BuildFrame(), "Updated tooltip") == nullptr);
}

TEST_CASE("TestTooltipCompilesThemeStyleBeforeUpdatingItsExtension") {
  tooltip_style_mode = State<int>{};
  TestPlatform platform;
  Runtime runtime{DynamicTooltipStyleApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 160.0F}});
  runtime.BuildFrame();

  const auto* target = runtime.RootNode()->children[0].get();
  REQUIRE(target != nullptr);
  REQUIRE(target->extensions.size() == 1);
  const NodeExtension* extension = target->extensions[0].extension.get();

  MovePointer(runtime, {40.0F, 20.0F});
  runtime.BuildFrame();
  REQUIRE(FindPresentedRectWithColor(runtime.BuildFrame(), DynamicTooltipStyle(0).background).has_value());

  tooltip_style_mode = 1;
  REQUIRE(FindPresentedRectWithColor(runtime.BuildFrame(), DynamicTooltipStyle(1).background).has_value());
  target = runtime.RootNode()->children[0].get();
  REQUIRE(target->extensions[0].extension.get() == extension);

  tooltip_style_mode = 2;
  REQUIRE_THROWS_AS(runtime.BuildFrame(), std::invalid_argument);

  tooltip_style_mode = 1;
  REQUIRE(FindPresentedRectWithColor(runtime.BuildFrame(), DynamicTooltipStyle(1).background).has_value());
  target = runtime.RootNode()->children[0].get();
  REQUIRE(target->extensions[0].extension.get() == extension);
}

TEST_CASE("TestOnlyOneTooltipIsVisiblePerWindow") {
  TestPlatform platform;
  Runtime runtime{MultipleTooltipTargetsApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 160.0F}});
  runtime.BuildFrame();

  runtime.HandleKeyEvent({KeyEventType::Down, Key::Tab});
  runtime.BuildFrame();
  REQUIRE(FindText(runtime.BuildFrame(), "First tooltip") != nullptr);

  runtime.HandleKeyEvent({KeyEventType::Down, Key::Tab});
  runtime.BuildFrame();
  REQUIRE(FindText(runtime.BuildFrame(), "Second tooltip") != nullptr);
  REQUIRE(FindText(runtime.BuildFrame(), "First tooltip") == nullptr);
}

TEST_CASE("TestRuntimeDispatchesHoverToEveryExtensionOnDeepestNode") {
  first_hovered = false;
  second_hovered = false;
  TestPlatform platform;
  Runtime runtime{MultipleHoverApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 160.0F}});
  runtime.BuildFrame();

  MovePointer(runtime, {40.0F, 20.0F});
  REQUIRE(first_hovered);
  REQUIRE(second_hovered);

  MovePointer(runtime, {200.0F, 120.0F});
  REQUIRE_FALSE(first_hovered);
  REQUIRE_FALSE(second_hovered);
}

TEST_CASE("TestTooltipRejectsEmptyLiteralAndInvalidStyle") {
  REQUIRE_THROWS_AS(Tooltip(""), std::invalid_argument);

  TestPlatform platform;
  const auto invalid = []() -> View {
    TooltipStyle style = TooltipStyle::Default();
    style.maximum_width = 0.0F;
    ThemeDefinition definition;
    definition.Set(style);
    return Theme {std::move(definition), Text("Target").With(Tooltip("Message"))};
  };
  REQUIRE_THROWS_AS(Runtime(invalid, platform).BuildFrame(), std::invalid_argument);
}

} // namespace huxerui::test
