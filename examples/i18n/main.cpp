#include <huxerui/huxerui.h>

#include <i18n_resources.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <utility>

using namespace huxerui;

constexpr std::array locale_tags{"en-US", "ar-EG", "he-IL"};

View ExampleSection(StringVariant title, StringVariant description, View content, const ThemeSpec& theme) {
  return Column {
    Text(std::move(title), TextRole::Title),
    Text(std::move(description)).With(Foreground(theme.colors.on_surface_variant)),
    std::move(content),
  }.With(
      Padding(theme.spacing.large),
      Spacing(theme.spacing.medium),
      Background(theme.colors.surface_container_low),
      CornerRadius(theme.shapes.large),
      CrossAlign(CrossAxisAlignment::Stretch)
  );
}

[[huxerui::composable]]
View LocalizedContent(State<std::size_t> selected_locale) {
  const ThemeSpec& theme = UseTheme();
  auto name = UseState(TextEditingValue::FromText(""));
  const std::string canvas_text = UseString(i18n::strings::canvas_text);
  const TextStyle canvas_style{Font::System(theme.typography.body_large), theme.colors.on_surface};
  const float canvas_padding = theme.spacing.large;
  const std::size_t selected_index = selected_locale.Get();

  return ScrollView {
    Column {
      Column {
        Text(i18n::strings::eyebrow, TextRole::Label).With(Foreground(theme.colors.primary)),
        Text(i18n::strings::title, TextRole::Title),
        Text(i18n::strings::introduction).With(Foreground(theme.colors.on_surface_variant)),
      }.With(Spacing(theme.spacing.extra_small), CrossAlign(CrossAxisAlignment::Stretch)),
      SegmentedButton(
          {i18n::strings::language_english, i18n::strings::language_arabic, i18n::strings::language_hebrew},
          selected_index
      )
          .OnChanged([selected_locale](std::size_t index) { selected_locale = index; }),
      Text::Format(i18n::strings::active_locale, locale_tags[selected_index])
          .With(Foreground(theme.colors.primary)),
      ExampleSection(
          i18n::strings::inherited_title,
          i18n::strings::inherited_body,
          Column {
            Text(i18n::strings::mixed_text, TextRole::Title),
            Button(i18n::strings::action).OnClick([] {}),
            TextField(name)
                .Label(i18n::strings::field_label)
                .Placeholder(i18n::strings::field_placeholder)
                .Validation(Validate(name->text, Required(i18n::strings::field_validation)))
                .OnChanged([name](const TextEditingValue& value) { name = value; }),
          }.With(Spacing(theme.spacing.medium), CrossAlign(CrossAxisAlignment::Stretch)),
          theme
      ),
      ExampleSection(
          i18n::strings::canvas_title,
          i18n::strings::canvas_body,
          Canvas([canvas_padding, canvas_text, canvas_style](PaintContext& context, Size size) {
            context.DrawText(
                {canvas_padding, 0.0F, std::max(0.0F, size.width - canvas_padding * 2.0F), size.height},
                canvas_text,
                canvas_style,
                TextLayoutOptions{
                    .align = TextAlign::Leading,
                    .vertical_align = TextVerticalAlign::Center,
                    .wrap = TextWrap::Word,
                }
            );
          }).With(Frame{.height = 104.0F}, Background(theme.colors.surface), CornerRadius(theme.shapes.medium)),
          theme
      ),
      ExampleSection(
          i18n::strings::explicit_title,
          i18n::strings::explicit_body,
          Text(i18n::strings::explicit_text)
              .Shaping({.direction = TextDirection::RightToLeft, .locale = "fa-IR"})
              .With(Foreground(theme.colors.primary)),
          theme
      ),
      ExampleSection(
          i18n::strings::boundary_title,
          i18n::strings::boundary_body,
          Text(i18n::strings::boundary_note, TextRole::Label)
              .With(Foreground(theme.colors.error)),
          theme
      ),
    }.With(
        Padding(theme.spacing.extra_large),
        Spacing(theme.spacing.large),
        Background(theme.colors.background),
        CrossAlign(CrossAxisAlignment::Stretch)
    ),
  }.With(ScrollBar());
}

[[huxerui::composable]]
View I18nExample() {
  auto selected_locale = UseState<std::size_t>(0);
  const std::size_t selected_index = selected_locale.Get();
  return ProvideEnvironment(
      Locale::FromLanguageTag(locale_tags[selected_index]),
      Scope([selected_locale] { return LocalizedContent(selected_locale); })
  );
}

View App() {
  return MaterialTheme {I18nExample()};
}

const Application application{
    App,
    {
        .window = {
            .title = "HuxerUI i18n",
            .initial_size = {760.0F, 820.0F},
        },
    }
};
