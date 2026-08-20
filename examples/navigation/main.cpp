#include <huxerui/huxerui.h>

#include <variant>

using namespace huxerui;

View RoutedNavigationDemo();
View FactoryDetailsPage(int section);
View FactoryFinalPage();
View FactoryReplacementPage();

View FactoryRootPage() {
  const ThemeSpec& theme = UseTheme();
  const NavigationController navigation = UseNavigation();
  return Column {
    Text("Factory navigation", TextRole::Title),
    Text("Page factories keep arbitrary arguments and mounted state inside a local navigation flow."),
    Text::Format("Stack depth: {}", navigation.Depth()),
    Flow {
      Button("Open details").OnClick([navigation] { navigation.Push(FactoryDetailsPage, 2); }),
      Button("Open routed navigation").OnClick([navigation] { navigation.Push(RoutedNavigationDemo); }),
    }.With(Spacing(theme.spacing.small)),
  }.With(
      Padding(theme.spacing.extra_large),
      Spacing(theme.spacing.medium),
      Background(theme.colors.background)
  );
}

View FactoryDetailsPage(int section) {
  const ThemeSpec& theme = UseTheme();
  const NavigationController navigation = UseNavigation();
  return Column {
    Text("Factory details", TextRole::Title),
    Text::Format("Section: {}", section),
    Text::Format("Stack depth: {}", navigation.Depth()),
    Flow {
      Button("Back").OnClick([navigation] { navigation.Pop(); }),
      Button("Continue").OnClick([navigation] { navigation.Push(FactoryFinalPage); }),
    }.With(Spacing(theme.spacing.small)),
  }.With(
      Padding(theme.spacing.extra_large),
      Spacing(theme.spacing.medium),
      Background(theme.colors.background)
  );
}

View FactoryFinalPage() {
  const ThemeSpec& theme = UseTheme();
  const NavigationController navigation = UseNavigation();
  return Column {
    Text("Factory final page", TextRole::Title),
    Text::Format("Stack depth: {}", navigation.Depth()),
    Flow {
      Button("Back to details").OnClick([navigation] { navigation.Pop(); }),
      Button("Replace page").OnClick([navigation] { navigation.Replace(FactoryReplacementPage); }),
    }.With(Spacing(theme.spacing.small)),
  }.With(
      Padding(theme.spacing.extra_large),
      Spacing(theme.spacing.medium),
      Background(theme.colors.background)
  );
}

View FactoryReplacementPage() {
  const ThemeSpec& theme = UseTheme();
  const NavigationController navigation = UseNavigation();
  return Column {
    Text("Factory replacement", TextRole::Title),
    Text("Replace changes the top page without rebuilding the retained prefix."),
    Button("Back to details").OnClick([navigation] { navigation.Pop(); }),
  }.With(
      Padding(theme.spacing.extra_large),
      Spacing(theme.spacing.medium),
      Background(theme.colors.background)
  );
}

View FactoryNavigationDemo() {
  return NavigationStack(FactoryRootPage);
}

struct ShelfRoute {
  int shelf = 0;

  bool operator==(const ShelfRoute&) const = default;
};

struct ArticleRoute {
  int article = 0;

  bool operator==(const ArticleRoute&) const = default;
};

using DemoRoute = std::variant<ShelfRoute, ArticleRoute>;

View RoutedArticleNotesPage(int article) {
  const ThemeSpec& theme = UseTheme();
  const NavigationController local_navigation = UseNavigation();
  const RouteNavigationController<DemoRoute> route_navigation = UseRootNavigation<DemoRoute>();
  return Column {
    Text("Local article notes", TextRole::Title),
    Text::Format("Article {} owns this nested factory page.", article),
    Flow {
      Button("Back to article").OnClick([local_navigation] { local_navigation.Pop(); }),
      Button("Back to route root").OnClick([route_navigation] {
        route_navigation.SetPath(NavigationPath<DemoRoute>{});
      }),
    }.With(Spacing(theme.spacing.small)),
  }.With(
      Padding(theme.spacing.extra_large),
      Spacing(theme.spacing.medium),
      Background(theme.colors.background)
  );
}

View RoutedArticleOverviewPage(int article) {
  const ThemeSpec& theme = UseTheme();
  const NavigationController local_navigation = UseNavigation();
  const RouteNavigationController<DemoRoute> route_navigation = UseRootNavigation<DemoRoute>();
  return Column {
    Text::Format(TextRole::Title, "Article {}", article),
    Text::Format("Routed depth: {}", route_navigation.Depth()),
    Text("This route contains a nested factory stack while retaining access to the outer routed stack."),
    Flow {
      Button("Open local notes").OnClick([local_navigation, article] {
        local_navigation.Push(RoutedArticleNotesPage, article);
      }),
      Button("Back to shelf").OnClick([route_navigation] { route_navigation.Pop(); }),
    }.With(Spacing(theme.spacing.small)),
  }.With(
      Padding(theme.spacing.extra_large),
      Spacing(theme.spacing.medium),
      Background(theme.colors.background)
  );
}

View RoutedArticlePage(const ArticleRoute& route) {
  return NavigationStack(RoutedArticleOverviewPage, route.article);
}

View RoutedShelfPage(const ShelfRoute& route) {
  const ThemeSpec& theme = UseTheme();
  const RouteNavigationController<DemoRoute> navigation = UseNavigation<DemoRoute>();
  return Column {
    Text::Format(TextRole::Title, "Shelf {}", route.shelf),
    Text::Format("Routed depth: {}", navigation.Depth()),
    Text("Equal route prefixes retain their mounted page state."),
    Flow {
      Button("Back").OnClick([navigation] { navigation.Pop(); }),
      Button("Open article 42").OnClick([navigation] { navigation.Push(ArticleRoute{42}); }),
      Button("Replace shelf").OnClick([navigation, shelf = route.shelf] {
        navigation.Replace(ShelfRoute{shelf + 1});
      }),
    }.With(Spacing(theme.spacing.small)),
  }.With(
      Padding(theme.spacing.extra_large),
      Spacing(theme.spacing.medium),
      Background(theme.colors.background)
  );
}

View ResolveDemoRoute(const DemoRoute& route) {
  if (const auto* shelf = std::get_if<ShelfRoute>(&route)) {
    return RoutedShelfPage(*shelf);
  }
  return RoutedArticlePage(std::get<ArticleRoute>(route));
}

View RoutedRootPage() {
  const ThemeSpec& theme = UseTheme();
  const NavigationController factory_navigation = UseRootNavigation();
  const RouteNavigationController<DemoRoute> route_navigation = UseNavigation<DemoRoute>();
  return Column {
    Text("Typed routed navigation", TextRole::Title),
    Text("NavigationPath is application-owned history suitable for deep links and restoration."),
    Text::Format("Routed depth: {}", route_navigation.Depth()),
    Flow {
      Button("Back to factory navigation").OnClick([factory_navigation] { factory_navigation.Pop(); }),
      Button("Push shelf 7").OnClick([route_navigation] { route_navigation.Push(ShelfRoute{7}); }),
      Button("Open deep path").OnClick([route_navigation] {
        route_navigation.SetPath(NavigationPath<DemoRoute>{{ShelfRoute{7}, ArticleRoute{42}}});
      }),
    }.With(Spacing(theme.spacing.small)),
  }.With(
      Padding(theme.spacing.extra_large),
      Spacing(theme.spacing.medium),
      Background(theme.colors.background)
  );
}

[[huxerui::scope]] View RoutedNavigationDemo() {
  auto path = UseState(NavigationPath<DemoRoute>{});
  return NavigationStack(RoutedRootPage, path, ResolveDemoRoute);
}

View App() {
  return MaterialTheme(FactoryNavigationDemo);
}

const Application application{
    App,
    {
        .window = {
            .title = "HuxerUI Navigation",
            .initial_size = {640.0F, 480.0F},
        },
    }
};
