#include <huxerui/huxerui.h>

#include <string>

using namespace huxerui;

[[huxerui::composable]]
View GestureContent() {
  auto tap_count = UseState<std::uint32_t>(0);
  auto long_press_status = UseState(std::string{"Hold to begin"});
  auto drag_offset = UseState(Point{});
  auto drag_origin = UseState(Point{});
  const ThemeSpec& theme = UseTheme();

  return ScrollView {
    Column {
      Text("Gestures", TextRole::Title),
      Text("Gesture modifiers share pointer recognition and ownership with Click and scrolling."),
      Column {
        Text("Multi-tap", TextRole::Title),
        Text("Double-click or double-tap the surface. Click remains independent."),
        Text(tap_count.Get() == 0 ? "Waiting for two taps" : "Recognized " + std::to_string(tap_count.Get()))
            .With(
                Frame{280.0F, 96.0F},
                Padding(theme.spacing.large),
                Background(theme.colors.surface_container_high),
                CornerRadius(theme.shapes.large),
                MultiTapGesture{}
            )
            .On<MultiTapEvents::Recognized>([tap_count](const MultiTapEvent& event) { tap_count = event.count; }),
      }.With(
          Spacing(theme.spacing.small),
          CrossAlign(CrossAxisAlignment::Start)
      ),
      Column {
        Text("Long press", TextRole::Title),
        Text(long_press_status.Get())
            .With(
                Frame{280.0F, 96.0F},
                Padding(theme.spacing.large),
                Background(theme.colors.surface_container_high),
                CornerRadius(theme.shapes.large),
                LongPressGesture{}
            )
            .On<LongPressEvents::Started>([long_press_status](const LongPressEvent&) {
              long_press_status = "Long press started";
            })
            .On<LongPressEvents::Ended>([long_press_status](const LongPressEvent&) {
              long_press_status = "Long press ended";
            })
            .On<LongPressEvents::Canceled>([long_press_status](const LongPressEvent&) {
              long_press_status = "Long press canceled";
            }),
      }.With(
          Spacing(theme.spacing.small),
          CrossAlign(CrossAxisAlignment::Start)
      ),
      Column {
        Text("Drag", TextRole::Title),
        Text("The accepted drag keeps receiving events outside the original surface."),
        Text("Drag me")
            .With(
                Frame{160.0F, 72.0F},
                Padding(theme.spacing.large),
                Background(theme.colors.secondary_container),
                Foreground(theme.colors.on_secondary_container),
                CornerRadius(theme.shapes.large),
                Offset(drag_offset.Get()),
                DragGesture{}
            )
            .On<DragEvents::Started>([drag_origin, drag_offset](const DragEvent&) {
              drag_origin = drag_offset.Get();
            })
            .On<DragEvents::Changed>([drag_origin, drag_offset](const DragEvent& event) {
              const Point origin = drag_origin.Get();
              drag_offset = Point{origin.x + event.translation.x, origin.y + event.translation.y};
            }),
      }.With(
          Frame{.height = 240.0F},
          Spacing(theme.spacing.small),
          CrossAlign(CrossAxisAlignment::Start)
      ),
    }.With(
        Padding(theme.spacing.extra_large),
        Spacing(theme.spacing.extra_large),
        CrossAlign(CrossAxisAlignment::Stretch)
    ),
  }.With(Background(theme.colors.background));
}

View App() {
  return MaterialTheme {
    GestureContent(),
  };
}

const Application application{
    App,
    {
        .window = {
            .title = "HuxerUI Gestures",
            .initial_size = {720.0F, 720.0F},
        },
    }
};
