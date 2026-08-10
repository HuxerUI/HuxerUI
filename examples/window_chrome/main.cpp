#include <huxerui/huxerui.h>

using namespace huxerui;

View WindowChromeDemo() {
  const ThemeSpec& theme = UseTheme();
  auto toast = UseToast();
  return Column {
    WindowTitleBar {
      Text("HuxerUI", TextRole::Label).With(Foreground(theme.colors.on_primary)),
      Text("samples / window_chrome.cpp", TextRole::Label)
          .With(Foreground(theme.colors.on_primary), Grow(1.0F)),
      Button("Run").OnClick([toast] { toast.Show("Run action"); }).With(Frame{.height = 30.0F}),
      Button("Account").OnClick([toast] { toast.Show("Account action"); }).With(Frame{.height = 30.0F}),
    }.With(
        Padding(EdgeInsets::Symmetric(theme.spacing.medium, theme.spacing.extra_small)),
        Spacing(theme.spacing.small),
        Background(theme.colors.primary)
    ),
    Row {
      Column {
        Text("Workspace", TextRole::Title),
        Text("examples", TextRole::Label).With(Foreground(theme.colors.on_surface_variant)),
        Text("  window_chrome", TextRole::Label),
        Text("    main.cpp", TextRole::Label).With(Foreground(theme.colors.primary)),
        Text("    CMakeLists.txt", TextRole::Label),
        Spacer().With(Grow(1.0F)),
        Text("The entire top edge belongs to application UI.", TextRole::Label)
            .With(Foreground(theme.colors.on_surface_variant)),
      }.With(
          Frame{.width = 220.0F},
          Padding(theme.spacing.large),
          Spacing(theme.spacing.small),
          Background(theme.colors.surface_container_low)
      ),
      Column {
        Row {
          Text("window_chrome.cpp", TextRole::Label),
          Text("Custom chrome", TextRole::Label)
              .With(Foreground(theme.colors.on_surface_variant), Grow(1.0F)),
        }.With(Spacing(theme.spacing.medium)),
        Divider(),
        Column {
          Text("Application-defined window chrome", TextRole::Title),
          Text("Title-bar content, workspace actions, and the main surface now form one continuous application shell."),
          Button("Interactive controls remain client content")
              .OnClick([toast] { toast.Show("Client interaction received"); }),
        }.With(Padding(theme.spacing.extra_large), Spacing(theme.spacing.medium)),
      }.With(Grow(1.0F), Background(theme.colors.surface)),
    }.With(Grow(1.0F)),
  }.With(
      CrossAlign(CrossAxisAlignment::Stretch),
      Background(theme.colors.background)
  );
}

View App() {
  return MaterialTheme(WindowChromeDemo);
}

HUXERUI_APP(
    App,
    AppOptions {
        .window = {
            .title = "HuxerUI Window Chrome",
            .initial_size = {760.0F, 480.0F},
            .chrome_mode = WindowChromeMode::Custom,
            .title_bar_height = 48.0F,
        },
    }
)
