#include "runtime_test_support.h"

#include <limits>
#include <stdexcept>

namespace huxerui::test {

std::vector<PointerEvent> received_pointer_events;
State<bool> show_pointer_target;
int pointer_clicks = 0;
ScrollController drag_scroll;
ScrollController configured_drag_scroll;
ScrollPhysics configured_scroll_physics;
ScrollController horizontal_drag_scroll;
ScrollController nested_outer_scroll;
ScrollController nested_inner_scroll;
State<bool> include_apply_only_modifier;
State<bool> recompose_activation_button;
State<bool> alternate_scroll_bar_style;
State<bool> alternate_default_indication;
State<PointerCursorKind> dynamic_pointer_cursor;
State<bool> wide_hover_target;
int drag_item_clicks = 0;
int drag_item_cancels = 0;
int covered_pointer_clicks = 0;
int exceptional_pointer_cancels = 0;
int exceptional_pointer_ups = 0;
std::vector<std::pair<InteractionState, InteractionEvent>> recorded_interactions;
std::vector<std::pair<std::string, HoverEvent>> received_hover_events;
int stable_extension_updates = 0;

std::optional<detail::ScrollBarGeometry> FindScrollBarGeometry(const detail::MountedNode& node) {
  if (auto geometry = detail::ResolveScrollBarGeometry(node)) {
    return geometry;
  }
  for (const auto& child : node.children) {
    if (auto geometry = FindScrollBarGeometry(*child)) {
      return geometry;
    }
  }
  return std::nullopt;
}

struct RecordInteractions;

class RecordInteractionsExtension final : public NodeExtension {
public:
  RecordInteractionsExtension(ViewNode&, const RecordInteractions&) {}

  void Update(ViewNode&, const RecordInteractions&) {}

  void OnInteraction(ViewNode&, const InteractionState& state,
                     const std::optional<InteractionEvent>& event) override {
    if (event.has_value()) {
      recorded_interactions.emplace_back(state, *event);
    }
  }
};

struct RecordInteractions {
  using Extension = RecordInteractionsExtension;

  bool operator==(const RecordInteractions&) const = default;
};

struct CapturePointer;

class CapturePointerExtension final : public NodeExtension {
public:
  CapturePointerExtension(ViewNode&, const CapturePointer&) {}

  void Update(ViewNode&, const CapturePointer&) {}

  [[nodiscard]] bool HitTest(ViewNode& node, Point position) const override {
    return node.Bounds().Contains(position);
  }

  PointerResult OnPointer(ViewNode&, const PointerEvent& event) override {
    return event.type == PointerEventType::Down ? PointerResult::Capture : PointerResult::Handled;
  }
};

struct CapturePointer {
  using Extension = CapturePointerExtension;

  bool operator==(const CapturePointer&) const = default;
};

struct ObservePointer;

class ObservePointerExtension final : public NodeExtension {
public:
  ObservePointerExtension(ViewNode&, const ObservePointer&) {}

  void Update(ViewNode&, const ObservePointer&) {}

  [[nodiscard]] bool HitTest(ViewNode& node, Point position) const override {
    return node.Bounds().Contains(position);
  }

  PointerResult OnPointer(ViewNode&, const PointerEvent& event) override {
    return event.type == PointerEventType::Down ? PointerResult::Observe : PointerResult::Ignored;
  }
};

struct ObservePointer {
  using Extension = ObservePointerExtension;

  bool operator==(const ObservePointer&) const = default;
};

struct StableExtension;

class StableExtensionState final : public NodeExtension {
public:
  StableExtensionState(ViewNode&, const StableExtension&) {}

  void Update(ViewNode&, const StableExtension&) {
    ++stable_extension_updates;
  }
};

struct StableExtension {
  using Extension = StableExtensionState;

  bool operator==(const StableExtension&) const = default;
};

View PointerInputApp() {
  auto visible = UseState(true);
  show_pointer_target = visible;
  if (!visible.Get()) {
    return Stack{
        Text("Hidden").With(huxerui::Frame{100.0F, 40.0F}),
    };
  }

  return Stack{
      Button("Target")
          .With(huxerui::Frame{100.0F, 40.0F})
          .On<ViewEvents::Pointer>([](const PointerEvent& event) { received_pointer_events.push_back(event); })
          .OnClick([] { ++pointer_clicks; }),
  };
}

View PointerCursorApp() {
  return Column {
    Text("Hand").With(huxerui::Frame{100.0F, 40.0F}, PointerCursor(PointerCursorKind::Hand)),
    Text("Inherited").With(huxerui::Frame{100.0F, 40.0F}),
    Text("Default").With(huxerui::Frame{100.0F, 40.0F}, PointerCursor(PointerCursorKind::Default)),
    Text("Disabled")
        .With(huxerui::Frame{100.0F, 40.0F}, Enabled(false), PointerCursor(PointerCursorKind::NotAllowed)),
  }.With(huxerui::Frame{100.0F, 160.0F}, PointerCursor(PointerCursorKind::Crosshair));
}

View DynamicPointerCursorApp() {
  auto kind = UseState(PointerCursorKind::Hand);
  dynamic_pointer_cursor = kind;
  return Text("Dynamic").With(huxerui::Frame{100.0F, 40.0F}, PointerCursor(kind.Get()));
}

View HoverEventApp() {
  return Column {
    Spacer().With(huxerui::Frame{.height = 20.0F}),
    Text("Hover target")
        .With(huxerui::Frame{.width = 100.0F, .height = 40.0F}, Enabled(false))
        .On<ViewEvents::Hover>([](const HoverEvent& event) {
          received_hover_events.emplace_back("child", event);
        }),
  }.With(huxerui::Frame{.width = 100.0F, .height = 60.0F})
      .On<ViewEvents::Hover>([](const HoverEvent& event) {
        received_hover_events.emplace_back("parent", event);
      });
}

View DynamicHoverEventApp() {
  auto wide = UseState(true);
  wide_hover_target = wide;
  return Row {
    Text("Dynamic hover")
        .With(huxerui::Frame{.width = wide.Get() ? 100.0F : 10.0F, .height = 40.0F})
        .On<ViewEvents::Hover>([](const HoverEvent& event) {
          received_hover_events.emplace_back("dynamic", event);
        }),
  }.With(huxerui::Frame{100.0F, 40.0F});
}

View HoverOverlayPointerTargetApp() {
  return Stack{
      Button("Behind")
          .With(huxerui::Frame{100.0F, 40.0F})
          .OnClick([] { ++covered_pointer_clicks; }),
      Text("Hover overlay")
          .With(huxerui::Frame{100.0F, 40.0F})
          .On<ViewEvents::Hover>([](const HoverEvent& event) {
            received_hover_events.emplace_back("overlay", event);
          }),
  };
}

View ExceptionalPointerInputApp() {
  return Button("exceptional pointer")
      .With(huxerui::Frame{100.0F, 40.0F})
      .On<ViewEvents::Pointer>([](const PointerEvent& event) {
        if (event.type == PointerEventType::Down) {
          throw std::runtime_error("pointer input failed");
        }
        if (event.type == PointerEventType::Cancel) {
          ++exceptional_pointer_cancels;
        } else if (event.type == PointerEventType::Up) {
          ++exceptional_pointer_ups;
        }
      });
}

View ExtensionPointerTargetApp() {
  return Stack{
      Button("Behind")
          .With(huxerui::Frame{100.0F, 40.0F})
          .OnClick([] { ++covered_pointer_clicks; }),
      Text("Overlay").With(
          huxerui::Frame{100.0F, 40.0F},
          huxerui::Indication{
              .hover = huxerui::IndicationLayer{.fill = huxerui::Color::Rgb(0, 0, 0, 0.08F)},
          }
      ),
  };
}

View InteractionEventApp() {
  return Button("Interaction").With(huxerui::Frame{100.0F, 40.0F}, RecordInteractions{});
}

View CapturedInteractionEventApp() {
  return Text("Captured").With(huxerui::Frame{100.0F, 40.0F}, RecordInteractions{}, CapturePointer{});
}

View ObservedInteractionEventApp() {
  return Text("Observed").With(huxerui::Frame{100.0F, 40.0F}, RecordInteractions{}, ObservePointer{});
}

View StableActivationExtensionApp() {
  auto alternate = UseState(false);
  recompose_activation_button = alternate;
  return Column{
      Button("Stable").With(StableExtension{}),
      Text(alternate.Get() ? "alternate" : "initial"),
  };
}

View DragScrollApp() {
  auto scroll = UseScrollController();
  drag_scroll = scroll;
  return VirtualList(
             std::size_t{100},
             [](std::size_t index) {
               return Button(std::to_string(index))
                   .With(huxerui::Frame{100.0F, 40.0F})
                   .On<ViewEvents::Pointer>([](const PointerEvent& event) {
                     if (event.type == PointerEventType::Cancel) {
                       ++drag_item_cancels;
                     }
                   })
                   .OnClick([] { ++drag_item_clicks; })
                   .Key(index);
             }
  )
      .ItemExtent(40.0F)
      .Controller(scroll)
      .With(huxerui::ScrollBar{});
}

View ConfiguredDragScrollApp() {
  auto scroll = UseScrollController();
  configured_drag_scroll = scroll;
  return VirtualList(
             std::size_t{100},
             [](std::size_t index) {
               return Text(std::to_string(index)).With(huxerui::Frame{100.0F, 40.0F}).Key(index);
             }
  )
      .ItemExtent(40.0F)
      .Controller(scroll)
      .With(configured_scroll_physics);
}

View ThemedScrollBarApp() {
  ThemeDefinition definition;
  definition.Set(huxerui::ScrollBarStyle{
      .thickness = 9.0F,
      .minimum_thumb_extent = 30.0F,
      .margin = 4.0F,
      .corner_radius = 4.5F,
      .fade_in_duration = 0.1F,
      .fade_out_delay = 0.6F,
      .fade_out_duration = 0.2F,
      .track_color = Color::Transparent(),
      .thumb_color = Color::Rgb(200, 80, 60, 0.75F),
  });
  return Theme {std::move(definition), Scope(DragScrollApp)};
}

View FlatDarkScrollBarApp() {
  return huxerui::FlatDarkTheme {Scope(DragScrollApp)};
}

View DynamicScrollBarApp() {
  auto alternate = UseState(false);
  alternate_scroll_bar_style = alternate;
  ScrollBarStyle style = ScrollBarStyle::Default();
  style.thickness = alternate.Get() ? 11.0F : 7.0F;
  style.corner_radius = style.thickness * 0.5F;
  ThemeDefinition definition;
  definition.Set(style);
  return Theme {std::move(definition), Scope(DragScrollApp)};
}

View DynamicDefaultIndicationApp() {
  auto alternate = UseState(false);
  alternate_default_indication = alternate;
  const Color color = alternate.Get() ? Color::Rgb(20, 100, 180) : Color::Rgb(180, 30, 40);
  ButtonStyle style = ButtonStyle::Default();
  style.indication = Indication{
      .hover = IndicationLayer{
          .fill = color,
          .enter = SnapSpec{},
          .exit = SnapSpec{},
      },
  };
  ThemeDefinition definition;
  definition.Set(style);
  return Theme {
    std::move(definition),
    Button("Dynamic indication").With(huxerui::Frame{120.0F, 40.0F}).OnClick([] {}),
  };
}

View HorizontalDragScrollApp() {
  auto scroll = UseScrollController();
  horizontal_drag_scroll = scroll;
  return VirtualList(
             std::size_t{100},
             [](std::size_t index) { return Text(std::to_string(index)).With(huxerui::Frame{40.0F, 40.0F}).Key(index); }
  )
      .ScrollAxis(Axis::Horizontal)
      .ItemExtent(40.0F)
      .Controller(scroll)
      .With(huxerui::ScrollBar{});
}

View NestedDragScrollApp() {
  auto outer = UseScrollController();
  auto inner = UseScrollController();
  nested_outer_scroll = outer;
  nested_inner_scroll = inner;

  return ScrollView{
      Column{
          Text("Header").With(huxerui::Frame{100.0F, 40.0F}),
          ScrollView{
              Column{
                  Text("0").With(huxerui::Frame{100.0F, 20.0F}),
                  Text("1").With(huxerui::Frame{100.0F, 20.0F}),
                  Text("2").With(huxerui::Frame{100.0F, 20.0F}),
                  Text("3").With(huxerui::Frame{100.0F, 20.0F}),
                  Text("4").With(huxerui::Frame{100.0F, 20.0F}),
                  Text("5").With(huxerui::Frame{100.0F, 20.0F}),
                  Text("6").With(huxerui::Frame{100.0F, 20.0F}),
                  Text("7").With(huxerui::Frame{100.0F, 20.0F}),
                  Text("8").With(huxerui::Frame{100.0F, 20.0F}),
                  Text("9").With(huxerui::Frame{100.0F, 20.0F}),
              },
          }
              .Controller(inner)
              .With(huxerui::Frame{100.0F, 60.0F}, huxerui::ScrollBar{}),
          Text("Footer").With(huxerui::Frame{100.0F, 200.0F}),
      },
  }
      .Controller(outer);
}

View ShortScrollBarApp() {
  return ScrollView{
      Text("Short").With(huxerui::Frame{100.0F, 40.0F}),
  }
      .With(huxerui::ScrollBar{});
}

View ModifierReconciliationApp() {
  auto include_apply_only = UseState(false);
  include_apply_only_modifier = include_apply_only;
  if (include_apply_only.Get()) {
    return ScrollView{
        Text("Tall").With(huxerui::Frame{100.0F, 200.0F}),
    }
        .With(huxerui::Padding{4.0F}, huxerui::ScrollPhysics{}, huxerui::ScrollBar{});
  }
  return ScrollView{
      Text("Tall").With(huxerui::Frame{100.0F, 200.0F}),
  }
      .With(huxerui::ScrollBar{});
}

TEST_CASE("InteractionAggregatesPreserveOmittedDefaults") {
  const KeyEvent key{.key = Key::Tab};
  REQUIRE(key.text.empty());
  REQUIRE(key.modifiers == KeyModifiers{});
  REQUIRE_FALSE(key.repeat);

  const NodeExtension::FrameResult frame{.needs_frame = true};
  REQUIRE(frame.needs_frame);
  REQUIRE_FALSE(frame.wake_after.has_value());

  const Indication indication{.hover = IndicationLayer{.fill = Color::Black()}};
  REQUIRE(indication.geometry == IndicationGeometry{});
  REQUIRE_FALSE(indication.focus.has_value());
  REQUIRE_FALSE(indication.press.has_value());
  REQUIRE_FALSE(indication.ripple.has_value());
  REQUIRE_FALSE(indication.hover->border.has_value());
  REQUIRE_FALSE(indication.hover->corner_radii.has_value());

  const LayerOptions layer{.level = LayerLevel::Notification};
  REQUIRE_FALSE(layer.on_dismiss_request);
  REQUIRE_FALSE(layer.barrier_color.has_value());
}

TEST_CASE("TestBuiltInPointerEventsAndClickLifecycle") {
  received_pointer_events.clear();
  pointer_clicks = 0;

  TestPlatform platform;
  Runtime runtime{PointerInputApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  runtime.BuildFrame();

  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Up,
          1,
          {50.0F, 20.0F},
      }
  );
  REQUIRE(received_pointer_events.size() == 1);
  REQUIRE(received_pointer_events[0].type == PointerEventType::Up);
  REQUIRE(pointer_clicks == 0);

  received_pointer_events.clear();
  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Down,
          7,
          {50.0F, 20.0F},
      }
  );
  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Move,
          7,
          {150.0F, 80.0F},
      }
  );
  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Up,
          7,
          {150.0F, 80.0F},
      }
  );
  REQUIRE(received_pointer_events.size() == 3);
  REQUIRE(received_pointer_events[0].type == PointerEventType::Down);
  REQUIRE(received_pointer_events[1].type == PointerEventType::Move);
  REQUIRE(received_pointer_events[2].type == PointerEventType::Up);
  REQUIRE(received_pointer_events[2].pointer_id == 7);
  REQUIRE(pointer_clicks == 0);

  received_pointer_events.clear();
  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Down,
          8,
          {50.0F, 20.0F},
      }
  );
  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Cancel,
          8,
          {150.0F, 80.0F},
      }
  );
  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Up,
          8,
          {50.0F, 20.0F},
      }
  );
  REQUIRE(received_pointer_events.size() == 3);
  REQUIRE(received_pointer_events[1].type == PointerEventType::Cancel);
  REQUIRE(pointer_clicks == 0);

  received_pointer_events.clear();
  ClickAt(runtime, {50.0F, 20.0F}, 9);
  REQUIRE(received_pointer_events.size() == 2);
  REQUIRE(pointer_clicks == 1);

  received_pointer_events.clear();
  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Down,
          11,
          {50.0F, 20.0F},
      }
  );
  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Down,
          11,
          {50.0F, 20.0F},
      }
  );
  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Up,
          11,
          {50.0F, 20.0F},
      }
  );
  REQUIRE(received_pointer_events.size() == 4);
  REQUIRE(received_pointer_events[0].type == PointerEventType::Down);
  REQUIRE(received_pointer_events[1].type == PointerEventType::Cancel);
  REQUIRE(received_pointer_events[2].type == PointerEventType::Down);
  REQUIRE(received_pointer_events[3].type == PointerEventType::Up);
  REQUIRE(pointer_clicks == 2);

  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Down,
          10,
          {50.0F, 20.0F},
      }
  );
  show_pointer_target = false;
  runtime.BuildFrame();
  const std::size_t events_before_release = received_pointer_events.size();
  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Up,
          10,
          {50.0F, 20.0F},
      }
  );
  REQUIRE(received_pointer_events.size() == events_before_release);
  REQUIRE(pointer_clicks == 2);
}

View CursorOverlayPointerTargetApp() {
  return Stack{
      Button("Behind")
          .With(huxerui::Frame{100.0F, 40.0F})
          .OnClick([] { ++covered_pointer_clicks; }),
      Text("Cursor overlay")
          .With(huxerui::Frame{100.0F, 40.0F}, PointerCursor(PointerCursorKind::Crosshair)),
  };
}

TEST_CASE("HoverEventReportsNestedEnterMoveAndLeaveForMouseAndPenOnly") {
  received_hover_events.clear();
  TestPlatform platform;
  Runtime runtime{HoverEventApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 60.0F}});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({PointerEventType::Move, 30, {50.0F, 40.0F}});
  REQUIRE(received_hover_events.size() == 2);
  REQUIRE(received_hover_events[0].first == "parent");
  REQUIRE(received_hover_events[0].second == HoverEvent{
                                                  HoverEventType::Enter,
                                                  30,
                                                  PointerDeviceKind::Mouse,
                                                  {50.0F, 40.0F},
                                                  {50.0F, 40.0F},
                                              });
  REQUIRE(received_hover_events[1].first == "child");
  REQUIRE(received_hover_events[1].second == HoverEvent{
                                                  HoverEventType::Enter,
                                                  30,
                                                  PointerDeviceKind::Mouse,
                                                  {50.0F, 20.0F},
                                                  {50.0F, 40.0F},
                                              });

  runtime.HandlePointerEvent({PointerEventType::Move, 30, {60.0F, 45.0F}});
  REQUIRE(received_hover_events.size() == 4);
  REQUIRE(received_hover_events[2].first == "parent");
  REQUIRE(received_hover_events[2].second.type == HoverEventType::Move);
  REQUIRE(received_hover_events[3].first == "child");
  REQUIRE(received_hover_events[3].second.type == HoverEventType::Move);

  runtime.HandlePointerEvent({PointerEventType::Move, 30, {60.0F, 45.0F}});
  runtime.HandlePointerEvent({PointerEventType::Move, 31, {60.0F, 45.0F}, PointerDeviceKind::Touch});
  REQUIRE(received_hover_events.size() == 4);

  runtime.HandlePointerEvent({PointerEventType::Cancel, 30, {60.0F, 45.0F}});
  REQUIRE(received_hover_events.size() == 6);
  REQUIRE(received_hover_events[4].first == "child");
  REQUIRE(received_hover_events[4].second.type == HoverEventType::Leave);
  REQUIRE(received_hover_events[5].first == "parent");
  REQUIRE(received_hover_events[5].second.type == HoverEventType::Leave);

  received_hover_events.clear();
  runtime.HandlePointerEvent({PointerEventType::Move, 34, {50.0F, 40.0F}, PointerDeviceKind::Pen});
  REQUIRE(received_hover_events.size() == 2);
  REQUIRE(received_hover_events[0].second.device_kind == PointerDeviceKind::Pen);
  REQUIRE(received_hover_events[1].second.device_kind == PointerDeviceKind::Pen);
  runtime.HandlePointerEvent({PointerEventType::Cancel, 34, {50.0F, 40.0F}, PointerDeviceKind::Pen});
}

TEST_CASE("HoverEventTracksGeometryChangesUnderAStationaryPointer") {
  received_hover_events.clear();
  TestPlatform platform;
  Runtime runtime{DynamicHoverEventApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 40.0F}});
  runtime.BuildFrame();
  runtime.HandlePointerEvent({PointerEventType::Move, 32, {50.0F, 20.0F}});
  REQUIRE(received_hover_events.size() == 1);
  REQUIRE(received_hover_events.back().second.type == HoverEventType::Enter);

  received_hover_events.clear();
  wide_hover_target = false;
  runtime.BuildFrame();
  REQUIRE(received_hover_events.size() == 1);
  REQUIRE(received_hover_events.back().second.type == HoverEventType::Leave);

  received_hover_events.clear();
  wide_hover_target = true;
  runtime.BuildFrame();
  REQUIRE(received_hover_events.size() == 1);
  REQUIRE(received_hover_events.back().second.type == HoverEventType::Enter);
}

TEST_CASE("HoverEventDoesNotTurnAVisualOverlayIntoAnInputTarget") {
  covered_pointer_clicks = 0;
  received_hover_events.clear();
  TestPlatform platform;
  Runtime runtime{HoverOverlayPointerTargetApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 40.0F}});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({PointerEventType::Move, 33, {50.0F, 20.0F}});
  REQUIRE(received_hover_events.size() == 1);
  REQUIRE(received_hover_events.back().second.type == HoverEventType::Enter);
  ClickAt(runtime, {50.0F, 20.0F}, 33);
  REQUIRE(covered_pointer_clicks == 1);
}

TEST_CASE("PointerCursorResolvesTheDeepestExplicitDeclaration") {
  TestPlatform platform;
  Runtime runtime{PointerCursorApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 200.0F}});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({PointerEventType::Move, 1, {50.0F, 20.0F}});
  REQUIRE(platform.pointer_cursors.back() == PointerCursorKind::Hand);

  runtime.HandlePointerEvent({PointerEventType::Move, 1, {50.0F, 60.0F}});
  REQUIRE(platform.pointer_cursors.back() == PointerCursorKind::Crosshair);

  runtime.HandlePointerEvent({PointerEventType::Move, 1, {50.0F, 100.0F}});
  REQUIRE(platform.pointer_cursors.back() == PointerCursorKind::Default);

  runtime.HandlePointerEvent({PointerEventType::Move, 1, {50.0F, 140.0F}});
  REQUIRE(platform.pointer_cursors.back() == PointerCursorKind::NotAllowed);

  runtime.HandlePointerEvent({
      .type = PointerEventType::Move,
      .pointer_id = 2,
      .position = {50.0F, 20.0F},
      .device_kind = PointerDeviceKind::Touch,
  });
  REQUIRE(platform.pointer_cursors.back() == PointerCursorKind::NotAllowed);

  runtime.HandlePointerEvent({PointerEventType::Cancel, 1, {50.0F, 140.0F}});
  REQUIRE(platform.pointer_cursors.back() == PointerCursorKind::Default);
}

TEST_CASE("PointerCursorTracksRecompositionUnderAStationaryPointer") {
  TestPlatform platform;
  Runtime runtime{DynamicPointerCursorApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 40.0F}});
  runtime.BuildFrame();
  runtime.HandlePointerEvent({PointerEventType::Move, 3, {50.0F, 20.0F}});
  REQUIRE(platform.pointer_cursors.back() == PointerCursorKind::Hand);

  dynamic_pointer_cursor = PointerCursorKind::Grabbing;
  runtime.BuildFrame();
  REQUIRE(platform.pointer_cursors.back() == PointerCursorKind::Grabbing);

  const std::size_t update_count = platform.pointer_cursors.size();
  dynamic_pointer_cursor = PointerCursorKind::Grabbing;
  runtime.BuildFrame();
  REQUIRE(platform.pointer_cursors.size() == update_count);
}

TEST_CASE("PointerCursorDoesNotTurnAVisualOverlayIntoAnInputTarget") {
  covered_pointer_clicks = 0;
  TestPlatform platform;
  Runtime runtime{CursorOverlayPointerTargetApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 40.0F}});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({PointerEventType::Move, 4, {50.0F, 20.0F}});
  REQUIRE(platform.pointer_cursors.back() == PointerCursorKind::Crosshair);
  ClickAt(runtime, {50.0F, 20.0F}, 5);
  REQUIRE(covered_pointer_clicks == 1);
}

TEST_CASE("TestNodeExtensionHitOwnsTopmostPointerBranch") {
  covered_pointer_clicks = 0;

  TestPlatform platform;
  Runtime runtime{ExtensionPointerTargetApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 40.0F}});
  runtime.BuildFrame();

  ClickAt(runtime, {50.0F, 20.0F}, 122);
  REQUIRE(covered_pointer_clicks == 0);
}

TEST_CASE("TestPointerExceptionQuarantinesThePhysicalSequence") {
  exceptional_pointer_cancels = 0;
  exceptional_pointer_ups = 0;

  TestPlatform platform;
  Runtime runtime{ExceptionalPointerInputApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 40.0F}});
  runtime.BuildFrame();

  REQUIRE_THROWS_AS(runtime.HandlePointerEvent({PointerEventType::Down, 123, {50.0F, 20.0F}}), std::runtime_error);
  REQUIRE(exceptional_pointer_cancels == 1);

  runtime.HandlePointerEvent({PointerEventType::Up, 123, {50.0F, 20.0F}});
  REQUIRE(exceptional_pointer_ups == 0);
}

TEST_CASE("TestRuntimePublishesOrderedInteractionEvents") {
  recorded_interactions.clear();
  TestPlatform platform;
  Runtime runtime{InteractionEventApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 40.0F}});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({PointerEventType::Down, 124, {25.0F, 20.0F}});
  runtime.HandlePointerEvent({PointerEventType::Up, 124, {30.0F, 20.0F}});
  REQUIRE(recorded_interactions.size() == 2);
  REQUIRE(recorded_interactions[0].first.pressed);
  REQUIRE_FALSE(recorded_interactions[1].first.pressed);
  REQUIRE(recorded_interactions[0].second.type == InteractionEvent::Type::Press);
  REQUIRE(recorded_interactions[1].second.type == InteractionEvent::Type::Release);
  REQUIRE(recorded_interactions[0].second.source == InteractionEvent::Source::Pointer);
  REQUIRE(recorded_interactions[0].second.press_id == recorded_interactions[1].second.press_id);
  REQUIRE(recorded_interactions[0].second.press_id != 0);
  REQUIRE(recorded_interactions[0].second.position == Point{25.0F, 20.0F});

  recorded_interactions.clear();
  runtime.HandlePointerEvent({PointerEventType::Down, 125, {25.0F, 20.0F}});
  runtime.HandlePointerEvent({PointerEventType::Cancel, 125, {120.0F, 20.0F}});
  REQUIRE(recorded_interactions.size() == 2);
  REQUIRE(recorded_interactions[1].second.type == InteractionEvent::Type::Cancel);
  REQUIRE(recorded_interactions[0].second.press_id == recorded_interactions[1].second.press_id);
}

TEST_CASE("TestMultiplePointersRetainPressedStateUntilTheLastRelease") {
  recorded_interactions.clear();
  TestPlatform platform;
  Runtime runtime{InteractionEventApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 40.0F}});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({PointerEventType::Down, 129, {20.0F, 20.0F}});
  runtime.HandlePointerEvent({PointerEventType::Down, 130, {40.0F, 20.0F}});
  runtime.HandlePointerEvent({PointerEventType::Up, 129, {20.0F, 20.0F}});
  runtime.HandlePointerEvent({PointerEventType::Up, 130, {40.0F, 20.0F}});

  REQUIRE(recorded_interactions.size() == 4);
  REQUIRE(recorded_interactions[0].first.pressed);
  REQUIRE(recorded_interactions[1].first.pressed);
  REQUIRE(recorded_interactions[2].first.pressed);
  REQUIRE_FALSE(recorded_interactions[3].first.pressed);
  REQUIRE(recorded_interactions[0].second.press_id != recorded_interactions[1].second.press_id);
  REQUIRE(recorded_interactions[0].second.press_id == recorded_interactions[2].second.press_id);
  REQUIRE(recorded_interactions[1].second.press_id == recorded_interactions[3].second.press_id);
}

TEST_CASE("TestCapturedPointerPublishesInteractionRelease") {
  recorded_interactions.clear();
  TestPlatform platform;
  Runtime runtime{CapturedInteractionEventApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 40.0F}});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({PointerEventType::Down, 126, {25.0F, 20.0F}});
  runtime.HandlePointerEvent({PointerEventType::Up, 126, {30.0F, 20.0F}});
  REQUIRE(recorded_interactions.size() == 2);
  REQUIRE(recorded_interactions[0].second.type == InteractionEvent::Type::Press);
  REQUIRE(recorded_interactions[1].second.type == InteractionEvent::Type::Release);
  REQUIRE_FALSE(recorded_interactions[1].first.pressed);
}

TEST_CASE("TestCapturedPointerPublishesInteractionCancel") {
  recorded_interactions.clear();
  TestPlatform platform;
  Runtime runtime{CapturedInteractionEventApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 40.0F}});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({PointerEventType::Down, 127, {25.0F, 20.0F}});
  runtime.HandlePointerEvent({PointerEventType::Cancel, 127, {30.0F, 20.0F}});
  REQUIRE(recorded_interactions.size() == 2);
  REQUIRE(recorded_interactions[0].second.type == InteractionEvent::Type::Press);
  REQUIRE(recorded_interactions[1].second.type == InteractionEvent::Type::Cancel);
  REQUIRE(recorded_interactions[0].second.press_id == recorded_interactions[1].second.press_id);
  REQUIRE_FALSE(recorded_interactions[1].first.pressed);
}

TEST_CASE("TestObservedPointerPublishesOneInteractionLifecycle") {
  recorded_interactions.clear();
  TestPlatform platform;
  Runtime runtime{ObservedInteractionEventApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 40.0F}});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({PointerEventType::Down, 128, {25.0F, 20.0F}});
  runtime.HandlePointerEvent({PointerEventType::Up, 128, {30.0F, 20.0F}});
  REQUIRE(recorded_interactions.size() == 2);
  REQUIRE(recorded_interactions[0].second.type == InteractionEvent::Type::Press);
  REQUIRE(recorded_interactions[1].second.type == InteractionEvent::Type::Release);
  REQUIRE(recorded_interactions[0].second.press_id == recorded_interactions[1].second.press_id);
}

TEST_CASE("TestActivationImplementationDoesNotInvalidateEqualExtensions") {
  stable_extension_updates = 0;
  TestPlatform platform;
  Runtime runtime{StableActivationExtensionApp, platform};
  runtime.SetWindowMetrics({.viewport = {120.0F, 80.0F}});
  runtime.BuildFrame();

  recompose_activation_button = true;
  runtime.BuildFrame();

  REQUIRE(stable_extension_updates == 0);
}

TEST_CASE("TestConsecutivePointerClicksDoNotSuppressActivation") {
  received_pointer_events.clear();
  pointer_clicks = 0;

  TestPlatform platform;
  Runtime runtime{PointerInputApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 100.0F}});
  runtime.BuildFrame();

  ClickAt(runtime, {50.0F, 20.0F}, 12);
  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Down,
          12,
          {50.0F, 20.0F},
          PointerDeviceKind::Mouse,
      }
  );
  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Up,
          12,
          {50.0F, 20.0F},
          PointerDeviceKind::Mouse,
      }
  );

  REQUIRE(pointer_clicks == 2);
}

TEST_CASE("TestPointerDragScrollingAndClickArbitration") {
  drag_item_clicks = 0;
  drag_item_cancels = 0;

  TestPlatform platform;
  Runtime runtime{DragScrollApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  runtime.BuildFrame();

  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Down,
          20,
          {50.0F, 20.0F},
      }
  );
  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Move,
          20,
          {50.0F, 16.0F},
      }
  );
  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Up,
          20,
          {50.0F, 16.0F},
      }
  );
  REQUIRE(drag_item_clicks == 1);
  REQUIRE(drag_item_cancels == 0);
  REQUIRE(drag_scroll.Offset() == 0.0F);

  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Down,
          21,
          {50.0F, 20.0F},
      }
  );
  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Move,
          21,
          {50.0F, 30.0F},
      }
  );
  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Up,
          21,
          {50.0F, 30.0F},
      }
  );
  REQUIRE(drag_item_clicks == 2);
  REQUIRE(drag_item_cancels == 0);
  REQUIRE(drag_scroll.Offset() == 0.0F);

  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Down,
          22,
          {50.0F, 20.0F},
      }
  );
  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Move,
          22,
          {50.0F, 10.0F},
      }
  );
  REQUIRE(drag_scroll.Offset() == 10.0F);
  REQUIRE(drag_item_cancels == 1);
  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Move,
          22,
          {50.0F, 0.0F},
      }
  );
  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Up,
          22,
          {50.0F, 0.0F},
      }
  );
  REQUIRE(drag_scroll.Offset() == 20.0F);
  REQUIRE(drag_item_clicks == 2);

  runtime.BuildFrame();
  REQUIRE(runtime.RootNode()->scroll_state->offset_y == 20.0F);
}

TEST_CASE("TestTouchDragContinuesWithMomentumAndCancelsOnPress") {
  TestPlatform platform;
  Runtime runtime{DragScrollApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  runtime.BuildFrame();

  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Down,
          60,
          {50.0F, 30.0F},
          PointerDeviceKind::Touch,
      }
  );
  platform.AdvanceTime(0.016);
  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Move,
          60,
          {50.0F, 20.0F},
          PointerDeviceKind::Touch,
      }
  );
  platform.AdvanceTime(0.016);
  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Move,
          60,
          {50.0F, 10.0F},
          PointerDeviceKind::Touch,
      }
  );
  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Up,
          60,
          {50.0F, 10.0F},
          PointerDeviceKind::Touch,
      }
  );

  const float released_offset = drag_scroll.Offset();
  runtime.BuildFrame();
  platform.AdvanceTime(0.016);
  runtime.BuildFrame();
  REQUIRE(drag_scroll.Offset() > released_offset);

  const float moving_offset = drag_scroll.Offset();
  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Down,
          61,
          {50.0F, 30.0F},
          PointerDeviceKind::Touch,
      }
  );
  platform.AdvanceTime(0.05);
  runtime.BuildFrame();
  REQUIRE(drag_scroll.Offset() == moving_offset);
  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Cancel,
          61,
          {50.0F, 30.0F},
          PointerDeviceKind::Touch,
      }
  );
}

TEST_CASE("TestDefaultTouchMomentumCarriesReleaseVelocity") {
  TestPlatform platform;
  Runtime runtime{DragScrollApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  runtime.BuildFrame();

  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Down,
          67,
          {50.0F, 30.0F},
          PointerDeviceKind::Touch,
      }
  );
  platform.AdvanceTime(0.016);
  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Move,
          67,
          {50.0F, 20.0F},
          PointerDeviceKind::Touch,
      }
  );
  platform.AdvanceTime(0.016);
  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Move,
          67,
          {50.0F, 10.0F},
          PointerDeviceKind::Touch,
      }
  );
  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Up,
          67,
          {50.0F, 10.0F},
          PointerDeviceKind::Touch,
      }
  );

  const float released_offset = drag_scroll.Offset();
  for (int frame = 0; frame < 100; ++frame) {
    platform.AdvanceTime(0.016);
    runtime.BuildFrame();
  }
  REQUIRE(drag_scroll.Offset() > released_offset + 190.0F);
  REQUIRE(drag_scroll.Offset() < released_offset + 220.0F);
}

TEST_CASE("TestTouchMomentumUsesRecentMovementInsteadOfOnlyTheFinalDelta") {
  TestPlatform platform;
  Runtime runtime{DragScrollApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({PointerEventType::Down, 68, {50.0F, 95.0F}, PointerDeviceKind::Touch});
  for (float y : {82.0F, 69.0F, 56.0F, 43.0F, 30.0F, 17.0F}) {
    platform.AdvanceTime(0.016);
    runtime.HandlePointerEvent({PointerEventType::Move, 68, {50.0F, y}, PointerDeviceKind::Touch});
  }
  platform.AdvanceTime(0.016);
  runtime.HandlePointerEvent({PointerEventType::Move, 68, {50.0F, 16.0F}, PointerDeviceKind::Touch});
  runtime.HandlePointerEvent({PointerEventType::Up, 68, {50.0F, 16.0F}, PointerDeviceKind::Touch});

  const float released_offset = drag_scroll.Offset();
  for (int frame = 0; frame < 100; ++frame) {
    platform.AdvanceTime(0.016);
    runtime.BuildFrame();
  }
  REQUIRE(drag_scroll.Offset() > released_offset + 130.0F);
}

TEST_CASE("TestTouchMomentumTracksReleaseAcceleration") {
  TestPlatform platform;
  Runtime runtime{DragScrollApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({PointerEventType::Down, 71, {50.0F, 95.0F}, PointerDeviceKind::Touch});
  for (float y : {92.0F, 87.0F, 79.0F, 68.0F, 52.0F, 30.0F, 3.0F}) {
    platform.AdvanceTime(0.016);
    runtime.HandlePointerEvent({PointerEventType::Move, 71, {50.0F, y}, PointerDeviceKind::Touch});
  }
  runtime.HandlePointerEvent({PointerEventType::Up, 71, {50.0F, 3.0F}, PointerDeviceKind::Touch});

  const float released_offset = drag_scroll.Offset();
  for (int frame = 0; frame < 100; ++frame) {
    platform.AdvanceTime(0.016);
    runtime.BuildFrame();
  }
  REQUIRE(drag_scroll.Offset() > released_offset + 500.0F);
}

TEST_CASE("TestTouchMomentumDoesNotReverseAfterMonotonicDeceleration") {
  TestPlatform platform;
  Runtime runtime{DragScrollApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({PointerEventType::Down, 72, {50.0F, 95.0F}, PointerDeviceKind::Touch});
  for (float y : {85.0F, 75.0F, 65.0F, 55.0F, 54.0F, 53.0F, 52.0F}) {
    platform.AdvanceTime(0.016);
    runtime.HandlePointerEvent({PointerEventType::Move, 72, {50.0F, y}, PointerDeviceKind::Touch});
  }
  runtime.HandlePointerEvent({PointerEventType::Up, 72, {50.0F, 52.0F}, PointerDeviceKind::Touch});

  const float released_offset = drag_scroll.Offset();
  for (int frame = 0; frame < 100; ++frame) {
    platform.AdvanceTime(0.016);
    runtime.BuildFrame();
  }
  REQUIRE(drag_scroll.Offset() >= released_offset);
}

TEST_CASE("TestTouchMomentumDoesNotStartAfterReleasePause") {
  TestPlatform platform;
  Runtime runtime{DragScrollApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({PointerEventType::Down, 69, {50.0F, 40.0F}, PointerDeviceKind::Touch});
  platform.AdvanceTime(0.016);
  runtime.HandlePointerEvent({PointerEventType::Move, 69, {50.0F, 10.0F}, PointerDeviceKind::Touch});
  platform.AdvanceTime(0.11);
  runtime.HandlePointerEvent({PointerEventType::Up, 69, {50.0F, 10.0F}, PointerDeviceKind::Touch});

  const float released_offset = drag_scroll.Offset();
  platform.AdvanceTime(0.05);
  runtime.BuildFrame();
  REQUIRE(drag_scroll.Offset() == released_offset);
}

TEST_CASE("TestTouchMomentumIgnoresStaleMovementSamples") {
  TestPlatform platform;
  Runtime runtime{DragScrollApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  runtime.BuildFrame();

  runtime.HandlePointerEvent({PointerEventType::Down, 70, {50.0F, 40.0F}, PointerDeviceKind::Touch});
  platform.AdvanceTime(0.2);
  runtime.HandlePointerEvent({PointerEventType::Move, 70, {50.0F, 10.0F}, PointerDeviceKind::Touch});
  runtime.HandlePointerEvent({PointerEventType::Up, 70, {50.0F, 10.0F}, PointerDeviceKind::Touch});

  const float released_offset = drag_scroll.Offset();
  platform.AdvanceTime(0.05);
  runtime.BuildFrame();
  REQUIRE(drag_scroll.Offset() == released_offset);
}

TEST_CASE("TestMomentumStopsAtBoundaryAndDoesNotStartForMouse") {
  TestPlatform platform;
  Runtime touch{DragScrollApp, platform};
  touch.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  touch.BuildFrame();
  REQUIRE(drag_scroll.ScrollTo(drag_scroll.MaxOffset() - 5.0F));
  touch.BuildFrame();

  touch.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Down,
          62,
          {50.0F, 30.0F},
          PointerDeviceKind::Touch,
      }
  );
  platform.AdvanceTime(0.016);
  touch.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Move,
          62,
          {50.0F, 10.0F},
          PointerDeviceKind::Touch,
      }
  );
  touch.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Up,
          62,
          {50.0F, 10.0F},
          PointerDeviceKind::Touch,
      }
  );
  touch.BuildFrame();
  platform.AdvanceTime(0.05);
  touch.BuildFrame();
  REQUIRE(drag_scroll.Offset() == drag_scroll.MaxOffset());

  TestPlatform mouse_platform;
  Runtime mouse{DragScrollApp, mouse_platform};
  mouse.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  mouse.BuildFrame();
  mouse.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Down,
          63,
          {50.0F, 30.0F},
          PointerDeviceKind::Mouse,
      }
  );
  mouse_platform.AdvanceTime(0.016);
  mouse.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Move,
          63,
          {50.0F, 10.0F},
          PointerDeviceKind::Mouse,
      }
  );
  mouse.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Up,
          63,
          {50.0F, 10.0F},
          PointerDeviceKind::Mouse,
      }
  );
  const float mouse_offset = drag_scroll.Offset();
  mouse.BuildFrame();
  mouse_platform.AdvanceTime(0.05);
  mouse.BuildFrame();
  REQUIRE(drag_scroll.Offset() == mouse_offset);

  TestPlatform horizontal_platform;
  Runtime horizontal{HorizontalDragScrollApp, horizontal_platform};
  horizontal.SetWindowMetrics({.viewport = {100.0F, 40.0F}});
  horizontal.BuildFrame();
  horizontal.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Down,
          64,
          {30.0F, 20.0F},
          PointerDeviceKind::Touch,
      }
  );
  horizontal_platform.AdvanceTime(0.016);
  horizontal.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Move,
          64,
          {10.0F, 20.0F},
          PointerDeviceKind::Touch,
      }
  );
  horizontal.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Up,
          64,
          {10.0F, 20.0F},
          PointerDeviceKind::Touch,
      }
  );
  const float horizontal_offset = horizontal_drag_scroll.Offset();
  horizontal.BuildFrame();
  horizontal_platform.AdvanceTime(0.016);
  horizontal.BuildFrame();
  REQUIRE(horizontal_drag_scroll.Offset() > horizontal_offset);
}

TEST_CASE("TestScrollPhysicsConfiguresAndValidatesMomentum") {
  configured_scroll_physics = ScrollPhysics{
      .fling_enabled = false,
  };
  TestPlatform platform;
  Runtime runtime{ConfiguredDragScrollApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  runtime.BuildFrame();
  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Down,
          65,
          {50.0F, 30.0F},
          PointerDeviceKind::Touch,
      }
  );
  platform.AdvanceTime(0.016);
  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Move,
          65,
          {50.0F, 10.0F},
          PointerDeviceKind::Touch,
      }
  );
  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Up,
          65,
          {50.0F, 10.0F},
          PointerDeviceKind::Touch,
      }
  );
  const float released_offset = configured_drag_scroll.Offset();
  runtime.BuildFrame();
  platform.AdvanceTime(0.05);
  runtime.BuildFrame();
  REQUIRE(configured_drag_scroll.Offset() == released_offset);

  TestPlatform invalid_platform;
  Runtime invalid{
      +[]() -> View {
        return VirtualList(std::size_t{1}, [](std::size_t) { return Text("Item"); })
            .With(ScrollPhysics{
                .minimum_fling_velocity = 100.0F,
                .maximum_fling_velocity = 50.0F,
            });
      },
      invalid_platform,
  };
  invalid.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  REQUIRE_THROWS_AS(invalid.BuildFrame(), std::invalid_argument);
}

TEST_CASE("TestHorizontalPointerDragUsesDominantAxis") {
  TestPlatform platform;
  Runtime runtime{HorizontalDragScrollApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 40.0F}});
  runtime.BuildFrame();

  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Down,
          30,
          {50.0F, 20.0F},
      }
  );
  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Move,
          30,
          {50.0F, 5.0F},
      }
  );
  REQUIRE(horizontal_drag_scroll.Offset() == 0.0F);

  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Move,
          30,
          {20.0F, 18.0F},
      }
  );
  REQUIRE(horizontal_drag_scroll.Offset() == 30.0F);
  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Up,
          30,
          {20.0F, 18.0F},
      }
  );

  runtime.BuildFrame();
  REQUIRE(runtime.RootNode()->scroll_state->offset_x == 30.0F);
}

TEST_CASE("TestNestedPointerDragPassesRemainingDelta") {
  TestPlatform platform;
  Runtime runtime{NestedDragScrollApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  runtime.BuildFrame();

  REQUIRE(nested_inner_scroll.ScrollTo(130.0F));
  runtime.BuildFrame();
  REQUIRE(nested_inner_scroll.MaxOffset() == 140.0F);
  REQUIRE(nested_outer_scroll.Offset() == 0.0F);

  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Down,
          40,
          {50.0F, 50.0F},
      }
  );
  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Move,
          40,
          {50.0F, 20.0F},
      }
  );
  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Up,
          40,
          {50.0F, 20.0F},
      }
  );

  REQUIRE(nested_inner_scroll.Offset() == 140.0F);
  REQUIRE(nested_outer_scroll.Offset() == 20.0F);
  runtime.BuildFrame();
  REQUIRE(runtime.RootNode()->scroll_state->offset_y == 20.0F);
}

TEST_CASE("TestNestedScrollInputPassesRemainingDelta") {
  TestPlatform platform;
  Runtime runtime{NestedDragScrollApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  runtime.BuildFrame();

  REQUIRE(nested_inner_scroll.ScrollTo(130.0F));
  runtime.BuildFrame();
  runtime.HandleScrollInput(
      ScrollInputEvent{
          {50.0F, 50.0F},
          0.0F,
          30.0F,
      }
  );
  REQUIRE(nested_inner_scroll.Offset() == 140.0F);
  REQUIRE(nested_outer_scroll.Offset() == 20.0F);

  REQUIRE(nested_inner_scroll.ScrollTo(10.0F));
  REQUIRE(nested_outer_scroll.ScrollTo(20.0F));
  runtime.BuildFrame();
  runtime.HandleScrollInput(
      ScrollInputEvent{
          {50.0F, 50.0F},
          0.0F,
          -40.0F,
      }
  );
  REQUIRE(nested_inner_scroll.Offset() == 0.0F);
  REQUIRE(nested_outer_scroll.Offset() == 0.0F);
}

TEST_CASE("TestNestedMomentumPassesRemainingVelocity") {
  TestPlatform platform;
  Runtime runtime{NestedDragScrollApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  runtime.BuildFrame();

  REQUIRE(nested_inner_scroll.ScrollTo(120.0F));
  runtime.BuildFrame();
  REQUIRE(nested_inner_scroll.MaxOffset() == 140.0F);
  REQUIRE(nested_outer_scroll.Offset() == 0.0F);

  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Down,
          66,
          {50.0F, 70.0F},
          PointerDeviceKind::Touch,
      }
  );
  platform.AdvanceTime(0.016);
  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Move,
          66,
          {50.0F, 60.0F},
          PointerDeviceKind::Touch,
      }
  );
  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Up,
          66,
          {50.0F, 60.0F},
          PointerDeviceKind::Touch,
      }
  );
  REQUIRE(nested_inner_scroll.Offset() == 130.0F);

  runtime.BuildFrame();
  platform.AdvanceTime(0.016);
  runtime.BuildFrame();
  platform.AdvanceTime(0.016);
  runtime.BuildFrame();
  platform.AdvanceTime(0.016);
  runtime.BuildFrame();

  REQUIRE(nested_inner_scroll.Offset() == nested_inner_scroll.MaxOffset());
  REQUIRE(nested_outer_scroll.Offset() > 0.0F);
}

TEST_CASE("TestApplyOnlyModifiersDoNotReplaceNodeExtensions") {
  TestPlatform platform;
  Runtime runtime{ModifierReconciliationApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  runtime.BuildFrame();

  const auto* root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->extensions.size() == 1);
  REQUIRE(root->extensions.front().extension != nullptr);
  const NodeExtension* extension = root->extensions.front().extension.get();

  include_apply_only_modifier = true;
  runtime.BuildFrame();

  root = runtime.RootNode();
  REQUIRE(root != nullptr);
  REQUIRE(root->properties.padding.top == 4.0F);
  REQUIRE(root->layout_values.contains(typeid(ScrollPhysics)));
  REQUIRE(root->extensions.size() == 1);
  REQUIRE(root->extensions.front().extension.get() == extension);
}

TEST_CASE("TestScrollBarGeometryRenderingAndDragging") {
  drag_item_clicks = 0;
  drag_item_cancels = 0;

  TestPlatform platform;
  Runtime vertical{DragScrollApp, platform};
  vertical.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  const FlattenedScene& vertical_display = vertical.BuildFrame();

  const auto vertical_bar = huxerui::detail::ResolveScrollBarGeometry(*vertical.RootNode());
  REQUIRE(vertical_bar.has_value());
  REQUIRE(vertical_bar->axis == Axis::Vertical);
  REQUIRE(vertical_bar->track.x == 91.0F);
  REQUIRE(vertical_bar->track.y == 3.0F);
  REQUIRE(vertical_bar->track.width == 6.0F);
  REQUIRE(vertical_bar->track.height == 94.0F);
  REQUIRE(vertical_bar->thumb.x == 91.0F);
  REQUIRE(vertical_bar->thumb.y == 3.0F);
  REQUIRE(vertical_bar->thumb.height == 24.0F);
  REQUIRE(ContainsRect(vertical_display, vertical_bar->thumb));

  vertical.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Down,
          49,
          {94.0F, 80.0F},
      }
  );
  vertical.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Up,
          49,
          {94.0F, 80.0F},
      }
  );
  REQUIRE(drag_scroll.Offset() == 0.0F);
  REQUIRE(drag_item_clicks == 0);

  vertical.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Down,
          50,
          {94.0F, 10.0F},
      }
  );
  vertical.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Move,
          50,
          {94.0F, 40.0F},
      }
  );
  vertical.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Up,
          50,
          {94.0F, 40.0F},
      }
  );
  REQUIRE(std::abs(drag_scroll.Offset() - 1671.4286F) < 0.01F);
  REQUIRE(drag_item_clicks == 0);
  REQUIRE(drag_item_cancels == 0);
  vertical.BuildFrame();
  const auto moved_vertical_bar = huxerui::detail::ResolveScrollBarGeometry(*vertical.RootNode());
  REQUIRE(moved_vertical_bar.has_value());
  REQUIRE(std::abs(moved_vertical_bar->thumb.y - 33.0F) < 0.01F);

  Runtime horizontal{HorizontalDragScrollApp, platform};
  horizontal.SetWindowMetrics({.viewport = {100.0F, 40.0F}});
  const FlattenedScene& horizontal_display = horizontal.BuildFrame();
  const auto horizontal_bar = huxerui::detail::ResolveScrollBarGeometry(*horizontal.RootNode());
  REQUIRE(horizontal_bar.has_value());
  REQUIRE(horizontal_bar->axis == Axis::Horizontal);
  REQUIRE(horizontal_bar->track.x == 3.0F);
  REQUIRE(horizontal_bar->track.y == 31.0F);
  REQUIRE(horizontal_bar->track.width == 94.0F);
  REQUIRE(horizontal_bar->track.height == 6.0F);
  REQUIRE(horizontal_bar->thumb.width == 24.0F);
  REQUIRE(ContainsRect(horizontal_display, horizontal_bar->thumb));

  horizontal.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Down,
          51,
          {10.0F, 34.0F},
      }
  );
  horizontal.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Move,
          51,
          {40.0F, 34.0F},
      }
  );
  horizontal.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Up,
          51,
          {40.0F, 34.0F},
      }
  );
  REQUIRE(std::abs(horizontal_drag_scroll.Offset() - 1671.4286F) < 0.01F);

  Runtime short_content{ShortScrollBarApp, platform};
  short_content.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  short_content.BuildFrame();
  REQUIRE(!huxerui::detail::ResolveScrollBarGeometry(*short_content.RootNode()));

  TestPlatform invalid_style_platform;
  Runtime invalid_style{
      +[]() -> View {
        return VirtualList(std::size_t{1}, [](std::size_t) { return Text("Item"); })
            .With(huxerui::ScrollBar{
                huxerui::ScrollBarStyle{
                    .thickness = 0.0F,
                },
            });
      },
      invalid_style_platform,
  };
  invalid_style.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  REQUIRE_THROWS_AS(invalid_style.BuildFrame(), std::invalid_argument);

  Runtime themed{ThemedScrollBarApp, platform};
  themed.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  themed.BuildFrame();
  const auto* themed_root = themed.RootNode();
  REQUIRE(themed_root != nullptr);
  REQUIRE(themed_root->children.size() == 1);
  const auto themed_bar = FindScrollBarGeometry(*themed_root);
  REQUIRE(themed_bar.has_value());
  REQUIRE(themed_bar->style.thickness == 9.0F);
  REQUIRE(themed_bar->style.minimum_thumb_extent == 30.0F);
  REQUIRE(themed_bar->style.corner_radius == 4.5F);

  Runtime dark{FlatDarkScrollBarApp, platform};
  dark.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  dark.BuildFrame();
  const auto* dark_root = dark.RootNode();
  REQUIRE(dark_root != nullptr);
  REQUIRE(dark_root->children.size() == 1);
  const auto dark_bar = FindScrollBarGeometry(*dark_root);
  REQUIRE(dark_bar.has_value());
  const ThemeSpec dark_theme = huxerui::FlatDarkThemeSpec();
  REQUIRE(dark_bar->style.thumb_color.red == dark_theme.colors.on_surface.red);
  REQUIRE(dark_bar->style.thumb_color.alpha == 0.55F);
  REQUIRE(dark_bar->style.fade_in_duration == static_cast<float>(dark_theme.motion.fast));

  alternate_scroll_bar_style = State<bool>{};
  Runtime dynamic{DynamicScrollBarApp, platform};
  dynamic.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  dynamic.BuildFrame();
  const auto* dynamic_node = dynamic.RootNode()->children[0]->children[0].get();
  REQUIRE(dynamic_node != nullptr);
  REQUIRE(dynamic_node->extensions.size() == 1);
  const NodeExtension* dynamic_extension = dynamic_node->extensions[0].extension.get();
  REQUIRE(FindScrollBarGeometry(*dynamic.RootNode())->style.thickness == 7.0F);

  alternate_scroll_bar_style = true;
  dynamic.BuildFrame();
  dynamic_node = dynamic.RootNode()->children[0]->children[0].get();
  REQUIRE(dynamic_node->extensions[0].extension.get() == dynamic_extension);
  REQUIRE(FindScrollBarGeometry(*dynamic.RootNode())->style.thickness == 11.0F);
}

TEST_CASE("TestDefaultIndicationConsumesCompiledThemeValue") {
  alternate_default_indication = State<bool>{};
  TestPlatform platform;
  Runtime runtime{DynamicDefaultIndicationApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 80.0F}});
  runtime.BuildFrame();

  const auto* button = runtime.RootNode()->children[0].get();
  REQUIRE(button != nullptr);
  REQUIRE(button->extensions.size() == 1);
  const NodeExtension* extension = button->extensions[0].extension.get();

  runtime.HandlePointerEvent({PointerEventType::Move, 1, {20.0F, 20.0F}, PointerDeviceKind::Mouse});
  REQUIRE(FindRectWithColor(runtime.BuildFrame(), Color::Rgb(180, 30, 40)) != nullptr);

  alternate_default_indication = true;
  REQUIRE(FindRectWithColor(runtime.BuildFrame(), Color::Rgb(20, 100, 180)) != nullptr);
  button = runtime.RootNode()->children[0].get();
  REQUIRE(button->extensions[0].extension.get() == extension);
}

TEST_CASE("TestFrameClockAndScrollBarAutoHide") {
  MotionController animated{0.0F};
  animated.AnimateTo(1.0F, TweenSpec{0.2, Easing::EaseOut});
  REQUIRE(animated.Advance({1.0, 0.0}).needs_frame);
  REQUIRE(animated.IsRunning());
  REQUIRE(animated.Advance({1.1, 0.1}).needs_frame);
  REQUIRE(std::abs(animated.Value() - 0.875F) < 0.001F);
  REQUIRE(!animated.Advance({1.21, 0.11}).needs_frame);
  REQUIRE(animated.Value() == 1.0F);

  drag_item_clicks = 0;
  drag_item_cancels = 0;

  TestPlatform platform;
  Runtime runtime{DragScrollApp, platform};
  runtime.SetWindowMetrics({.viewport = {100.0F, 100.0F}});
  const FlattenedScene& initial = runtime.BuildFrame();
  const auto geometry = huxerui::detail::ResolveScrollBarGeometry(*runtime.RootNode());
  REQUIRE(geometry.has_value());
  REQUIRE(ContainsRect(initial, geometry->thumb));
  REQUIRE(runtime.LastCommit().next_frame_deadline.has_value());
  REQUIRE(std::abs(*runtime.LastCommit().next_frame_deadline - 0.7) < 0.001);

  platform.AdvanceTime(0.7);
  runtime.BuildFrame();
  REQUIRE(runtime.LastCommit().next_frame_deadline == platform.current_time);

  platform.AdvanceTime(0.11);
  const FlattenedScene& fading = runtime.BuildFrame();
  const auto fading_alpha = RectAlpha(fading, geometry->thumb);
  REQUIRE(fading_alpha.has_value());
  REQUIRE(*fading_alpha > 0.0F);
  REQUIRE(*fading_alpha < geometry->style.thumb_color.alpha);

  platform.AdvanceTime(0.11);
  const FlattenedScene& hidden = runtime.BuildFrame();
  REQUIRE(!ContainsRect(hidden, geometry->thumb));

  ClickAt(runtime, {94.0F, 80.0F}, 60);
  REQUIRE(drag_item_clicks == 1);

  runtime.HandleScrollInput(
      ScrollInputEvent{
          {50.0F, 50.0F},
          0.0F,
          40.0F,
      }
  );
  runtime.BuildFrame();
  platform.AdvanceTime(0.12);
  const FlattenedScene& shown = runtime.BuildFrame();
  const auto shown_geometry = huxerui::detail::ResolveScrollBarGeometry(*runtime.RootNode());
  REQUIRE(shown_geometry.has_value());
  REQUIRE(ContainsRect(shown, shown_geometry->thumb));

  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Move,
          61,
          {94.0F, 50.0F},
      }
  );
  runtime.BuildFrame();
  platform.AdvanceTime(2.0);
  const FlattenedScene& held = runtime.BuildFrame();
  REQUIRE(ContainsRect(held, shown_geometry->thumb));

  runtime.HandlePointerEvent(
      PointerEvent{
          PointerEventType::Cancel,
          61,
          {120.0F, 50.0F},
      }
  );
  runtime.BuildFrame();
  platform.AdvanceTime(0.7);
  runtime.BuildFrame();
  platform.AdvanceTime(0.22);
  const FlattenedScene& hidden_after_exit = runtime.BuildFrame();
  REQUIRE(!ContainsRect(hidden_after_exit, shown_geometry->thumb));
}

TEST_CASE("TestEaseInTweenStartsSlowlyAndReachesItsTarget") {
  MotionController animated{0.0F};
  animated.AnimateTo(1.0F, TweenSpec{1.0, Easing::EaseIn});

  REQUIRE(animated.Advance({2.0, 0.0}).needs_frame);
  REQUIRE(animated.Advance({2.25, 0.25}).needs_frame);
  REQUIRE(animated.Value() == Catch::Approx(0.015625F));
  REQUIRE(animated.Advance({2.5, 0.25}).needs_frame);
  REQUIRE(animated.Value() == Catch::Approx(0.125F));
  REQUIRE(animated.Advance({2.75, 0.25}).needs_frame);
  REQUIRE(animated.Value() == Catch::Approx(0.421875F));
  REQUIRE_FALSE(animated.Advance({3.0, 0.25}).needs_frame);
  REQUIRE(animated.Value() == 1.0F);
}

namespace {

State<bool> alternate_disabled_button_style;
std::optional<DialogHandle> saved_dialogs;
State<bool> first_focus_enabled;
std::vector<std::string> focus_changes;
std::vector<Key> received_keys;
int first_keyboard_clicks = 0;
int third_keyboard_clicks = 0;
int custom_keyboard_clicks = 0;
std::vector<std::string> key_route;
bool consume_root_key = false;
bool consume_focused_intercept = false;
bool consume_focused_key_down = false;
bool consume_focused_key_up = false;
int routed_keyboard_clicks = 0;
int disabled_clicks = 0;
int underlying_clicks = 0;
int background_dialog_clicks = 0;
int first_dialog_clicks = 0;
int second_dialog_clicks = 0;

View InteractionTestTheme(View content) {
  ThemeSpec spec = huxerui::FlatLightThemeSpec();
  spec.motion.reduced_motion = true;
  spec.interactions.indication.hover = IndicationLayer{.fill = Color::Rgb(20, 80, 160, 0.2F)};
  spec.interactions.indication.press = IndicationLayer{.fill = Color::Rgb(200, 40, 60, 0.3F)};
  return Theme {ThemeDefinition{spec}, std::move(content)};
}

View ThemedIndicationApp() {
  return InteractionTestTheme(Button("themed indication").OnClick([] {}));
}

View FallbackIndicationApp() {
  ThemeSpec spec = huxerui::FlatLightThemeSpec();
  spec.motion.reduced_motion = true;
  spec.interactions.indication.hover = IndicationLayer{.fill = Color::Rgb(20, 80, 160, 0.2F)};
  spec.interactions.indication.press.reset();
  return Theme {ThemeDefinition{spec}, Button("fallback indication").OnClick([] {})};
}

View SurfaceIndicationApp() {
  const Color normal_border = Color::Rgb(160, 40, 60);
  const Color pressed_border = Color::Rgb(20, 100, 200);
  return Button("surface indication")
      .OnClick([] {})
      .With(
          Frame{160.0F, 48.0F},
          Border{normal_border, 2.0F},
          CornerRadius{8.0F},
          Indication{
              .press = IndicationLayer{
                  .border = Border{pressed_border, 4.0F},
                  .corner_radii = CornerRadii{20.0F},
                  .enter = SnapSpec{},
                  .exit = SnapSpec{},
              },
          }
      );
}

View FocusTestTheme(View content) {
  ThemeSpec spec = huxerui::FlatLightThemeSpec();
  spec.motion.reduced_motion = true;
  spec.interactions.focus_ring = FocusRing{Color::Rgb(40, 180, 90), 3.0F, 4.0F};
  spec.interactions.disabled_opacity = 0.3F;
  return Theme {ThemeDefinition{spec}, std::move(content)};
}

View FlatSliderFocusApp() {
  return FocusTestTheme(Slider(0.5F));
}

View FocusContent() {
  HUXERUI_SCOPE({
    first_focus_enabled = UseState(true);
    return Column {
      Button("first")
          .With(Enabled{first_focus_enabled})
          .OnClick([] { ++first_keyboard_clicks; })
          .On<ViewEvents::FocusChanged>([](bool focused) {
            focus_changes.push_back(focused ? "first:on" : "first:off");
          }),
      Button("disabled").With(Enabled{false}).OnClick([] { ++disabled_clicks; }),
      Button("third").OnClick([] { ++third_keyboard_clicks; }).On<ViewEvents::FocusChanged>([](bool focused) {
        focus_changes.push_back(focused ? "third:on" : "third:off");
      }),
      Text("custom focus")
          .With(Focusable{})
          .OnClick([] { ++custom_keyboard_clicks; })
          .On<ViewEvents::KeyDown>([](const KeyEvent& event) {
            received_keys.push_back(event.key);
            return false;
          }),
    };
  });
}

View FocusApp() {
  return FocusTestTheme(FocusContent());
}

View KeyRoutingApp() {
  return Column {
    Text("focused")
        .With(Focusable{})
        .OnClick([] { ++routed_keyboard_clicks; })
        .On<ViewEvents::KeyIntercept>([](const KeyEvent&) {
          key_route.push_back("focused-intercept");
          return consume_focused_intercept;
        })
        .On<ViewEvents::KeyDown>([](const KeyEvent&) {
          key_route.push_back("down");
          return consume_focused_key_down;
        })
        .On<ViewEvents::KeyUp>([](const KeyEvent&) {
          key_route.push_back("up");
          return consume_focused_key_up;
        }),
  }.On<ViewEvents::KeyIntercept>([](const KeyEvent&) {
    key_route.push_back("root-intercept");
    return consume_root_key;
  });
}

View FocusOnlyApp() {
  return FocusTestTheme(Text("focus only").With(Focusable{}));
}

View InvalidFocusRingApp() {
  ThemeSpec spec = FlatLightThemeSpec();
  spec.interactions.focus_ring.width = -1.0F;
  return Theme {ThemeDefinition{spec}, Text("invalid focus ring").With(Focusable{})};
}

View DisabledHitTestApp() {
  return Stack {
    Button("underlying").OnClick([] { ++underlying_clicks; }),
    Button("disabled overlay").With(Enabled{false}).OnClick([] { ++disabled_clicks; }),
  };
}

View DisabledSubtreeApp() {
  return Column {
    Button("disabled child").With(Enabled{true}).OnClick([] { ++disabled_clicks; }),
  }.With(Enabled{false});
}

View DisabledButtonStyleUpdateApp() {
  auto alternate = UseState(false);
  alternate_disabled_button_style = alternate;
  ButtonStyle style = ButtonStyle::Default();
  style.disabled_background = alternate ? Color::Rgb(180, 40, 60) : Color::Rgb(30, 80, 170);
  ThemeDefinition definition;
  definition.Set(style);
  return Theme {std::move(definition), Button("disabled style").With(Enabled{false})};
}

View FocusDialogApp() {
  HUXERUI_SCOPE({
    saved_dialogs = UseDialog();
    return Button("background focus").OnClick([] { ++background_dialog_clicks; });
  });
}

} // namespace

TEST_CASE("TestFlatSliderRetainsThemeFocusRing") {
  TestPlatform platform;
  Runtime runtime{FlatSliderFocusApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 64.0F}});
  runtime.BuildFrame();
  runtime.HandleKeyEvent(KeyEvent{.type = KeyEventType::Down, .key = Key::Tab});
  const FlattenedScene& focused = runtime.BuildFrame();

  const bool drew_focus_ring = std::ranges::any_of(focused.Commands(), [](const PaintCommand& command) {
    const auto* border = std::get_if<DrawBorderCommand>(&command);
    return border != nullptr && border->color == Color::Rgb(40, 180, 90) && border->style.width == 3.0F;
  });
  REQUIRE(drew_focus_ring);
}

TEST_CASE("TestThemeDrivesHoverAndPressedIndication") {
  TestPlatform platform;
  Runtime runtime{ThemedIndicationApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 80.0F}});
  runtime.BuildFrame();

  const Color hover = Color::Rgb(20, 80, 160, 0.2F);
  const Color pressed = Color::Rgb(200, 40, 60, 0.3F);
  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Move,
      101,
      {20.0F, 20.0F},
  });
  const FlattenedScene& hovered = runtime.BuildFrame();
  REQUIRE(FindRectWithColor(hovered, hover) != nullptr);

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Down,
      101,
      {20.0F, 20.0F},
  });
  const FlattenedScene& down = runtime.BuildFrame();
  REQUIRE(FindRectWithColor(down, pressed) != nullptr);

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Up,
      101,
      {20.0F, 20.0F},
  });
  const FlattenedScene& released = runtime.BuildFrame();
  REQUIRE(FindRectWithColor(released, hover) != nullptr);

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Move,
      101,
      {240.0F, 120.0F},
  });
  const FlattenedScene& outside = runtime.BuildFrame();
  REQUIRE(FindRectWithColor(outside, hover) == nullptr);

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Down,
      102,
      {20.0F, 20.0F},
      huxerui::PointerDeviceKind::Touch,
  });
  const FlattenedScene& touch_down = runtime.BuildFrame();
  REQUIRE(FindRectWithColor(touch_down, pressed) != nullptr);

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Up,
      102,
      {20.0F, 20.0F},
      huxerui::PointerDeviceKind::Touch,
  });
  const FlattenedScene& touch_released = runtime.BuildFrame();
  REQUIRE(FindRectWithColor(touch_released, pressed) == nullptr);
  REQUIRE(FindRectWithColor(touch_released, hover) == nullptr);
}

TEST_CASE("TestPressedInteractionFallsBackToAvailableHoverLayer") {
  TestPlatform platform;
  Runtime runtime{FallbackIndicationApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 80.0F}});
  runtime.BuildFrame();

  const Color hover = Color::Rgb(20, 80, 160, 0.2F);
  runtime.HandlePointerEvent(PointerEvent{PointerEventType::Move, 103, {20.0F, 20.0F}});
  REQUIRE(FindRectWithColor(runtime.BuildFrame(), hover) != nullptr);

  runtime.HandlePointerEvent(PointerEvent{PointerEventType::Down, 103, {20.0F, 20.0F}});
  REQUIRE(FindRectWithColor(runtime.BuildFrame(), hover) != nullptr);
}

TEST_CASE("TestIndicationReplacesResolvedBorderAndCornerRadii") {
  TestPlatform platform;
  Runtime runtime{SurfaceIndicationApp, platform};
  runtime.SetWindowMetrics({.viewport = {200.0F, 80.0F}});

  const Color normal_border = Color::Rgb(160, 40, 60);
  const Color pressed_border = Color::Rgb(20, 100, 200);
  const FlattenedScene& normal = runtime.BuildFrame();
  REQUIRE(FindBorderWithColor(normal, normal_border) != nullptr);
  REQUIRE(FindBorderWithColor(normal, pressed_border) == nullptr);

  runtime.HandlePointerEvent(PointerEvent{PointerEventType::Down, 104, {20.0F, 20.0F}});
  const FlattenedScene& pressed = runtime.BuildFrame();
  const DrawBorderCommand* border = FindBorderWithColor(pressed, pressed_border);
  REQUIRE(border != nullptr);
  REQUIRE(border->style.width == 4.0F);
  REQUIRE(border->corner_radius == 20.0F);
  REQUIRE(FindBorderWithColor(pressed, normal_border) == nullptr);
}

TEST_CASE("TestEnabledInheritanceAndHitTestBlocking") {
  disabled_clicks = 0;
  underlying_clicks = 0;

  TestPlatform platform;
  Runtime overlay{DisabledHitTestApp, platform};
  overlay.SetWindowMetrics({.viewport = {200.0F, 80.0F}});
  const FlattenedScene& scene = overlay.BuildFrame();
  const DrawTextCommand* disabled = FindText(scene, "disabled overlay");
  REQUIRE(disabled != nullptr);
  REQUIRE(std::abs(disabled->style.foreground.alpha - 0.42F) < 0.001F);

  const auto* overlay_root = overlay.RootNode();
  REQUIRE(overlay_root != nullptr);
  REQUIRE(overlay_root->children.size() == 2);
  REQUIRE(!overlay_root->children[1]->IsEnabled());
  REQUIRE(overlay_root->children[1]->render_node.opacity == 1.0F);
  ClickAt(
      overlay,
      {
          disabled->rect.x + disabled->rect.width * 0.5F,
          disabled->rect.y + disabled->rect.height * 0.5F,
      },
      102
  );
  REQUIRE(disabled_clicks == 0);
  REQUIRE(underlying_clicks == 0);

  Runtime subtree{DisabledSubtreeApp, platform};
  subtree.SetWindowMetrics({.viewport = {200.0F, 80.0F}});
  const FlattenedScene& subtree_display = subtree.BuildFrame();
  const auto* subtree_root = subtree.RootNode();
  REQUIRE(subtree_root != nullptr);
  REQUIRE(!subtree_root->IsEnabled());
  REQUIRE(subtree_root->applies_disabled_appearance);
  REQUIRE(subtree_root->render_node.opacity == Catch::Approx(0.42F));
  REQUIRE(subtree_root->children.size() == 1);
  REQUIRE(!subtree_root->children[0]->IsEnabled());
  REQUIRE_FALSE(subtree_root->children[0]->applies_disabled_appearance);
  REQUIRE(subtree_root->children[0]->render_node.opacity == 1.0F);
  const DrawTextCommand* child = FindText(subtree_display, "disabled child");
  REQUIRE(child != nullptr);
  REQUIRE(child->style.foreground.alpha == 1.0F);
  ClickAt(
      subtree,
      {
          child->rect.x + child->rect.width * 0.5F,
          child->rect.y + child->rect.height * 0.5F,
      },
      103
  );
  REQUIRE(disabled_clicks == 0);
}

TEST_CASE("TestDisabledButtonStyleChangeInvalidatesContentPaint") {
  TestPlatform platform;
  Runtime runtime{DisabledButtonStyleUpdateApp, platform};
  runtime.SetWindowMetrics({.viewport = {180.0F, 64.0F}});

  const Color initial = Color::Rgb(30, 80, 170);
  REQUIRE(FindRectWithColor(runtime.BuildFrame(), initial) != nullptr);

  alternate_disabled_button_style = true;
  const Color updated = Color::Rgb(180, 40, 60);
  const FlattenedScene& scene = runtime.BuildFrame();
  REQUIRE(FindRectWithColor(scene, initial) == nullptr);
  REQUIRE(FindRectWithColor(scene, updated) != nullptr);
}

TEST_CASE("TestFocusTraversalKeyboardAndThemeVisuals") {
  focus_changes.clear();
  received_keys.clear();
  first_keyboard_clicks = 0;
  third_keyboard_clicks = 0;
  custom_keyboard_clicks = 0;
  disabled_clicks = 0;

  TestPlatform platform;
  Runtime runtime{FocusApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 180.0F}});
  runtime.BuildFrame();

  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Tab,
  });
  const FlattenedScene& first_focused = runtime.BuildFrame();
  REQUIRE(focus_changes.size() == 1);
  REQUIRE(focus_changes.back() == "first:on");
  const DrawBorderCommand* first_border = FindBorderWithColor(first_focused, Color::Rgb(40, 180, 90));
  REQUIRE(first_border != nullptr);
  REQUIRE(first_border->style.width == 3.0F);
  const detail::MountedNode* focused_first_node = FindMountedText(*runtime.RootNode(), "first");
  REQUIRE(focused_first_node != nullptr);
  const Rect focused_bounds = focused_first_node->PresentationBounds();
  const float focus_ring_outset = 7.0F;
  const Rect expected_focus_ring{
      focused_bounds.x - focus_ring_outset,
      focused_bounds.y - focus_ring_outset,
      focused_bounds.width + focus_ring_outset * 2.0F,
      focused_bounds.height + focus_ring_outset * 2.0F,
  };
  REQUIRE(first_border->rect == expected_focus_ring);
  REQUIRE(first_border->corner_radius == focused_first_node->properties.corner_radii.top_left + focus_ring_outset);

  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Enter,
  });
  REQUIRE(first_keyboard_clicks == 1);
  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Up,
      .key = Key::Enter,
  });

  first_focus_enabled = false;
  const FlattenedScene& disabled_first = runtime.BuildFrame();
  REQUIRE(focus_changes.back() == "first:off");
  const DrawTextCommand* first_text = FindText(disabled_first, "first");
  REQUIRE(first_text != nullptr);
  REQUIRE(std::abs(first_text->style.foreground.alpha - 0.3F) < 0.001F);
  const detail::MountedNode* first_node = FindMountedText(*runtime.RootNode(), "first");
  REQUIRE(first_node != nullptr);
  REQUIRE(first_node->render_node.opacity == 1.0F);

  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Tab,
  });
  runtime.BuildFrame();
  REQUIRE(focus_changes.back() == "third:on");

  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Space,
  });
  REQUIRE(third_keyboard_clicks == 0);
  const FlattenedScene& space_down = runtime.BuildFrame();
  const ThemeSpec light_theme = huxerui::FlatLightThemeSpec();
  const Color* keyboard_press = LayerFillColor(light_theme.interactions.indication.press);
  REQUIRE(keyboard_press != nullptr);
  REQUIRE(FindRectWithColor(space_down, *keyboard_press) != nullptr);

  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Up,
      .key = Key::Space,
  });
  REQUIRE(third_keyboard_clicks == 1);

  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Tab,
  });
  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::ArrowRight,
  });
  REQUIRE(received_keys.size() == 1);
  REQUIRE(received_keys.front() == Key::ArrowRight);
  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Enter,
  });
  REQUIRE(custom_keyboard_clicks == 1);

  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Tab,
      .modifiers = {
          .shift = true,
      },
  });
  runtime.BuildFrame();
  REQUIRE(focus_changes.back() == "third:on");
}

TEST_CASE("TestKeyRoutingConsumptionAndActivation") {
  key_route.clear();
  consume_root_key = false;
  consume_focused_intercept = false;
  consume_focused_key_down = false;
  consume_focused_key_up = false;
  routed_keyboard_clicks = 0;

  TestPlatform platform;
  Runtime runtime{KeyRoutingApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 80.0F}});
  runtime.BuildFrame();
  const auto send_key = [&runtime](KeyEventType type, Key key) {
    return runtime.HandleKeyEvent({.type = type, .key = key});
  };

  REQUIRE(send_key(KeyEventType::Down, Key::Tab));
  REQUIRE((key_route == std::vector<std::string>{"root-intercept"}));

  key_route.clear();
  REQUIRE_FALSE(send_key(KeyEventType::Down, Key::Unknown));
  REQUIRE((key_route == std::vector<std::string>{"root-intercept", "focused-intercept", "down"}));
  REQUIRE(runtime.HandleKeyEvent(KeyEvent{.type = KeyEventType::Down, .key = Key::Tab, .repeat = true}));

  key_route.clear();
  consume_root_key = true;
  REQUIRE(send_key(KeyEventType::Down, Key::B));
  REQUIRE((key_route == std::vector<std::string>{"root-intercept"}));

  key_route.clear();
  consume_root_key = false;
  consume_focused_intercept = true;
  REQUIRE(send_key(KeyEventType::Down, Key::C));
  REQUIRE((key_route == std::vector<std::string>{"root-intercept", "focused-intercept"}));

  key_route.clear();
  consume_focused_intercept = false;
  consume_focused_key_down = true;
  REQUIRE(send_key(KeyEventType::Down, Key::Enter));
  REQUIRE(routed_keyboard_clicks == 0);

  consume_focused_key_down = false;
  REQUIRE(send_key(KeyEventType::Down, Key::Enter));
  REQUIRE(routed_keyboard_clicks == 1);
  REQUIRE(runtime.HandleKeyEvent(KeyEvent{.type = KeyEventType::Down, .key = Key::Enter, .repeat = true}));
  REQUIRE(send_key(KeyEventType::Up, Key::Enter));
  REQUIRE(routed_keyboard_clicks == 1);

  REQUIRE(send_key(KeyEventType::Down, Key::Space));
  REQUIRE(runtime.HandleKeyEvent(KeyEvent{.type = KeyEventType::Down, .key = Key::Space, .repeat = true}));
  consume_focused_key_up = true;
  REQUIRE(send_key(KeyEventType::Up, Key::Space));
  REQUIRE(routed_keyboard_clicks == 1);
}

TEST_CASE("TestPointerFocusDoesNotPaintFocusRing") {
  focus_changes.clear();

  TestPlatform platform;
  Runtime runtime{FocusApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 180.0F}});
  const FlattenedScene& initial = runtime.BuildFrame();
  const DrawTextCommand* first = FindText(initial, "first");
  REQUIRE(first != nullptr);
  const Point pointer{
      first->rect.x + first->rect.width * 0.5F,
      first->rect.y + first->rect.height * 0.5F,
  };

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Down,
      104,
      pointer,
  });
  const FlattenedScene& pointer_focused = runtime.BuildFrame();
  REQUIRE(focus_changes.size() == 1);
  REQUIRE(focus_changes.back() == "first:on");
  REQUIRE(FindBorderWithColor(pointer_focused, Color::Rgb(40, 180, 90)) == nullptr);

  runtime.HandlePointerEvent(PointerEvent{
      PointerEventType::Up,
      104,
      pointer,
  });
  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::ControlLeft,
      .modifiers = {
          .control = true,
      },
  });
  const FlattenedScene& modifier_only = runtime.BuildFrame();
  REQUIRE(FindBorderWithColor(modifier_only, Color::Rgb(40, 180, 90)) == nullptr);

  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Unknown,
  });
  const FlattenedScene& unknown_key = runtime.BuildFrame();
  REQUIRE(FindBorderWithColor(unknown_key, Color::Rgb(40, 180, 90)) != nullptr);

  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Tab,
  });
  const FlattenedScene& keyboard_focused = runtime.BuildFrame();
  REQUIRE(focus_changes.back() == "third:on");
  REQUIRE(FindBorderWithColor(keyboard_focused, Color::Rgb(40, 180, 90)) != nullptr);
}

TEST_CASE("TestFocusableViewWithoutClickPaintsFocusRing") {
  TestPlatform platform;
  Runtime runtime{FocusOnlyApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 180.0F}});
  runtime.BuildFrame();

  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Tab,
  });
  const FlattenedScene& focused = runtime.BuildFrame();
  REQUIRE(FindBorderWithColor(focused, Color::Rgb(40, 180, 90)) != nullptr);
}

TEST_CASE("TestVisualFillIndicationAndFocusRingValidateDuringViewResolution") {
  const auto rejects = [](RootFactory factory) {
    TestPlatform platform;
    Runtime runtime{factory, platform};
    runtime.SetWindowMetrics({.viewport = {160.0F, 80.0F}});
    REQUIRE_THROWS_AS(runtime.BuildFrame(), std::invalid_argument);
  };

  rejects(+[] { return Spacer().With(Background{LinearGradient{.stops = {{0.0F, Color::Black()}}}}); });
  rejects(+[] {
    return Spacer().With(Background{LinearGradient{
        .stops = {{0.0F, Color::Black()}, {1.0F, Color::White()}},
        .transform = {0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F},
    }});
  });
  rejects(+[] { return Spacer().With(Background{ImageFill{.source = ImageAsset{}}}); });
  rejects(+[] {
    return Button("invalid indication").With(Indication{.geometry = {.layer_size = Size{-1.0F, 20.0F}}});
  });
  rejects(+[] {
    return Button("invalid ripple")
        .With(Indication{
            .ripple = RippleEffect{
                .color = {0.0F, 0.0F, 0.0F, std::numeric_limits<float>::quiet_NaN()},
            },
        });
  });

  TestPlatform platform;
  Runtime runtime{InvalidFocusRingApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 80.0F}});
  REQUIRE_THROWS_AS(runtime.BuildFrame(), std::invalid_argument);
}

TEST_CASE("TestModalDialogTrapsAndRestoresFocusTraversal") {
  saved_dialogs.reset();
  background_dialog_clicks = 0;
  first_dialog_clicks = 0;
  second_dialog_clicks = 0;

  TestPlatform platform;
  Runtime runtime{FocusDialogApp, platform};
  runtime.SetWindowMetrics({.viewport = {240.0F, 160.0F}});
  runtime.BuildFrame();

  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Tab,
  });
  runtime.BuildFrame();

  const LayerId dialog = saved_dialogs->Show(
      [] {
        return Column {
          Button("first dialog focus").OnClick([] { ++first_dialog_clicks; }),
          Button("second dialog focus").OnClick([] { ++second_dialog_clicks; }),
        };
      },
      huxerui::DialogOptions{.dismiss_on_outside_press = false}
  );
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);

  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Enter,
  });
  REQUIRE(first_dialog_clicks == 1);
  REQUIRE(background_dialog_clicks == 0);

  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Tab,
  });
  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Enter,
  });
  REQUIRE(second_dialog_clicks == 1);
  REQUIRE(background_dialog_clicks == 0);

  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Tab,
  });
  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Enter,
  });
  REQUIRE(first_dialog_clicks == 2);

  REQUIRE(saved_dialogs->Dismiss(dialog));
  runtime.BuildFrame();
  SettlePresentation(platform, runtime);
  runtime.HandleKeyEvent(KeyEvent{
      .type = KeyEventType::Down,
      .key = Key::Enter,
  });
  REQUIRE(background_dialog_clicks == 1);
}

} // namespace huxerui::test
