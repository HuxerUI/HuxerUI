#include "runtime_test_support.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace huxerui::test {
namespace {

std::vector<std::string> gesture_events;
std::vector<DragEvent> drag_events;
std::vector<TransformEvent> transform_events;
std::vector<PointerEvent> canceled_pointer_events;
int gesture_clicks = 0;
int pointer_cancels = 0;
State<float> drag_minimum;
State<Point> moving_drag_offset;
State<bool> gesture_target_enabled;
State<bool> gesture_modifier_present;
bool throw_on_gesture_cancel = false;
bool throw_on_transform_started = false;

View MultiTapApp() {
  return Button("multi tap")
      .With(huxerui::Frame{120.0F, 60.0F}, MultiTapGesture{})
      .On<ViewEvents::PointerUp>([](const PointerEvent&) { gesture_events.emplace_back("pointer up"); })
      .On<MultiTapEvents::Recognized>([](const MultiTapEvent& event) {
        gesture_events.emplace_back("multi " + std::to_string(event.count));
      })
      .OnClick([] {
        ++gesture_clicks;
        gesture_events.emplace_back("click");
      });
}

View ExtensionOnlyMultiTapApp() {
  return ScrollView {
    Column {
      Text("multi tap")
          .With(huxerui::Frame{120.0F, 60.0F}, MultiTapGesture{})
          .On<MultiTapEvents::Recognized>([](const MultiTapEvent& event) {
            gesture_events.emplace_back("multi " + std::to_string(event.count));
          }),
    },
  };
}

View NestedTapFallbackApp() {
  return Column {
    Text("child")
        .With(huxerui::Frame{40.0F, 40.0F}, MultiTapGesture{})
        .On<MultiTapEvents::Recognized>([](const MultiTapEvent&) { gesture_events.emplace_back("multi"); }),
  }.With(huxerui::Frame{120.0F, 60.0F})
      .OnClick([] {
        ++gesture_clicks;
        gesture_events.emplace_back("parent click");
      });
}

View LongPressApp() {
  return Button("long press")
      .With(huxerui::Frame{120.0F, 60.0F}, LongPressGesture{})
      .On<ViewEvents::PointerCancel>([](const PointerEvent&) { ++pointer_cancels; })
      .On<LongPressEvents::Started>([](const LongPressEvent&) { gesture_events.emplace_back("started"); })
      .On<LongPressEvents::Ended>([](const LongPressEvent&) { gesture_events.emplace_back("ended"); })
      .On<LongPressEvents::Canceled>([](const LongPressEvent&) { gesture_events.emplace_back("canceled"); })
      .OnClick([] { ++gesture_clicks; });
}

View DragApp() {
  return Button("drag")
      .With(huxerui::Frame{120.0F, 60.0F}, DragGesture{})
      .On<ViewEvents::PointerCancel>([](const PointerEvent&) { ++pointer_cancels; })
      .On<DragEvents::Started>([](const DragEvent& event) {
        gesture_events.emplace_back("started");
        drag_events.push_back(event);
      })
      .On<DragEvents::Changed>([](const DragEvent& event) {
        gesture_events.emplace_back("changed");
        drag_events.push_back(event);
      })
      .On<DragEvents::Ended>([](const DragEvent& event) {
        gesture_events.emplace_back("ended");
        drag_events.push_back(event);
      })
      .On<DragEvents::Canceled>([](const DragEvent& event) {
        gesture_events.emplace_back("canceled");
        drag_events.push_back(event);
      })
      .OnClick([] { ++gesture_clicks; });
}

View TransformApp() {
  return Button("transform")
      .With(huxerui::Frame{160.0F, 100.0F}, TransformGesture{})
      .On<ViewEvents::PointerCancel>([](const PointerEvent& event) {
        ++pointer_cancels;
        canceled_pointer_events.push_back(event);
      })
      .On<TransformEvents::Started>([](const TransformEvent& event) {
        gesture_events.emplace_back("started");
        transform_events.push_back(event);
        if (throw_on_transform_started) {
          throw std::runtime_error("transform start failed");
        }
      })
      .On<TransformEvents::Changed>([](const TransformEvent& event) {
        gesture_events.emplace_back("changed");
        transform_events.push_back(event);
      })
      .On<TransformEvents::Ended>([](const TransformEvent& event) {
        gesture_events.emplace_back("ended");
        transform_events.push_back(event);
      })
      .On<TransformEvents::Canceled>([](const TransformEvent& event) {
        gesture_events.emplace_back("canceled");
        transform_events.push_back(event);
      })
      .OnClick([] { ++gesture_clicks; });
}

View TransformLifecycleApp() {
  auto enabled = UseState(true);
  gesture_target_enabled = enabled;
  return Text("transform lifecycle")
      .With(huxerui::Frame{160.0F, 100.0F}, Enabled(enabled.Get()), TransformGesture{})
      .On<TransformEvents::Started>([](const TransformEvent&) { gesture_events.emplace_back("started"); })
      .On<TransformEvents::Canceled>([](const TransformEvent&) { gesture_events.emplace_back("canceled"); });
}

View DelayedDragApp() {
  return Text("delayed drag")
      .With(
          huxerui::Frame{120.0F, 60.0F},
          DragGesture{.minimum_press_duration = std::chrono::duration<double>{0.4}}
      )
      .On<DragEvents::Started>([](const DragEvent& event) { drag_events.push_back(event); })
      .On<DragEvents::Changed>([](const DragEvent& event) { drag_events.push_back(event); });
}

View ConfigurableDragApp() {
  auto minimum = UseState(10.0F);
  drag_minimum = minimum;
  return Text("configurable drag")
      .With(
          huxerui::Frame{120.0F, 60.0F},
          DragGesture{.minimum_distance = minimum.Get()}
      )
      .On<DragEvents::Started>([](const DragEvent& event) { drag_events.push_back(event); });
}

View MovingDragApp() {
  auto offset = UseState(Point{});
  moving_drag_offset = offset;
  return Text("moving drag")
      .With(
          huxerui::Frame{120.0F, 60.0F},
          Offset(offset.Get()),
          DragGesture{}
      )
      .On<DragEvents::Changed>([offset](const DragEvent& event) {
        drag_events.push_back(event);
        offset = event.translation;
      });
}

View NestedDragApp() {
  return Column {
    Text("child")
        .With(huxerui::Frame{100.0F, 40.0F}, DragGesture{})
        .On<DragEvents::Started>([](const DragEvent&) { gesture_events.emplace_back("child"); }),
  }.With(huxerui::Frame{120.0F, 60.0F}, DragGesture{})
      .On<DragEvents::Started>([](const DragEvent&) { gesture_events.emplace_back("parent"); });
}

View DisabledDragApp() {
  return Text("disabled drag")
      .With(huxerui::Frame{120.0F, 60.0F}, DragGesture{}, Enabled(false))
      .On<DragEvents::Started>([](const DragEvent&) { gesture_events.emplace_back("started"); });
}

View GestureLifecycleApp() {
  auto enabled = UseState(true);
  auto modifier_present = UseState(true);
  gesture_target_enabled = enabled;
  gesture_modifier_present = modifier_present;

  View target = Text("lifecycle drag").With(huxerui::Frame{120.0F, 60.0F}, Enabled(enabled.Get()));
  if (modifier_present.Get()) {
    target = std::move(target).With(DragGesture{});
  }
  return std::move(target)
      .On<DragEvents::Started>([](const DragEvent&) { gesture_events.emplace_back("started"); })
      .On<DragEvents::Canceled>([](const DragEvent&) {
        gesture_events.emplace_back("canceled");
        if (throw_on_gesture_cancel) {
          throw std::runtime_error("gesture cancellation failed");
        }
      })
      .On<DragEvents::Ended>([](const DragEvent&) { gesture_events.emplace_back("ended"); });
}

class GesturePlatform final : public TestPlatform {
public:
  GestureSettings GestureDefaults() const noexcept override {
    return settings;
  }

  GestureSettings settings;
};

void Pointer(Runtime& runtime, PointerEventType type, std::int64_t pointer_id, Point position,
             PointerDeviceKind device = PointerDeviceKind::Mouse) {
  runtime.HandlePointerEvent({type, pointer_id, position, device});
}

void ResetGestureEvents() {
  gesture_events.clear();
  drag_events.clear();
  transform_events.clear();
  canceled_pointer_events.clear();
  gesture_clicks = 0;
  pointer_cancels = 0;
  throw_on_transform_started = false;
}

} // namespace

TEST_CASE("MultiTap shares successful taps with Click and preserves output order") {
  ResetGestureEvents();
  TestPlatform platform;
  Runtime runtime{MultiTapApp, platform};
  runtime.SetWindowMetrics({.viewport = {120.0F, 60.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 1, {30.0F, 30.0F});
  Pointer(runtime, PointerEventType::Up, 1, {30.0F, 30.0F});
  platform.AdvanceTime(0.2);
  Pointer(runtime, PointerEventType::Down, 2, {32.0F, 30.0F});
  Pointer(runtime, PointerEventType::Up, 2, {32.0F, 30.0F});

  REQUIRE(gesture_clicks == 2);
  REQUIRE(gesture_events == std::vector<std::string>{"pointer up", "click", "pointer up", "multi 2", "click"});
}

TEST_CASE("MultiTap resets an incomplete sequence after movement cancellation") {
  ResetGestureEvents();
  TestPlatform platform;
  Runtime runtime{MultiTapApp, platform};
  runtime.SetWindowMetrics({.viewport = {120.0F, 60.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 3, {30.0F, 30.0F});
  Pointer(runtime, PointerEventType::Up, 3, {30.0F, 30.0F});
  Pointer(runtime, PointerEventType::Down, 4, {30.0F, 30.0F});
  Pointer(runtime, PointerEventType::Up, 4, {150.0F, 30.0F});
  Pointer(runtime, PointerEventType::Down, 5, {30.0F, 30.0F});
  Pointer(runtime, PointerEventType::Up, 5, {30.0F, 30.0F});

  REQUIRE(std::ranges::none_of(gesture_events, [](const std::string& event) { return event == "multi 2"; }));
}

TEST_CASE("MultiTap recognizes an extension-only target inside a scroll view") {
  ResetGestureEvents();
  TestPlatform platform;
  Runtime runtime{ExtensionOnlyMultiTapApp, platform};
  runtime.SetWindowMetrics({.viewport = {120.0F, 60.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 6, {30.0F, 30.0F});
  Pointer(runtime, PointerEventType::Up, 6, {30.0F, 30.0F});
  platform.AdvanceTime(0.2);
  Pointer(runtime, PointerEventType::Down, 7, {32.0F, 30.0F});
  Pointer(runtime, PointerEventType::Up, 7, {32.0F, 30.0F});

  REQUIRE(gesture_events == std::vector<std::string>{"multi 2"});
  REQUIRE(gesture_clicks == 0);
}

TEST_CASE("A rejected descendant tap falls back to an eligible ancestor") {
  ResetGestureEvents();
  TestPlatform platform;
  Runtime runtime{NestedTapFallbackApp, platform};
  runtime.SetWindowMetrics({.viewport = {120.0F, 60.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 17, {20.0F, 20.0F});
  Pointer(runtime, PointerEventType::Up, 17, {80.0F, 20.0F});

  REQUIRE(gesture_clicks == 1);
  REQUIRE(gesture_events == std::vector<std::string>{"parent click"});
}

TEST_CASE("LongPress accepts at its deadline and owns the terminal event") {
  ResetGestureEvents();
  TestPlatform platform;
  Runtime runtime{LongPressApp, platform};
  runtime.SetWindowMetrics({.viewport = {120.0F, 60.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 6, {30.0F, 30.0F}, PointerDeviceKind::Touch);
  platform.AdvanceTime(0.49);
  runtime.BuildFrame();
  REQUIRE(gesture_events.empty());

  platform.AdvanceTime(0.02);
  runtime.BuildFrame();
  REQUIRE(gesture_events == std::vector<std::string>{"started"});
  REQUIRE(pointer_cancels == 1);

  Pointer(runtime, PointerEventType::Up, 6, {160.0F, 30.0F}, PointerDeviceKind::Touch);
  REQUIRE(gesture_events == std::vector<std::string>{"started", "ended"});
  REQUIRE(gesture_clicks == 0);
}

TEST_CASE("LongPress rejects early release and reports cancellation only after acceptance") {
  ResetGestureEvents();
  TestPlatform platform;
  Runtime runtime{LongPressApp, platform};
  runtime.SetWindowMetrics({.viewport = {120.0F, 60.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 7, {30.0F, 30.0F}, PointerDeviceKind::Touch);
  platform.AdvanceTime(0.2);
  Pointer(runtime, PointerEventType::Up, 7, {30.0F, 30.0F}, PointerDeviceKind::Touch);
  REQUIRE(gesture_events.empty());
  REQUIRE(gesture_clicks == 1);

  Pointer(runtime, PointerEventType::Down, 8, {30.0F, 30.0F}, PointerDeviceKind::Touch);
  platform.AdvanceTime(0.51);
  runtime.BuildFrame();
  Pointer(runtime, PointerEventType::Cancel, 8, {30.0F, 30.0F}, PointerDeviceKind::Touch);
  REQUIRE(gesture_events == std::vector<std::string>{"started", "canceled"});
}

TEST_CASE("Drag reports frozen local deltas and velocity outside its bounds") {
  ResetGestureEvents();
  TestPlatform platform;
  Runtime runtime{DragApp, platform};
  runtime.SetWindowMetrics({.viewport = {120.0F, 60.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 9, {10.0F, 30.0F});
  platform.AdvanceTime(0.02);
  Pointer(runtime, PointerEventType::Move, 9, {30.0F, 30.0F});
  platform.AdvanceTime(0.02);
  Pointer(runtime, PointerEventType::Move, 9, {140.0F, 40.0F});
  platform.AdvanceTime(0.02);
  Pointer(runtime, PointerEventType::Up, 9, {150.0F, 45.0F});

  REQUIRE(gesture_events == std::vector<std::string>{"started", "changed", "changed", "ended"});
  REQUIRE(pointer_cancels == 1);
  REQUIRE(gesture_clicks == 0);
  REQUIRE(drag_events[1].translation == Point{20.0F, 0.0F});
  REQUIRE(drag_events[2].delta == Point{110.0F, 10.0F});
  REQUIRE(drag_events.back().position == Point{150.0F, 45.0F});
  REQUIRE(drag_events.back().velocity.x > 0.0F);
}

TEST_CASE("Delayed Drag starts with a rebased zero translation") {
  ResetGestureEvents();
  TestPlatform platform;
  Runtime runtime{DelayedDragApp, platform};
  runtime.SetWindowMetrics({.viewport = {120.0F, 60.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 10, {20.0F, 30.0F}, PointerDeviceKind::Touch);
  Pointer(runtime, PointerEventType::Move, 10, {22.0F, 30.0F}, PointerDeviceKind::Touch);
  platform.AdvanceTime(0.41);
  runtime.BuildFrame();
  REQUIRE(drag_events.size() == 1);
  REQUIRE(drag_events.front().translation == Point{});
  REQUIRE(drag_events.front().origin == Point{22.0F, 30.0F});

  Pointer(runtime, PointerEventType::Move, 10, {32.0F, 30.0F}, PointerDeviceKind::Touch);
  REQUIRE(drag_events.back().delta == Point{10.0F, 0.0F});
}

TEST_CASE("Transform atomically owns both pointers and reports incremental geometry") {
  ResetGestureEvents();
  TestPlatform platform;
  Runtime runtime{TransformApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 100.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 30, {20.0F, 30.0F}, PointerDeviceKind::Touch);
  REQUIRE(transform_events.empty());
  Pointer(runtime, PointerEventType::Down, 31, {60.0F, 30.0F}, PointerDeviceKind::Touch);

  REQUIRE(gesture_events == std::vector<std::string>{"started"});
  REQUIRE(canceled_pointer_events.size() == 1);
  REQUIRE(canceled_pointer_events.front().type == PointerEventType::Cancel);
  REQUIRE(canceled_pointer_events.front().pointer_id == 30);
  REQUIRE(canceled_pointer_events.front().position == Point{20.0F, 30.0F});
  REQUIRE(canceled_pointer_events.front().device_kind == PointerDeviceKind::Touch);
  REQUIRE(transform_events.front().pointer_count == 2);
  REQUIRE(transform_events.front().centroid == Point{40.0F, 30.0F});
  REQUIRE(transform_events.front().pan == Point{});
  REQUIRE(transform_events.front().scale == 1.0F);

  Pointer(runtime, PointerEventType::Move, 31, {80.0F, 30.0F}, PointerDeviceKind::Touch);
  REQUIRE(transform_events.back().pan == Point{10.0F, 0.0F});
  REQUIRE(transform_events.back().scale == Catch::Approx(1.5F));
  REQUIRE(transform_events.back().rotation == Catch::Approx(0.0F));

  Pointer(runtime, PointerEventType::Move, 30, {30.0F, 30.0F}, PointerDeviceKind::Touch);
  REQUIRE(transform_events.back().pan == Point{5.0F, 0.0F});
  REQUIRE(transform_events.back().scale == Catch::Approx(5.0F / 6.0F));

  Pointer(runtime, PointerEventType::Move, 31, {55.0F, 55.0F}, PointerDeviceKind::Touch);
  REQUIRE(transform_events.back().pan == Point{-12.5F, 12.5F});
  REQUIRE(transform_events.back().scale == Catch::Approx(std::sqrt(0.5F)));
  REQUIRE(transform_events.back().rotation == Catch::Approx(std::numbers::pi_v<float> / 4.0F));

  Pointer(runtime, PointerEventType::Up, 31, {55.0F, 55.0F}, PointerDeviceKind::Touch);
  REQUIRE(gesture_events ==
          std::vector<std::string>{"started", "changed", "changed", "changed", "ended"});
  REQUIRE(transform_events.back().pointer_count == 1);
  REQUIRE(pointer_cancels == 1);
  REQUIRE(gesture_clicks == 0);

  Pointer(runtime, PointerEventType::Up, 30, {30.0F, 30.0F}, PointerDeviceKind::Touch);
  REQUIRE(gesture_events ==
          std::vector<std::string>{"started", "changed", "changed", "changed", "ended"});
  REQUIRE(gesture_clicks == 0);
}

TEST_CASE("Transform rebases pointer-set changes and cancels the shared recognition once") {
  ResetGestureEvents();
  TestPlatform platform;
  Runtime runtime{TransformApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 100.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 32, {20.0F, 40.0F}, PointerDeviceKind::Touch);
  Pointer(runtime, PointerEventType::Down, 33, {60.0F, 40.0F}, PointerDeviceKind::Touch);
  Pointer(runtime, PointerEventType::Down, 34, {100.0F, 40.0F}, PointerDeviceKind::Touch);

  REQUIRE(gesture_events == std::vector<std::string>{"started", "changed"});
  REQUIRE(transform_events.back().pointer_count == 3);
  REQUIRE(transform_events.back().pan == Point{});
  REQUIRE(transform_events.back().scale == 1.0F);
  REQUIRE(transform_events.back().rotation == 0.0F);

  Pointer(runtime, PointerEventType::Up, 34, {100.0F, 40.0F}, PointerDeviceKind::Touch);
  REQUIRE(gesture_events == std::vector<std::string>{"started", "changed", "changed"});
  REQUIRE(transform_events.back().pointer_count == 2);
  REQUIRE(transform_events.back().pan == Point{});

  Pointer(runtime, PointerEventType::Cancel, 32, {20.0F, 40.0F}, PointerDeviceKind::Touch);
  Pointer(runtime, PointerEventType::Cancel, 33, {60.0F, 40.0F}, PointerDeviceKind::Touch);
  REQUIRE(gesture_events == std::vector<std::string>{"started", "changed", "changed", "canceled"});
  REQUIRE(transform_events.back().pointer_count == 2);
}

TEST_CASE("Transform does not combine pointers from different device kinds") {
  ResetGestureEvents();
  TestPlatform platform;
  Runtime runtime{TransformApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 100.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 35, {20.0F, 40.0F}, PointerDeviceKind::Touch);
  Pointer(runtime, PointerEventType::Down, 36, {60.0F, 40.0F}, PointerDeviceKind::Pen);
  Pointer(runtime, PointerEventType::Up, 36, {60.0F, 40.0F}, PointerDeviceKind::Pen);
  Pointer(runtime, PointerEventType::Up, 35, {20.0F, 40.0F}, PointerDeviceKind::Touch);

  REQUIRE(transform_events.empty());
  REQUIRE(gesture_clicks == 2);
}

TEST_CASE("Disabling an active Transform cancels its shared recognition once") {
  ResetGestureEvents();
  TestPlatform platform;
  Runtime runtime{TransformLifecycleApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 100.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 37, {20.0F, 40.0F}, PointerDeviceKind::Touch);
  Pointer(runtime, PointerEventType::Down, 38, {60.0F, 40.0F}, PointerDeviceKind::Touch);
  REQUIRE(gesture_events == std::vector<std::string>{"started"});

  gesture_target_enabled = false;
  runtime.BuildFrame();
  REQUIRE(gesture_events == std::vector<std::string>{"started", "canceled"});

  Pointer(runtime, PointerEventType::Move, 37, {30.0F, 40.0F}, PointerDeviceKind::Touch);
  Pointer(runtime, PointerEventType::Cancel, 37, {30.0F, 40.0F}, PointerDeviceKind::Touch);
  Pointer(runtime, PointerEventType::Cancel, 38, {60.0F, 40.0F}, PointerDeviceKind::Touch);
  REQUIRE(gesture_events == std::vector<std::string>{"started", "canceled"});
}

TEST_CASE("A throwing Transform handler quarantines every owned pointer") {
  ResetGestureEvents();
  TestPlatform platform;
  Runtime runtime{TransformApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 100.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 39, {20.0F, 40.0F}, PointerDeviceKind::Touch);
  throw_on_transform_started = true;
  REQUIRE_THROWS_AS(
      Pointer(runtime, PointerEventType::Down, 40, {60.0F, 40.0F}, PointerDeviceKind::Touch),
      std::runtime_error
  );
  throw_on_transform_started = false;

  REQUIRE(gesture_events == std::vector<std::string>{"started", "canceled"});
  Pointer(runtime, PointerEventType::Move, 39, {30.0F, 40.0F}, PointerDeviceKind::Touch);
  Pointer(runtime, PointerEventType::Move, 40, {70.0F, 40.0F}, PointerDeviceKind::Touch);
  Pointer(runtime, PointerEventType::Cancel, 39, {30.0F, 40.0F}, PointerDeviceKind::Touch);
  Pointer(runtime, PointerEventType::Cancel, 40, {70.0F, 40.0F}, PointerDeviceKind::Touch);
  REQUIRE(gesture_events == std::vector<std::string>{"started", "canceled"});
}

TEST_CASE("Gesture defaults are snapshotted from PlatformAdapter") {
  ResetGestureEvents();
  GesturePlatform platform;
  platform.settings.pointer_slop = 20.0F;
  Runtime runtime{DragApp, platform};
  runtime.SetWindowMetrics({.viewport = {120.0F, 60.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 11, {10.0F, 30.0F});
  Pointer(runtime, PointerEventType::Move, 11, {25.0F, 30.0F});
  REQUIRE(drag_events.empty());
  Pointer(runtime, PointerEventType::Move, 11, {35.0F, 30.0F});
  REQUIRE_FALSE(drag_events.empty());
}

TEST_CASE("An active Drag snapshots its modifier configuration across compatible reconciliation") {
  ResetGestureEvents();
  TestPlatform platform;
  Runtime runtime{ConfigurableDragApp, platform};
  runtime.SetWindowMetrics({.viewport = {120.0F, 60.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 12, {10.0F, 30.0F});
  drag_minimum = 100.0F;
  runtime.BuildFrame();
  Pointer(runtime, PointerEventType::Move, 12, {25.0F, 30.0F});
  REQUIRE(drag_events.size() == 1);
  Pointer(runtime, PointerEventType::Up, 12, {25.0F, 30.0F});

  drag_events.clear();
  Pointer(runtime, PointerEventType::Down, 13, {10.0F, 30.0F});
  Pointer(runtime, PointerEventType::Move, 13, {25.0F, 30.0F});
  REQUIRE(drag_events.empty());
}

TEST_CASE("Drag keeps its frozen local coordinates while its target moves") {
  ResetGestureEvents();
  TestPlatform platform;
  Runtime runtime{MovingDragApp, platform};
  runtime.SetWindowMetrics({.viewport = {180.0F, 80.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 14, {10.0F, 30.0F});
  Pointer(runtime, PointerEventType::Move, 14, {30.0F, 30.0F});
  runtime.BuildFrame();
  REQUIRE(moving_drag_offset.Get() == Point{20.0F, 0.0F});
  Pointer(runtime, PointerEventType::Move, 14, {50.0F, 30.0F});
  REQUIRE(drag_events.back().translation == Point{40.0F, 0.0F});
}

TEST_CASE("Nested gestures resolve in deterministic deepest-node order") {
  ResetGestureEvents();
  TestPlatform platform;
  Runtime runtime{NestedDragApp, platform};
  runtime.SetWindowMetrics({.viewport = {120.0F, 60.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 15, {10.0F, 20.0F});
  Pointer(runtime, PointerEventType::Move, 15, {30.0F, 20.0F});

  REQUIRE(gesture_events == std::vector<std::string>{"child"});
}

TEST_CASE("Disabled nodes do not create gesture recognizers") {
  ResetGestureEvents();
  TestPlatform platform;
  Runtime runtime{DisabledDragApp, platform};
  runtime.SetWindowMetrics({.viewport = {120.0F, 60.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 16, {10.0F, 30.0F});
  Pointer(runtime, PointerEventType::Move, 16, {30.0F, 30.0F});

  REQUIRE(gesture_events.empty());
}

TEST_CASE("Disabling an active gesture cancels it before later pointer delivery") {
  ResetGestureEvents();
  throw_on_gesture_cancel = false;
  TestPlatform platform;
  Runtime runtime{GestureLifecycleApp, platform};
  runtime.SetWindowMetrics({.viewport = {120.0F, 60.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 18, {10.0F, 30.0F});
  Pointer(runtime, PointerEventType::Move, 18, {30.0F, 30.0F});
  gesture_target_enabled = false;
  runtime.BuildFrame();
  Pointer(runtime, PointerEventType::Up, 18, {40.0F, 30.0F});

  REQUIRE(gesture_events == std::vector<std::string>{"started", "canceled"});
}

TEST_CASE("Removing an active gesture quarantines the sequence without calling an unmounted handler") {
  ResetGestureEvents();
  throw_on_gesture_cancel = false;
  TestPlatform platform;
  Runtime runtime{GestureLifecycleApp, platform};
  runtime.SetWindowMetrics({.viewport = {120.0F, 60.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 19, {10.0F, 30.0F});
  Pointer(runtime, PointerEventType::Move, 19, {30.0F, 30.0F});
  gesture_modifier_present = false;
  runtime.BuildFrame();
  Pointer(runtime, PointerEventType::Up, 19, {40.0F, 30.0F});

  REQUIRE(gesture_events == std::vector<std::string>{"started"});
}

TEST_CASE("A throwing cancellation handler cannot keep gesture ownership live") {
  ResetGestureEvents();
  throw_on_gesture_cancel = false;
  TestPlatform platform;
  Runtime runtime{GestureLifecycleApp, platform};
  runtime.SetWindowMetrics({.viewport = {120.0F, 60.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 20, {10.0F, 30.0F});
  Pointer(runtime, PointerEventType::Move, 20, {30.0F, 30.0F});
  throw_on_gesture_cancel = true;
  REQUIRE_THROWS_AS(runtime.HandlePointerEvent({PointerEventType::Cancel, 20, {30.0F, 30.0F}}), std::runtime_error);
  throw_on_gesture_cancel = false;
  Pointer(runtime, PointerEventType::Up, 20, {40.0F, 30.0F});

  REQUIRE(gesture_events == std::vector<std::string>{"started", "canceled"});
}

TEST_CASE("Gesture declarations and platform defaults reject invalid values") {
  TestPlatform platform;
  Runtime invalid_multi_tap{
      [] { return Text("invalid").With(MultiTapGesture{.count = 1}); },
      platform,
  };
  invalid_multi_tap.SetWindowMetrics({.viewport = {100.0F, 40.0F}});
  REQUIRE_THROWS_AS(invalid_multi_tap.BuildFrame(), std::invalid_argument);

  GesturePlatform invalid_platform;
  invalid_platform.settings.pointer_slop = std::numeric_limits<float>::quiet_NaN();
  REQUIRE_THROWS_AS(Runtime(DragApp, invalid_platform), std::logic_error);
}

} // namespace huxerui::test
