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
std::vector<PointerEvent> pointer_events;
std::vector<Point> context_menu_positions;
int gesture_clicks = 0;
int pointer_cancels = 0;
State<float> drag_minimum;
State<Point> moving_drag_offset;
State<bool> gesture_target_enabled;
State<bool> gesture_modifier_present;
State<int> pointer_intercept_mode;
State<bool> pointer_intercept_present;
bool throw_on_gesture_cancel = false;
bool throw_on_transform_started = false;
bool drag_drop_completed = false;
State<std::string> drag_drop_payload;
State<bool> drop_target_accepts;
State<bool> drop_target_uses_string;
State<bool> drag_source_present;
State<bool> drop_target_present;
bool throw_on_drop_predicate = false;
bool throw_on_drop_entered = false;
std::optional<DropEvent> last_drop_event;
std::optional<ScrollController> drag_drop_scroll;

struct MoveOnlyDropPredicate {
  MoveOnlyDropPredicate() = default;
  MoveOnlyDropPredicate(const MoveOnlyDropPredicate&) = delete;
  MoveOnlyDropPredicate(MoveOnlyDropPredicate&&) = default;

  bool operator()(const std::string&) const {
    return true;
  }
};

template <class Predicate>
concept AcceptsStringDropPredicate = requires(Predicate predicate) {
  DropTarget::Accepts<std::string>(std::move(predicate));
};

static_assert(!AcceptsStringDropPredicate<MoveOnlyDropPredicate>);

View MultiTapApp() {
  return Button("multi tap")
      .With(huxerui::Frame{120.0F, 60.0F}, MultiTapGesture{})
      .On<ViewEvents::Pointer>([](const PointerEvent& event) {
        if (event.type == PointerEventType::Up) {
          gesture_events.emplace_back("pointer up");
        }
      })
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
      .On<ViewEvents::Pointer>([](const PointerEvent& event) {
        if (event.type == PointerEventType::Cancel) {
          ++pointer_cancels;
        }
      })
      .On<LongPressEvents::Started>([](const LongPressEvent&) { gesture_events.emplace_back("started"); })
      .On<LongPressEvents::Ended>([](const LongPressEvent&) { gesture_events.emplace_back("ended"); })
      .On<LongPressEvents::Canceled>([](const LongPressEvent&) { gesture_events.emplace_back("canceled"); })
      .OnClick([] { ++gesture_clicks; });
}

View DragApp() {
  return Button("drag")
      .With(huxerui::Frame{120.0F, 60.0F}, DragGesture{})
      .On<ViewEvents::Pointer>([](const PointerEvent& event) {
        if (event.type == PointerEventType::Cancel) {
          ++pointer_cancels;
        }
      })
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
      .On<ViewEvents::Pointer>([](const PointerEvent& event) {
        if (event.type == PointerEventType::Cancel) {
          ++pointer_cancels;
          canceled_pointer_events.push_back(event);
        }
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

View DragDropApp() {
  return Row {
    Text("source")
        .With(huxerui::Frame{80.0F, 60.0F}, DragSource(std::string{"document"}))
        .On<DragSourceEvents::Started>([](const DragEvent&) { gesture_events.emplace_back("source started"); })
        .On<DragSourceEvents::Changed>([](const DragEvent&) { gesture_events.emplace_back("source changed"); })
        .On<DragSourceEvents::Ended>([](const DragDropResult& result) {
          drag_drop_completed = result.dropped;
          gesture_events.emplace_back("source ended");
        })
        .On<DragSourceEvents::Canceled>([](const DragEvent&) { gesture_events.emplace_back("source canceled"); }),
    Text("target")
        .With(huxerui::Frame{80.0F, 60.0F}, DropTarget::Accepts<std::string>())
        .On<DropEvents<std::string>::Entered>([](const std::string& payload, const DropEvent&) {
          gesture_events.emplace_back("entered " + payload);
        })
        .On<DropEvents<std::string>::Moved>([](const std::string&, const DropEvent&) {
          gesture_events.emplace_back("moved");
        })
        .On<DropEvents<std::string>::Exited>([](const std::string&, const DropEvent&) {
          gesture_events.emplace_back("exited");
        })
        .On<DropEvents<std::string>::Dropped>([](const std::string& payload, const DropEvent&) {
          gesture_events.emplace_back("dropped " + payload);
        }),
  };
}

View DragDropPreviewApp() {
  return Row {
    Text("source")
        .With(
            huxerui::Frame{80.0F, 60.0F},
            DragSource(std::string{"document"}, [] {
              return Text("drag preview")
                  .With(DropTarget::Accepts<std::string>())
                  .On<DropEvents<std::string>::Entered>([](const std::string&, const DropEvent&) {
                    gesture_events.emplace_back("preview entered");
                  });
            })
        ),
    Text("target")
        .With(huxerui::Frame{80.0F, 60.0F}, DropTarget::Accepts<std::string>())
        .On<DropEvents<std::string>::Entered>([](const std::string&, const DropEvent&) {
          gesture_events.emplace_back("target entered");
        }),
  };
}

View SnapshotDragDropApp() {
  auto payload = UseState(std::string{"original"});
  drag_drop_payload = payload;
  return Row {
    Text("source").With(huxerui::Frame{80.0F, 60.0F}, DragSource(payload.Get())),
    Text("target")
        .With(huxerui::Frame{80.0F, 60.0F}, DropTarget::Accepts<std::string>())
        .On<DropEvents<std::string>::Dropped>([](const std::string& value, const DropEvent& event) {
          gesture_events.push_back(value);
          last_drop_event = event;
        }),
  };
}

View NestedDropTargetApp() {
  return Column {
    Text("inner")
        .With(
            huxerui::Frame{100.0F, 60.0F},
            DropTarget::Accepts<std::string>([](const std::string&) { return false; })
        ),
  }.With(huxerui::Frame{100.0F, 60.0F}, DropTarget::Accepts<std::string>())
      .On<DropEvents<std::string>::Entered>([](const std::string&, const DropEvent&) {
        gesture_events.emplace_back("ancestor entered");
      });
}

View NestedDropTargetRoot() {
  return Row {
    Text("source").With(huxerui::Frame{80.0F, 60.0F}, DragSource(std::string{"payload"})),
    NestedDropTargetApp(),
  };
}

View AutoScrollDragDropApp() {
  auto scroll = UseScrollController();
  drag_drop_scroll = scroll;
  return Row {
    Text("source").With(huxerui::Frame{60.0F, 100.0F}, DragSource(std::string{"payload"})),
    ScrollView {
      Text("scroll target").With(
          huxerui::Frame{100.0F, 300.0F},
          DropTarget::Accepts<std::string>()
      ),
    }.Controller(scroll).With(huxerui::Frame{100.0F, 100.0F}),
  };
}

View WrongTypeDragDropApp() {
  return Row {
    Text("source")
        .With(huxerui::Frame{80.0F, 60.0F}, DragSource(7))
        .On<DragSourceEvents::Ended>([](const DragDropResult& result) {
          drag_drop_completed = result.dropped;
          gesture_events.emplace_back("ended");
        }),
    Text("target")
        .With(huxerui::Frame{80.0F, 60.0F}, DropTarget::Accepts<std::string>())
        .On<DropEvents<std::string>::Entered>([](const std::string&, const DropEvent&) {
          gesture_events.emplace_back("entered");
        }),
  };
}

View UpdatingDropTargetApp() {
  auto accepts = UseState(true);
  drop_target_accepts = accepts;
  return Row {
    Text("source").With(huxerui::Frame{80.0F, 60.0F}, DragSource(std::string{"payload"})),
    Text("target")
        .With(
            huxerui::Frame{80.0F, 60.0F},
            DropTarget::Accepts<std::string>([accepted = accepts.Get()](const std::string&) { return accepted; })
        )
        .On<DropEvents<std::string>::Entered>([](const std::string&, const DropEvent&) {
          gesture_events.emplace_back("entered");
        })
        .On<DropEvents<std::string>::Exited>([](const std::string&, const DropEvent&) {
          gesture_events.emplace_back("exited");
        }),
  };
}

View RetypedDropTargetApp() {
  auto uses_string = UseState(true);
  drop_target_uses_string = uses_string;
  View target = Text("target")
                    .With(huxerui::Frame{80.0F, 60.0F})
                    .On<DropEvents<std::string>::Entered>([](const std::string&, const DropEvent&) {
                      gesture_events.emplace_back("string entered");
                    })
                    .On<DropEvents<std::string>::Exited>([](const std::string& value, const DropEvent&) {
                      gesture_events.emplace_back("string exited " + value);
                    })
                    .On<DropEvents<int>::Exited>([](const int&, const DropEvent&) {
                      gesture_events.emplace_back("int exited");
                    });
  target = uses_string.Get() ? std::move(target).With(DropTarget::Accepts<std::string>())
                             : std::move(target).With(DropTarget::Accepts<int>());
  return Row {
    Text("source").With(huxerui::Frame{80.0F, 60.0F}, DragSource(std::string{"payload"})),
    std::move(target),
  };
}

View TransformedDragPreviewApp() {
  return Text("scaled source").With(
      huxerui::Frame{80.0F, 60.0F},
      Scale{2.0F, huxerui::TransformOrigin{0.0F, 0.0F}},
      DragSource(std::string{"payload"}, [] { return Text("transformed preview"); })
  );
}

View DragDropLifecycleApp() {
  auto source_present = UseState(true);
  auto target_present = UseState(true);
  drag_source_present = source_present;
  drop_target_present = target_present;

  View source = Text("source placeholder").With(huxerui::Frame{80.0F, 60.0F});
  if (source_present.Get()) {
    source = Text("source")
                 .With(
                     huxerui::Frame{80.0F, 60.0F},
                     DragSource(std::string{"payload"}, [] { return Text("lifecycle preview"); })
                 )
                 .On<DragSourceEvents::Started>([](const DragEvent&) {
                   gesture_events.emplace_back("source started");
                 })
                 .On<DragSourceEvents::Ended>([](const DragDropResult& result) {
                   drag_drop_completed = result.dropped;
                   gesture_events.emplace_back("source ended");
                 })
                 .On<DragSourceEvents::Canceled>([](const DragEvent&) {
                   gesture_events.emplace_back("source canceled");
                 });
  }

  View target = Text("target placeholder").With(huxerui::Frame{80.0F, 60.0F});
  if (target_present.Get()) {
    target = Text("target")
                 .With(huxerui::Frame{80.0F, 60.0F}, DropTarget::Accepts<std::string>())
                 .On<DropEvents<std::string>::Entered>([](const std::string&, const DropEvent&) {
                   gesture_events.emplace_back("target entered");
                 })
                 .On<DropEvents<std::string>::Exited>([](const std::string&, const DropEvent&) {
                   gesture_events.emplace_back("target exited");
                 });
  }

  return Row {
    std::move(source),
    std::move(target),
  };
}

View ThrowingDragDropApp() {
  return Row {
    Text("source")
        .With(
            huxerui::Frame{80.0F, 60.0F},
            DragSource(std::string{"payload"}, [] { return Text("throwing preview"); })
        )
        .On<DragSourceEvents::Started>([](const DragEvent&) {
          gesture_events.emplace_back("source started");
        })
        .On<DragSourceEvents::Canceled>([](const DragEvent&) {
          gesture_events.emplace_back("source canceled");
        }),
    Text("target")
        .With(
            huxerui::Frame{80.0F, 60.0F},
            DropTarget::Accepts<std::string>([](const std::string&) {
              if (throw_on_drop_predicate) {
                throw std::runtime_error("drop predicate failed");
              }
              return true;
            })
        )
        .On<DropEvents<std::string>::Entered>([](const std::string&, const DropEvent&) {
          gesture_events.emplace_back("target entered");
          if (throw_on_drop_entered) {
            throw std::runtime_error("drop entered failed");
          }
        })
        .On<DropEvents<std::string>::Exited>([](const std::string&, const DropEvent&) {
          gesture_events.emplace_back("target exited");
        }),
  };
}

std::string PointerEventName(PointerEventType type) {
  switch (type) {
  case PointerEventType::Down:
    return "down";
  case PointerEventType::Move:
    return "move";
  case PointerEventType::Up:
    return "up";
  case PointerEventType::Cancel:
    return "cancel";
  }
  throw std::logic_error("HuxerUI test received an unknown pointer event type");
}

View PointerInterceptApp() {
  auto mode = UseState(0);
  auto intercept_present = UseState(true);
  pointer_intercept_mode = mode;
  pointer_intercept_present = intercept_present;

  View content = Column {
    Text("raw target")
        .With(huxerui::Frame{120.0F, 60.0F})
        .On<ViewEvents::Pointer>([](const PointerEvent& event) {
          gesture_events.push_back("raw " + PointerEventName(event.type));
        }),
  }.With(huxerui::Frame{120.0F, 60.0F}, DragGesture{})
      .On<DragEvents::Started>([](const DragEvent&) { gesture_events.emplace_back("drag started"); });
  if (intercept_present.Get()) {
    content = std::move(content).On<ViewEvents::PointerIntercept>(
        [current_mode = mode.Get()](const PointerEvent& event) {
          gesture_events.push_back("intercept " + PointerEventName(event.type));
          if (current_mode == 3 && event.type == PointerEventType::Down) {
            throw std::runtime_error("pointer intercept failed");
          }
          if (current_mode == 0) {
            return event.type == PointerEventType::Down;
          }
          if (current_mode == 1) {
            return event.type == PointerEventType::Move;
          }
          return current_mode == 4 &&
                 event.IsButtonPressed(PointerButton::Primary | PointerButton::Secondary);
        }
    );
  }
  return content;
}

View NestedPointerInterceptApp() {
  return Column {
    Text("child")
        .With(huxerui::Frame{120.0F, 60.0F})
        .On<ViewEvents::PointerIntercept>([](const PointerEvent& event) {
          gesture_events.push_back("child " + PointerEventName(event.type));
          return false;
        })
        .On<ViewEvents::Pointer>([](const PointerEvent& event) {
          if (event.type == PointerEventType::Down) {
            gesture_events.emplace_back("raw down");
          }
        }),
  }.With(huxerui::Frame{120.0F, 60.0F})
      .On<ViewEvents::PointerIntercept>([](const PointerEvent& event) {
        gesture_events.push_back("parent " + PointerEventName(event.type));
        return event.type == PointerEventType::Down;
      });
}

View PointerButtonApp() {
  return Column {
    Button("target")
        .With(huxerui::Frame{60.0F, 60.0F})
        .On<ViewEvents::Pointer>([](const PointerEvent& event) {
          if (event.type == PointerEventType::Move) {
            return;
          }
          gesture_events.push_back("raw " + PointerEventName(event.type));
          pointer_events.push_back(event);
        })
        .On<ViewEvents::ContextMenuRequested>([](Point position) {
          gesture_events.emplace_back("child context");
          context_menu_positions.push_back(position);
        })
        .OnClick([] { ++gesture_clicks; }),
  }.With(huxerui::Frame{120.0F, 60.0F})
      .On<ViewEvents::ContextMenuRequested>([](Point) { gesture_events.emplace_back("parent context"); });
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

void Pointer(Runtime& runtime, PointerEventType type, std::int64_t pointer_id, Point position,
             PointerButton changed_button, PointerButton pressed_buttons) {
  runtime.HandlePointerEvent({type, pointer_id, position, PointerDeviceKind::Mouse, 1, changed_button,
                              pressed_buttons});
}

void ResetGestureEvents() {
  gesture_events.clear();
  drag_events.clear();
  transform_events.clear();
  canceled_pointer_events.clear();
  pointer_events.clear();
  context_menu_positions.clear();
  gesture_clicks = 0;
  pointer_cancels = 0;
  throw_on_transform_started = false;
  throw_on_drop_predicate = false;
  throw_on_drop_entered = false;
  drag_drop_completed = false;
  last_drop_event.reset();
  drag_drop_scroll.reset();
}

} // namespace

TEST_CASE("PointerButton masks report changed and pressed buttons") {
  const PointerButton chord = PointerButton::Primary | PointerButton::Secondary;
  REQUIRE((chord & PointerButton::Primary) == PointerButton::Primary);
  REQUIRE((chord & PointerButton::Middle) == PointerButton::None);

  const PointerEvent event{
      PointerEventType::Move,
      1,
      {10.0F, 20.0F},
      PointerDeviceKind::Mouse,
      1,
      PointerButton::None,
      chord,
  };
  REQUIRE(event.IsButtonPressed(PointerButton::Primary));
  REQUIRE(event.IsButtonPressed(chord));
  REQUIRE_FALSE(event.IsButtonPressed(PointerButton::Middle));
  REQUIRE_FALSE(event.IsButtonPressed(PointerButton::None));
}

TEST_CASE("Secondary pointer requests the deepest context menu after raw Up") {
  ResetGestureEvents();
  TestPlatform platform;
  Runtime runtime{PointerButtonApp, platform};
  runtime.SetWindowMetrics({.viewport = {120.0F, 60.0F}});
  runtime.BuildFrame();

  REQUIRE(runtime.HasContextMenuHandler({20.0F, 30.0F}));
  Pointer(runtime, PointerEventType::Down, 50, {20.0F, 30.0F}, PointerButton::Secondary,
          PointerButton::Secondary);
  Pointer(runtime, PointerEventType::Up, 50, {24.0F, 32.0F}, PointerButton::Secondary, PointerButton::None);

  REQUIRE(gesture_events == std::vector<std::string>{"raw down", "raw up", "child context"});
  REQUIRE(gesture_clicks == 0);
  REQUIRE(context_menu_positions == std::vector<Point>{{24.0F, 32.0F}});
  REQUIRE(pointer_events.size() == 2);
  REQUIRE(pointer_events[0].changed_button == PointerButton::Secondary);
  REQUIRE(pointer_events[0].IsButtonPressed(PointerButton::Secondary));
  REQUIRE(pointer_events[1].changed_button == PointerButton::Secondary);
  REQUIRE(pointer_events[1].pressed_buttons == PointerButton::None);
}

TEST_CASE("Middle button and mouse chords remain raw pointer input") {
  ResetGestureEvents();
  TestPlatform platform;
  Runtime runtime{PointerButtonApp, platform};
  runtime.SetWindowMetrics({.viewport = {120.0F, 60.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 51, {20.0F, 30.0F}, PointerButton::Middle,
          PointerButton::Middle);
  Pointer(runtime, PointerEventType::Up, 51, {20.0F, 30.0F}, PointerButton::Middle, PointerButton::None);
  REQUIRE(gesture_events == std::vector<std::string>{"raw down", "raw up"});
  REQUIRE(gesture_clicks == 0);
  REQUIRE(context_menu_positions.empty());

  ResetGestureEvents();
  Pointer(runtime, PointerEventType::Down, 52, {20.0F, 30.0F}, PointerButton::Primary,
          PointerButton::Primary);
  Pointer(runtime, PointerEventType::Down, 52, {20.0F, 30.0F}, PointerButton::Secondary,
          PointerButton::Primary | PointerButton::Secondary);
  Pointer(runtime, PointerEventType::Up, 52, {20.0F, 30.0F}, PointerButton::Secondary,
          PointerButton::Primary);
  Pointer(runtime, PointerEventType::Up, 52, {20.0F, 30.0F}, PointerButton::Primary, PointerButton::None);

  REQUIRE(gesture_events ==
          std::vector<std::string>{"raw down", "raw down", "raw up", "raw up"});
  REQUIRE(gesture_clicks == 0);
  REQUIRE(context_menu_positions.empty());
}

TEST_CASE("Context menu candidate does not transfer to an ancestor") {
  ResetGestureEvents();
  TestPlatform platform;
  Runtime runtime{PointerButtonApp, platform};
  runtime.SetWindowMetrics({.viewport = {120.0F, 60.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 54, {20.0F, 30.0F}, PointerButton::Secondary,
          PointerButton::Secondary);
  Pointer(runtime, PointerEventType::Up, 54, {90.0F, 30.0F}, PointerButton::Secondary, PointerButton::None);

  REQUIRE(gesture_events == std::vector<std::string>{"raw down", "raw up"});
  REQUIRE(context_menu_positions.empty());
}

TEST_CASE("Keyboard context menu uses the focused View center") {
  ResetGestureEvents();
  TestPlatform platform;
  Runtime runtime{PointerButtonApp, platform};
  runtime.SetWindowMetrics({.viewport = {120.0F, 60.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 53, {10.0F, 10.0F}, PointerButton::Primary,
          PointerButton::Primary);
  Pointer(runtime, PointerEventType::Up, 53, {10.0F, 10.0F}, PointerButton::Primary, PointerButton::None);
  ResetGestureEvents();
  runtime.HandleKeyEvent({KeyEventType::Down, Key::F10, {}, {.shift = true}});

  REQUIRE(gesture_events == std::vector<std::string>{"child context"});
  REQUIRE(context_menu_positions == std::vector<Point>{{30.0F, 30.0F}});

  ResetGestureEvents();
  runtime.HandleKeyEvent({KeyEventType::Down, Key::ContextMenu});

  REQUIRE(gesture_events == std::vector<std::string>{"child context"});
  REQUIRE(context_menu_positions == std::vector<Point>{{30.0F, 30.0F}});
}

TEST_CASE("PointerIntercept can own Down before raw delivery and retained recognizers") {
  ResetGestureEvents();
  TestPlatform platform;
  Runtime runtime{PointerInterceptApp, platform};
  runtime.SetWindowMetrics({.viewport = {120.0F, 60.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 30, {10.0F, 30.0F});
  Pointer(runtime, PointerEventType::Move, 30, {30.0F, 30.0F});
  Pointer(runtime, PointerEventType::Up, 30, {40.0F, 30.0F});

  REQUIRE((gesture_events ==
           std::vector<std::string>{"intercept down", "intercept move", "intercept up"}));
}

TEST_CASE("An accepted PointerIntercept continues receiving mouse chords") {
  ResetGestureEvents();
  TestPlatform platform;
  Runtime runtime{PointerInterceptApp, platform};
  runtime.SetWindowMetrics({.viewport = {120.0F, 60.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 55, {10.0F, 30.0F}, PointerButton::Primary,
          PointerButton::Primary);
  Pointer(runtime, PointerEventType::Down, 55, {10.0F, 30.0F}, PointerButton::Secondary,
          PointerButton::Primary | PointerButton::Secondary);
  Pointer(runtime, PointerEventType::Move, 55, {20.0F, 30.0F}, PointerButton::None,
          PointerButton::Primary | PointerButton::Secondary);
  Pointer(runtime, PointerEventType::Up, 55, {20.0F, 30.0F}, PointerButton::Secondary,
          PointerButton::Primary);
  Pointer(runtime, PointerEventType::Up, 55, {20.0F, 30.0F}, PointerButton::Primary, PointerButton::None);

  REQUIRE((gesture_events == std::vector<std::string>{
                                 "intercept down",
                                 "intercept down",
                                 "intercept move",
                                 "intercept up",
                                 "intercept up",
                             }));
}

TEST_CASE("A pending PointerIntercept can accept a mouse chord") {
  ResetGestureEvents();
  TestPlatform platform;
  Runtime runtime{PointerInterceptApp, platform};
  runtime.SetWindowMetrics({.viewport = {120.0F, 60.0F}});
  runtime.BuildFrame();
  pointer_intercept_mode = 4;
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 56, {10.0F, 30.0F}, PointerButton::Primary,
          PointerButton::Primary);
  Pointer(runtime, PointerEventType::Down, 56, {10.0F, 30.0F}, PointerButton::Secondary,
          PointerButton::Primary | PointerButton::Secondary);
  Pointer(runtime, PointerEventType::Up, 56, {20.0F, 30.0F}, PointerButton::Secondary,
          PointerButton::Primary);
  Pointer(runtime, PointerEventType::Up, 56, {20.0F, 30.0F}, PointerButton::Primary, PointerButton::None);

  REQUIRE((gesture_events == std::vector<std::string>{
                                 "intercept down",
                                 "raw down",
                                 "intercept down",
                                 "raw cancel",
                                 "intercept up",
                                 "intercept up",
                             }));
}

TEST_CASE("PointerIntercept can take a pending sequence and cancel its raw target once") {
  ResetGestureEvents();
  TestPlatform platform;
  Runtime runtime{PointerInterceptApp, platform};
  runtime.SetWindowMetrics({.viewport = {120.0F, 60.0F}});
  runtime.BuildFrame();
  pointer_intercept_mode = 1;
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 31, {10.0F, 30.0F});
  Pointer(runtime, PointerEventType::Move, 31, {30.0F, 30.0F});
  Pointer(runtime, PointerEventType::Up, 31, {40.0F, 30.0F});

  REQUIRE((gesture_events == std::vector<std::string>{
                                 "intercept down",
                                 "raw down",
                                 "intercept move",
                                 "raw cancel",
                                 "intercept up",
                             }));
}

TEST_CASE("PointerIntercept false keeps ordinary raw delivery active") {
  ResetGestureEvents();
  TestPlatform platform;
  Runtime runtime{PointerInterceptApp, platform};
  runtime.SetWindowMetrics({.viewport = {120.0F, 60.0F}});
  runtime.BuildFrame();
  pointer_intercept_mode = 2;
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 32, {10.0F, 30.0F});
  Pointer(runtime, PointerEventType::Move, 32, {11.0F, 30.0F});
  Pointer(runtime, PointerEventType::Up, 32, {11.0F, 30.0F});

  REQUIRE((gesture_events == std::vector<std::string>{
                                 "intercept down",
                                 "raw down",
                                 "intercept move",
                                 "raw move",
                                 "intercept up",
                                 "raw up",
                             }));
}

TEST_CASE("PointerIntercept candidates resolve deepest first and cancel pending competitors") {
  ResetGestureEvents();
  TestPlatform platform;
  Runtime runtime{NestedPointerInterceptApp, platform};
  runtime.SetWindowMetrics({.viewport = {120.0F, 60.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 33, {10.0F, 30.0F});
  Pointer(runtime, PointerEventType::Up, 33, {10.0F, 30.0F});

  REQUIRE((gesture_events == std::vector<std::string>{
                                 "child down",
                                 "parent down",
                                 "child cancel",
                                 "parent up",
                             }));
}

TEST_CASE("Removing a pending PointerIntercept cancels the raw sequence without calling the removed handler") {
  ResetGestureEvents();
  TestPlatform platform;
  Runtime runtime{PointerInterceptApp, platform};
  runtime.SetWindowMetrics({.viewport = {120.0F, 60.0F}});
  runtime.BuildFrame();
  pointer_intercept_mode = 2;
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 34, {10.0F, 30.0F});
  pointer_intercept_present = false;
  runtime.BuildFrame();
  Pointer(runtime, PointerEventType::Up, 34, {10.0F, 30.0F});

  REQUIRE((gesture_events == std::vector<std::string>{"intercept down", "raw down", "raw cancel"}));
}

TEST_CASE("A throwing PointerIntercept quarantines its sequence") {
  ResetGestureEvents();
  TestPlatform platform;
  Runtime runtime{PointerInterceptApp, platform};
  runtime.SetWindowMetrics({.viewport = {120.0F, 60.0F}});
  runtime.BuildFrame();
  pointer_intercept_mode = 3;
  runtime.BuildFrame();

  REQUIRE_THROWS_AS(Pointer(runtime, PointerEventType::Down, 35, {10.0F, 30.0F}), std::runtime_error);
  Pointer(runtime, PointerEventType::Move, 35, {20.0F, 30.0F});
  Pointer(runtime, PointerEventType::Up, 35, {20.0F, 30.0F});

  REQUIRE((gesture_events == std::vector<std::string>{"intercept down", "intercept cancel"}));
}

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
  REQUIRE(canceled_pointer_events.front().changed_button == PointerButton::None);
  REQUIRE(canceled_pointer_events.front().pressed_buttons == PointerButton::None);
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

TEST_CASE("Typed drag-and-drop preserves source and target lifecycle order") {
  ResetGestureEvents();
  TestPlatform platform;
  Runtime runtime{DragDropApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 60.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 30, {20.0F, 30.0F});
  Pointer(runtime, PointerEventType::Move, 30, {40.0F, 30.0F});
  Pointer(runtime, PointerEventType::Move, 30, {120.0F, 30.0F});
  Pointer(runtime, PointerEventType::Up, 30, {120.0F, 30.0F});

  REQUIRE(gesture_events == std::vector<std::string>{
                                "source started",
                                "source changed",
                                "entered document",
                                "dropped document",
                                "source ended",
                            });
  REQUIRE(drag_drop_completed);
}

TEST_CASE("Cancel exits the active drop target before canceling the source") {
  ResetGestureEvents();
  TestPlatform platform;
  Runtime runtime{DragDropApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 60.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 31, {20.0F, 30.0F});
  Pointer(runtime, PointerEventType::Move, 31, {40.0F, 30.0F});
  Pointer(runtime, PointerEventType::Move, 31, {120.0F, 30.0F});
  Pointer(runtime, PointerEventType::Cancel, 31, {120.0F, 30.0F});

  REQUIRE(gesture_events == std::vector<std::string>{
                                "source started",
                                "source changed",
                                "entered document",
                                "exited",
                                "source canceled",
                            });
  REQUIRE_FALSE(drag_drop_completed);
}

TEST_CASE("DragSource snapshots its payload at pointer Down and reports target-local coordinates") {
  ResetGestureEvents();
  TestPlatform platform;
  Runtime runtime{SnapshotDragDropApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 60.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 32, {20.0F, 30.0F});
  drag_drop_payload = "updated";
  runtime.BuildFrame();
  Pointer(runtime, PointerEventType::Move, 32, {40.0F, 30.0F});
  Pointer(runtime, PointerEventType::Move, 32, {120.0F, 30.0F});
  Pointer(runtime, PointerEventType::Up, 32, {120.0F, 30.0F});

  REQUIRE(gesture_events == std::vector<std::string>{"original"});
  REQUIRE(last_drop_event.has_value());
  REQUIRE(last_drop_event->position == Point{40.0F, 30.0F});
  REQUIRE(last_drop_event->window_position == Point{120.0F, 30.0F});
}

TEST_CASE("A rejecting nested target falls back to a compatible ancestor") {
  ResetGestureEvents();
  TestPlatform platform;
  Runtime runtime{NestedDropTargetRoot, platform};
  runtime.SetWindowMetrics({.viewport = {180.0F, 60.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 33, {20.0F, 30.0F});
  Pointer(runtime, PointerEventType::Move, 33, {40.0F, 30.0F});
  Pointer(runtime, PointerEventType::Move, 33, {120.0F, 30.0F});

  REQUIRE(gesture_events == std::vector<std::string>{"ancestor entered"});
}

TEST_CASE("Drag preview is a transient layer dismissed with the session") {
  ResetGestureEvents();
  TestPlatform platform;
  Runtime runtime{DragDropPreviewApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 80.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 34, {20.0F, 30.0F});
  Pointer(runtime, PointerEventType::Move, 34, {40.0F, 30.0F});
  REQUIRE(ContainsText(runtime.BuildFrame(), "drag preview"));
  Pointer(runtime, PointerEventType::Move, 34, {120.0F, 30.0F});
  REQUIRE(gesture_events == std::vector<std::string>{"target entered"});

  Pointer(runtime, PointerEventType::Cancel, 34, {120.0F, 30.0F});
  REQUIRE_FALSE(ContainsText(runtime.BuildFrame(), "drag preview"));
}

TEST_CASE("A compatible target enables stationary edge auto-scroll through its ancestor route") {
  ResetGestureEvents();
  TestPlatform platform;
  Runtime runtime{AutoScrollDragDropApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 100.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 35, {20.0F, 50.0F});
  Pointer(runtime, PointerEventType::Move, 35, {40.0F, 50.0F});
  Pointer(runtime, PointerEventType::Move, 35, {100.0F, 96.0F});
  platform.AdvanceTime(0.05);
  runtime.BuildFrame();

  REQUIRE(drag_drop_scroll.has_value());
  REQUIRE(drag_drop_scroll->Offset() > 0.0F);
}

TEST_CASE("DropTarget requires exact payload type identity") {
  ResetGestureEvents();
  TestPlatform platform;
  Runtime runtime{WrongTypeDragDropApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 60.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 36, {20.0F, 30.0F});
  Pointer(runtime, PointerEventType::Move, 36, {40.0F, 30.0F});
  Pointer(runtime, PointerEventType::Move, 36, {120.0F, 30.0F});
  Pointer(runtime, PointerEventType::Up, 36, {120.0F, 30.0F});

  REQUIRE(gesture_events == std::vector<std::string>{"ended"});
  REQUIRE_FALSE(drag_drop_completed);
}

TEST_CASE("An active target uses its latest compatible predicate") {
  ResetGestureEvents();
  TestPlatform platform;
  Runtime runtime{UpdatingDropTargetApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 60.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 37, {20.0F, 30.0F});
  Pointer(runtime, PointerEventType::Move, 37, {40.0F, 30.0F});
  Pointer(runtime, PointerEventType::Move, 37, {120.0F, 30.0F});
  drop_target_accepts = false;
  runtime.BuildFrame();

  REQUIRE(gesture_events == std::vector<std::string>{"entered", "exited"});
}

TEST_CASE("An active target retains typed exit dispatch when its declaration changes type") {
  ResetGestureEvents();
  TestPlatform platform;
  Runtime runtime{RetypedDropTargetApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 60.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 38, {20.0F, 30.0F});
  Pointer(runtime, PointerEventType::Move, 38, {40.0F, 30.0F});
  Pointer(runtime, PointerEventType::Move, 38, {120.0F, 30.0F});
  drop_target_uses_string = false;
  runtime.BuildFrame();

  REQUIRE(gesture_events == std::vector<std::string>{"string entered", "string exited payload"});
}

TEST_CASE("Drag preview converts a transformed source grab point into window space") {
  ResetGestureEvents();
  TestPlatform platform;
  Runtime runtime{TransformedDragPreviewApp, platform};
  runtime.SetWindowMetrics({.viewport = {400.0F, 120.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 39, {20.0F, 30.0F});
  Pointer(runtime, PointerEventType::Move, 39, {40.0F, 30.0F});
  const std::optional<Rect> preview = FindPresentedTextRect(runtime.BuildFrame(), "transformed preview");

  REQUIRE(preview.has_value());
  REQUIRE(preview->x == Catch::Approx(20.0F));
}

TEST_CASE("Removing an active DragSource closes its target and preview without calling an unmounted handler") {
  ResetGestureEvents();
  TestPlatform platform;
  Runtime runtime{DragDropLifecycleApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 60.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 41, {20.0F, 30.0F});
  Pointer(runtime, PointerEventType::Move, 41, {40.0F, 30.0F});
  Pointer(runtime, PointerEventType::Move, 41, {120.0F, 30.0F});
  drag_source_present = false;
  runtime.BuildFrame();

  REQUIRE(gesture_events == std::vector<std::string>{"source started", "target entered", "target exited"});
  REQUIRE_FALSE(ContainsText(runtime.BuildFrame(), "lifecycle preview"));
  Pointer(runtime, PointerEventType::Up, 41, {120.0F, 30.0F});
  REQUIRE(gesture_events == std::vector<std::string>{"source started", "target entered", "target exited"});
}

TEST_CASE("Removing an active DropTarget clears it while the source remains owned") {
  ResetGestureEvents();
  TestPlatform platform;
  Runtime runtime{DragDropLifecycleApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 60.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 42, {20.0F, 30.0F});
  Pointer(runtime, PointerEventType::Move, 42, {40.0F, 30.0F});
  Pointer(runtime, PointerEventType::Move, 42, {120.0F, 30.0F});
  drop_target_present = false;
  runtime.BuildFrame();
  Pointer(runtime, PointerEventType::Up, 42, {120.0F, 30.0F});

  REQUIRE(gesture_events == std::vector<std::string>{"source started", "target entered", "source ended"});
  REQUIRE_FALSE(drag_drop_completed);
  REQUIRE_FALSE(ContainsText(runtime.BuildFrame(), "lifecycle preview"));
}

TEST_CASE("A throwing DropTarget predicate quarantines the transfer and dismisses its preview") {
  ResetGestureEvents();
  TestPlatform platform;
  Runtime runtime{ThrowingDragDropApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 60.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 43, {20.0F, 30.0F});
  Pointer(runtime, PointerEventType::Move, 43, {40.0F, 30.0F});
  throw_on_drop_predicate = true;
  REQUIRE_THROWS_AS(
      runtime.HandlePointerEvent({PointerEventType::Move, 43, {120.0F, 30.0F}}),
      std::runtime_error
  );
  throw_on_drop_predicate = false;

  REQUIRE(gesture_events == std::vector<std::string>{"source started", "source canceled"});
  REQUIRE_FALSE(ContainsText(runtime.BuildFrame(), "throwing preview"));
  Pointer(runtime, PointerEventType::Up, 43, {120.0F, 30.0F});
  REQUIRE(gesture_events == std::vector<std::string>{"source started", "source canceled"});
}

TEST_CASE("A throwing DropTarget event exits the committed target before canceling the source") {
  ResetGestureEvents();
  TestPlatform platform;
  Runtime runtime{ThrowingDragDropApp, platform};
  runtime.SetWindowMetrics({.viewport = {160.0F, 60.0F}});
  runtime.BuildFrame();

  Pointer(runtime, PointerEventType::Down, 44, {20.0F, 30.0F});
  Pointer(runtime, PointerEventType::Move, 44, {40.0F, 30.0F});
  throw_on_drop_entered = true;
  REQUIRE_THROWS_AS(
      runtime.HandlePointerEvent({PointerEventType::Move, 44, {120.0F, 30.0F}}),
      std::runtime_error
  );
  throw_on_drop_entered = false;

  REQUIRE(gesture_events == std::vector<std::string>{
                                "source started",
                                "target entered",
                                "target exited",
                                "source canceled",
                            });
  REQUIRE_FALSE(ContainsText(runtime.BuildFrame(), "throwing preview"));
  Pointer(runtime, PointerEventType::Up, 44, {120.0F, 30.0F});
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
