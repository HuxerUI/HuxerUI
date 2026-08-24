#include <huxerui/huxerui.h>

#include <string>
#include <utility>

using namespace huxerui;

struct GreetingLocale {
  std::string name;
  std::string greeting;

  static GreetingLocale Default() {
    return {"Default", "Hello"};
  }

  bool operator==(const GreetingLocale&) const = default;
};

[[huxerui::composable]]
View LocaleCard(std::string title) {
  const auto& locale = UseEnvironment<GreetingLocale>();
  const auto& theme = UseTheme();
  return Column {
    Text(title, TextRole::Label),
    Text(locale.name + ": " + locale.greeting, TextRole::Title),
  }.With(
      Padding(theme.spacing.medium),
      Spacing(theme.spacing.small),
      Background(theme.colors.surface),
      CornerRadius(theme.shapes.medium)
  );
}

[[huxerui::composable]]
View FrenchContent() {
  return LocaleCard("Nested provider overrides the value");
}

[[huxerui::composable]]
View ProvidedContent() {
  return Column {
    LocaleCard("Inherited from the nearest provider"),
    ProvideEnvironment(GreetingLocale {"French", "Bonjour"}, FrenchContent()),
  }.With(Spacing(UseTheme().spacing.medium));
}

[[huxerui::composable]]
View EnvironmentContent() {
  auto use_chinese = UseState(true);
  const ThemeSpec& theme = UseTheme();
  GreetingLocale locale = use_chinese
      ? GreetingLocale {"Chinese", "你好"}
      : GreetingLocale {"English", "Hello"};

  return Column {
    Text("Environment", TextRole::Title),
    Text(
        "A provider supplies a typed value to its subtree. "
        "The closest provider wins."
    ),
    LocaleCard("No provider uses GreetingLocale::Default()"),
    ProvideEnvironment(std::move(locale), ProvidedContent()),
    Button("Toggle outer locale").OnClick([use_chinese] {
      use_chinese = !use_chinese;
    }),
  }.With(
      Padding(theme.spacing.extra_large),
      Spacing(theme.spacing.medium),
      Background(theme.colors.background),
      CrossAlign(CrossAxisAlignment::Stretch)
  );
}

View App() {
  return MaterialTheme {EnvironmentContent()};
}

const Application application{
    App,
    {
        .window = {
            .title = "HuxerUI Environment",
            .initial_size = {620.0F, 700.0F},
        },
    }
};
