#include <huxerui/huxerui.h>

using namespace huxerui;

const char* ViewportClassName(ViewportClass viewport_class) {
  switch (viewport_class) {
  case ViewportClass::Compact:
    return "Compact";
  case ViewportClass::Medium:
    return "Medium";
  case ViewportClass::Expanded:
    return "Expanded";
  }
  return "Unknown";
}

[[huxerui::scope]]
View TabsDemo() {
  const ThemeSpec& theme = UseTheme();
  const ViewportClass viewport_class = UseViewportClass();
  auto selected = UseState<std::size_t>(0);
  auto flat_selected = UseState<std::size_t>(1);
  const std::vector<const char*> pages{"Overview", "Activity", "Reports", "Settings"};

  return Column {
    Text("Tabs", TextRole::Title),
    Text::Format("Viewport class: {}", ViewportClassName(viewport_class)),
    Tabs(
        std::vector<TabItem>{
            TabItem("Overview"),
            TabItem("Activity"),
            std::move(TabItem("Reports")).Enabled(false),
            TabItem("Settings"),
        },
        selected
    ).OnChanged([selected](std::size_t index) { selected = index; }),
    Text::Format("Selected page: {}", pages[selected.Get()]),
    HUXERUI_THEME(
        FlatTheme,
        Column {
          Text("Flat tabs", TextRole::Label),
          Tabs({"Files", "Search", "History"}, flat_selected)
              .OnChanged([flat_selected](std::size_t index) { flat_selected = index; }),
        }.With(
            Padding(theme.spacing.medium),
            Spacing(theme.spacing.small),
            Background(UseTheme().colors.background)
        )
    ),
  }.With(
      Padding(theme.spacing.extra_large),
      Spacing(theme.spacing.medium),
      Background(theme.colors.background)
  );
}

View App() {
  return MaterialTheme(TabsDemo);
}

HUXERUI_APP(
    App,
    {
        .title = "HuxerUI Tabs",
        .width = 640.0F,
        .height = 480.0F,
    }
)
