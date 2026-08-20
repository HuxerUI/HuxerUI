#include <huxerui/huxerui.h>

#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <huxerui/web/navigation.h>
#endif

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

View RoutedRootContent(std::optional<NavigationController> factory_navigation) {
  const ThemeSpec& theme = UseTheme();
  const RouteNavigationController<DemoRoute> route_navigation = UseNavigation<DemoRoute>();
  Views actions;
  if (factory_navigation) {
    const NavigationController navigation = *factory_navigation;
    actions.Add(Button("Back to factory navigation").OnClick([navigation] { navigation.Pop(); }));
  }
  actions.Add(Button("Push shelf 7").OnClick([route_navigation] { route_navigation.Push(ShelfRoute{7}); }));
  actions.Add(Button("Open deep path").OnClick([route_navigation] {
    route_navigation.SetPath(NavigationPath<DemoRoute>{{ShelfRoute{7}, ArticleRoute{42}}});
  }));
  return Column {
    Text("Typed routed navigation", TextRole::Title),
    Text("NavigationPath is application-owned history suitable for deep links and restoration."),
    Text::Format("Routed depth: {}", route_navigation.Depth()),
    Flow {std::move(actions)}.With(Spacing(theme.spacing.small)),
  }.With(
      Padding(theme.spacing.extra_large),
      Spacing(theme.spacing.medium),
      Background(theme.colors.background)
  );
}

View StandaloneRoutedRootPage() {
  return RoutedRootContent(std::nullopt);
}

View NestedRoutedRootPage() {
  return RoutedRootContent(UseRootNavigation());
}

#ifdef __EMSCRIPTEN__

struct DemoRouteCodec {
  std::optional<NavigationPath<DemoRoute>> Decode(std::string_view location) const {
    const std::size_t fragment = location.find('#');
    if (fragment == std::string_view::npos) {
      return std::nullopt;
    }
    location.remove_prefix(fragment + 1);
    if (!location.starts_with("/navigation")) {
      return std::nullopt;
    }
    location.remove_prefix(std::string_view{"/navigation"}.size());

    std::vector<DemoRoute> routes;
    while (!location.empty()) {
      if (!location.starts_with('/')) {
        return std::nullopt;
      }
      location.remove_prefix(1);
      const std::size_t separator = location.find('/');
      const std::string_view kind = location.substr(0, separator);
      if (separator == std::string_view::npos) {
        return std::nullopt;
      }
      location.remove_prefix(separator + 1);
      const std::size_t value_end = location.find('/');
      const std::string_view value_text = location.substr(0, value_end);
      int value = 0;
      const auto [end, error] = std::from_chars(value_text.data(), value_text.data() + value_text.size(), value);
      if (error != std::errc{} || end != value_text.data() + value_text.size()) {
        return std::nullopt;
      }
      if (kind == "shelf") {
        routes.emplace_back(ShelfRoute{value});
      } else if (kind == "article") {
        routes.emplace_back(ArticleRoute{value});
      } else {
        return std::nullopt;
      }
      location = value_end == std::string_view::npos ? std::string_view{} : location.substr(value_end);
    }
    return NavigationPath<DemoRoute>(std::move(routes));
  }

  std::string Encode(const NavigationPath<DemoRoute>& path) const {
    std::string location = "#/navigation";
    for (const DemoRoute& route : path.Routes()) {
      std::visit(
          [&location](const auto& value) {
            using Route = std::decay_t<decltype(value)>;
            if constexpr (std::same_as<Route, ShelfRoute>) {
              location += "/shelf/" + std::to_string(value.shelf);
            } else {
              location += "/article/" + std::to_string(value.article);
            }
          },
          route
      );
    }
    return location;
  }
};

#endif

View RoutedNavigationContent(State<NavigationPath<DemoRoute>> path) {
#ifdef __EMSCRIPTEN__
  return web::BrowserNavigationStack(StandaloneRoutedRootPage, path, ResolveDemoRoute, DemoRouteCodec{});
#else
  return NavigationStack(NestedRoutedRootPage, path, ResolveDemoRoute);
#endif
}

[[huxerui::scope]] View RoutedNavigationDemo() {
  auto path = UseState(NavigationPath<DemoRoute>{});
  return RoutedNavigationContent(path);
}

View App() {
#ifdef __EMSCRIPTEN__
  return MaterialTheme(RoutedNavigationDemo);
#else
  return MaterialTheme(FactoryNavigationDemo);
#endif
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
