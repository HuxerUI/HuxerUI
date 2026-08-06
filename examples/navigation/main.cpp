#include <huxerui/huxerui.h>

using namespace huxerui;

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

View App() {
  return MaterialTheme([] { return NavigationStack(HomePage); });
}

HUXERUI_APP(
    App,
    {
        .title = "HuxerUI Navigation",
        .width = 640.0F,
        .height = 480.0F,
    }
)
