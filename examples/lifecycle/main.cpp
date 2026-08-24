#include <huxerui/huxerui.h>

#include <string>
#include <utility>

using namespace huxerui;

[[huxerui::composable]]
View ChannelConnection(std::string channel, StateList<std::string> events) {
  Lifecycle([channel, events] {
    events.PushBack("Setup: connect to " + channel);
    return [channel, events]() noexcept {
      events.PushBack("Cleanup: disconnect from " + channel);
    };
  }, channel);

  const ThemeSpec& theme = UseTheme();
  return Column {
    Text("Connected", TextRole::Label).With(Foreground(theme.colors.primary)),
    Text(channel, TextRole::Title),
  }.With(
      Padding(theme.spacing.large),
      Spacing(theme.spacing.small),
      Background(theme.colors.surface_container_low),
      CornerRadius(theme.shapes.medium)
  );
}

[[huxerui::composable]]
View LifecycleContent() {
  auto channel = UseState<std::string>("Design");
  auto mounted = UseState(true);
  auto events = UseStateList<std::string>();
  const ThemeSpec& theme = UseTheme();

  View connection;
  if (mounted.Get()) {
    connection = ChannelConnection(channel.Get(), events);
  } else {
    connection = Text("The connection component is unmounted.").With(
        Padding(theme.spacing.large),
        Foreground(theme.colors.on_surface_variant),
        Background(theme.colors.surface_container_low),
        CornerRadius(theme.shapes.medium)
    );
  }

  return ScrollView {
    Column {
      Text("Lifecycle", TextRole::Title),
      Text(
          "Lifecycle ties an external resource to a component scope. Changing a dependency restarts it, "
          "and unmounting the component runs its cleanup."
      ),
      Row {
        Button("Design").OnClick([channel] { channel = "Design"; }),
        Button("Runtime").OnClick([channel] { channel = "Runtime"; }),
        Button(mounted.Get() ? "Unmount" : "Mount").OnClick([mounted] { mounted = !mounted.Get(); }),
      }.With(
          Spacing(theme.spacing.medium),
          CrossAlign(CrossAxisAlignment::Center)
      ),
      std::move(connection),
      Row {
        Text("Committed lifecycle events", TextRole::Title),
        Spacer(),
        Button("Clear").OnClick([events] { events.Clear(); }),
      }.With(CrossAlign(CrossAxisAlignment::Center)),
      Column {
        ForEach(events, [](const std::string& event) { return Text(event); }),
      }.With(
          Padding(theme.spacing.large),
          Spacing(theme.spacing.small),
          Background(theme.colors.surface_container_low),
          CornerRadius(theme.shapes.medium)
      ),
    }.With(
        Padding(theme.spacing.extra_large),
        Spacing(theme.spacing.medium),
        CrossAlign(CrossAxisAlignment::Stretch)
    ),
  }.With(Background(theme.colors.background));
}

View App() {
  return MaterialTheme {
    LifecycleContent(),
  };
}

const Application application{
    App,
    {
        .window = {
            .title = "HuxerUI Lifecycle",
            .initial_size = {720.0F, 620.0F},
        },
    }
};
