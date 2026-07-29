#include <huxerui/huxerui.h>

#include <string>
#include <utility>

using namespace huxerui;

struct Locale {
  std::string name;
  std::string greeting;
};

struct LocaleKey {
  using Value = Locale;

  static Value Default() {
    return {"Default", "Hello"};
  }
};

View LocaleCard(std::string title) {
  const auto& locale = UseEnvironment<LocaleKey>();
  const auto& theme = UseTheme();
  return Column {
    Text(std::move(title), TextRole::Label),
    Text(locale.name + ": " + locale.greeting, TextRole::Title),
  }.With(
      Padding(theme.spacing.medium),
      Spacing(theme.spacing.small),
      Background(theme.colors.surface),
      CornerRadius(theme.shapes.medium)
  );
}

View FrenchContent() {
  return LocaleCard("Nested provider overrides the value");
}

View ProvidedContent() {
  return Column {
    LocaleCard("Inherited from the nearest provider"),
    ProvideEnvironment<LocaleKey>(Locale {"French", "Bonjour"}, FrenchContent),
  }.With(Spacing(UseTheme().spacing.medium));
}

[[huxerui::scope]]
View EnvironmentContent() {
  auto use_chinese = UseState(true);
  const ThemeSpec& theme = UseTheme();
  Locale locale = use_chinese
      ? Locale {"Chinese", "你好"}
      : Locale {"English", "Hello"};

  return Column {
    Text("Environment", TextRole::Title),
    Text(
        "A provider supplies a typed value to its subtree. "
        "The closest provider wins."
    ),
    LocaleCard("No provider uses LocaleKey::Default()"),
    ProvideEnvironment<LocaleKey>(std::move(locale), ProvidedContent),
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
  return MaterialTheme(EnvironmentContent);
}

HUXERUI_APP(
    App,
    {
        .title = "HuxerUI Environment",
        .width = 620.0F,
        .height = 700.0F,
    }
)
