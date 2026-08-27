#include <huxerui/huxerui.h>

#include <numbers>
#include <string>

using namespace huxerui;

[[huxerui::composable]]
View GestureContent() {
  auto tap_count = UseState<std::uint32_t>(0);
  auto long_press_status = UseState(std::string{"Hold to begin"});
  auto drag_offset = UseState(Point{});
  auto drag_origin = UseState(Point{});
  auto transform_scale = UseState(1.0F);
  auto transform_rotation = UseState(0.0F);
  auto transform_pointers = UseState<std::uint32_t>(0);
  const ThemeSpec& theme = UseTheme();
  const auto apply_transform = [=](const TransformEvent& event) {
    transform_scale = transform_scale.Get() * event.scale;
    transform_rotation = transform_rotation.Get() + event.rotation * 180.0F / std::numbers::pi_v<float>;
    transform_pointers = event.pointer_count;
  };

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
      Column {
        Text("Transform", TextRole::Title),
        Text("Use two or more pointers to pinch and rotate the surface."),
        Text(transform_pointers.Get() < 2 ? "Waiting for two pointers"
                                          : std::to_string(transform_pointers.Get()) + " pointers")
            .With(
                Frame{280.0F, 140.0F},
                Padding(theme.spacing.large),
                Background(theme.colors.primary),
                Foreground(theme.colors.on_primary),
                CornerRadius(theme.shapes.large),
                Scale(transform_scale.Get()),
                Rotation(transform_rotation.Get()),
                TransformGesture{}
            )
            .On<TransformEvents::Started>([transform_pointers](const TransformEvent& event) {
              transform_pointers = event.pointer_count;
            })
            .On<TransformEvents::Changed>(apply_transform)
            .On<TransformEvents::Ended>([transform_pointers](const TransformEvent&) {
              transform_pointers = 0;
            })
            .On<TransformEvents::Canceled>([transform_pointers](const TransformEvent&) {
              transform_pointers = 0;
            }),
      }.With(
          Frame{.height = 260.0F},
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
