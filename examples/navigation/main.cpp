#include <huxerui/huxerui.h>

#include <cstddef>
#include <optional>
#include <vector>

using namespace huxerui;

VectorAsset HomeIcon() {
  static const VectorAsset icon = VectorAsset::Create({20.0F, 20.0F}, [](VectorBuilder& builder) {
    builder.StrokePath(
        Path{}.MoveTo({2.5F, 9.0F}).LineTo({10.0F, 2.5F}).LineTo({17.5F, 9.0F}),
        Color::Black(),
        2.0F,
        StrokeCap::Round,
        StrokeJoin::Round
    );
    builder.StrokePath(
        Path{}.MoveTo({5.0F, 8.0F}).LineTo({5.0F, 17.0F}).LineTo({15.0F, 17.0F}).LineTo({15.0F, 8.0F}),
        Color::Black(),
        2.0F,
        StrokeCap::Round,
        StrokeJoin::Round
    );
  });
  return icon;
}

VectorAsset LibraryIcon() {
  static const VectorAsset icon = VectorAsset::Create({20.0F, 20.0F}, [](VectorBuilder& builder) {
    builder.FillPath(Path::RoundedRect({2.5F, 3.0F, 4.0F, 14.0F}, CornerRadii{1.5F}), Color::Black());
    builder.FillPath(Path::RoundedRect({8.0F, 3.0F, 4.0F, 14.0F}, CornerRadii{1.5F}), Color::Black());
    builder.FillPath(Path::RoundedRect({13.5F, 3.0F, 4.0F, 14.0F}, CornerRadii{1.5F}), Color::Black());
  });
  return icon;
}

VectorAsset SettingsIcon() {
  static const VectorAsset icon = VectorAsset::Create({20.0F, 20.0F}, [](VectorBuilder& builder) {
    builder.StrokePath(
        Path{}
            .MoveTo({2.0F, 5.0F})
            .LineTo({18.0F, 5.0F})
            .MoveTo({2.0F, 10.0F})
            .LineTo({18.0F, 10.0F})
            .MoveTo({2.0F, 15.0F})
            .LineTo({18.0F, 15.0F}),
        Color::Black(),
        2.0F,
        StrokeCap::Round
    );
    builder.FillPath(Path::RoundedRect({5.0F, 3.0F, 4.0F, 4.0F}, CornerRadii{2.0F}), Color::Black());
    builder.FillPath(Path::RoundedRect({12.0F, 8.0F, 4.0F, 4.0F}, CornerRadii{2.0F}), Color::Black());
    builder.FillPath(Path::RoundedRect({7.0F, 13.0F, 4.0F, 4.0F}, CornerRadii{2.0F}), Color::Black());
  });
  return icon;
}

VectorAsset MenuIcon() {
  static const VectorAsset icon = VectorAsset::Create({20.0F, 20.0F}, [](VectorBuilder& builder) {
    for (float y : {5.0F, 10.0F, 15.0F}) {
      builder.StrokePath(
          Path{}.MoveTo({2.0F, y}).LineTo({18.0F, y}),
          Color::Black(),
          2.0F,
          StrokeCap::Round
      );
    }
  });
  return icon;
}

View DetailsPage(int section);

View HomePage() {
  const ThemeSpec& theme = UseTheme();
  const NavigationController navigation = UseNavigation();
  return Column {
    Text("Navigation", TextRole::Title),
    Text("Pages retain their local state while another page covers them."),
    Button("Open details").OnClick([navigation] { navigation.Push(DetailsPage, 2); }),
  }.With(
      Padding(theme.spacing.extra_large),
      Spacing(theme.spacing.medium),
      Background(theme.colors.background)
  );
}

View FinalPage() {
  const ThemeSpec& theme = UseTheme();
  const NavigationController navigation = UseNavigation();
  return Column {
    Text("Final page", TextRole::Title),
    Text::Format("Stack depth: {}", navigation.Depth()),
    Button("Back to details").OnClick([navigation] { navigation.Pop(); }),
    Button("Replace this page").OnClick([navigation] {
      navigation.Replace([] {
        const ThemeSpec& replaced_theme = UseTheme();
        const NavigationController replaced_navigation = UseNavigation();
        return Column {
          Text("Replacement page", TextRole::Title),
          Button("Back home").OnClick([replaced_navigation] { replaced_navigation.Pop(); }),
        }.With(
            Padding(replaced_theme.spacing.extra_large),
            Spacing(replaced_theme.spacing.medium),
            Background(replaced_theme.colors.background)
        );
      });
    }),
  }.With(
      Padding(theme.spacing.extra_large),
      Spacing(theme.spacing.medium),
      Background(theme.colors.background)
  );
}

View DetailsPage(int section) {
  const ThemeSpec& theme = UseTheme();
  const NavigationController navigation = UseNavigation();
  return Column {
    Text("Details", TextRole::Title),
    Text::Format("Section: {}", section),
    Text::Format("Stack depth: {}", navigation.Depth()),
    Row {
      Button("Back").OnClick([navigation] { navigation.Pop(); }),
      Button("Continue").OnClick([navigation] { navigation.Push(FinalPage); }),
    }.With(Spacing(theme.spacing.small)),
  }.With(
      Padding(theme.spacing.extra_large),
      Spacing(theme.spacing.medium),
      Background(theme.colors.background)
  );
}

View LibraryPage() {
  const ThemeSpec& theme = UseTheme();
  return Column {
    Text("Library", TextRole::Title),
    Text("Destination selection is controlled independently from page history."),
  }.With(
      Padding(theme.spacing.extra_large),
      Spacing(theme.spacing.medium),
      Background(theme.colors.background)
  );
}

View SettingsPage() {
  const ThemeSpec& theme = UseTheme();
  return Column {
    Text("Settings", TextRole::Title),
    Text("Applications decide which content a NavigationBar or NavigationPane selects."),
  }.With(
      Padding(theme.spacing.extra_large),
      Spacing(theme.spacing.medium),
      Background(theme.colors.background)
  );
}

View DestinationContent(std::size_t destination) {
  switch (destination) {
  case 1:
    return LibraryPage();
  case 2:
    return SettingsPage();
  default:
    return NavigationStack(HomePage);
  }
}

View NavigationDemo() {
  const ThemeSpec& theme = UseTheme();
  const ViewportClass viewport_class = UseViewportClass();
  auto destination = UseState<std::size_t>(0);
  auto start_open = UseState(false);
  auto end_open = UseState(false);

  const auto destinations = [] {
    return std::vector<NavigationItem>{
        NavigationItem(HomeIcon(), "Home"),
        NavigationItem(LibraryIcon(), "Library"),
        NavigationItem(SettingsIcon(), "Settings"),
    };
  };

  std::optional<View> leading;
  if (viewport_class == ViewportClass::Compact) {
    leading = IconButton(MenuIcon(), "Open navigation")
                  .OnClick([start_open, end_open] {
                    end_open = false;
                    start_open = true;
                  });
  }
  std::vector<View> actions;
  if (viewport_class != ViewportClass::Expanded) {
    actions.push_back(IconButton(SettingsIcon(), "Open tools").OnClick([start_open, end_open, viewport_class] {
      if (viewport_class == ViewportClass::Compact) {
        start_open = false;
      }
      end_open = true;
    }));
  }

  return DrawerLayout {
    Column {
      TopAppBar("Navigation", std::move(leading), std::move(actions)),
      DestinationContent(destination.Get()).With(Grow()),
      NavigationBar(destinations(), destination).OnChanged([destination](std::size_t index) {
        destination = index;
      }),
    }.With(Background(theme.colors.background), CrossAlign(CrossAxisAlignment::Stretch)),

    StartDrawer {
      Column {
        Text("Destinations", TextRole::Title).With(Padding(theme.spacing.medium)),
        NavigationPane(destinations(), destination, true)
            .OnChanged([destination, start_open](std::size_t index) {
              destination = index;
              start_open = false;
            }),
      }.With(CrossAlign(CrossAxisAlignment::Stretch))
    }.Open(start_open).OnOpenChanged([start_open](bool open) { start_open = open; }),

    EndDrawer {
      Column {
        Text("Tools", TextRole::Title),
        Text("EndDrawer owns app content and follows the same controlled open state."),
        Button("Close").OnClick([end_open] { end_open = false; }),
      }.With(Padding(theme.spacing.large), Spacing(theme.spacing.medium))
    }.Open(end_open).OnOpenChanged([end_open](bool open) { end_open = open; }),
  };
}

View App() {
  return MaterialTheme(NavigationDemo);
}

HUXERUI_APP(
    App,
    {
        .title = "HuxerUI Navigation",
        .width = 640.0F,
        .height = 480.0F,
    }
)
