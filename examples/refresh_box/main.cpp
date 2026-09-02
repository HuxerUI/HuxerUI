#include <huxerui/huxerui.h>

#include <array>
#include <cstddef>
#include <string>

using namespace huxerui;

namespace {

[[huxerui::composable]]
View RefreshBoxDemo() {
  static constexpr std::array<std::size_t, 8> activity_indices{0, 1, 2, 3, 4, 5, 6, 7};
  const ThemeSpec& theme = UseTheme();
  const bool compact = UseViewportClass() == ViewportClass::Compact;
  auto tasks = UseTaskScope();
  auto refreshing = UseState(false);
  auto refresh_count = UseState<std::size_t>(0);
  auto status = UseState(std::string{"Pull the feed down to refresh"});

  return Column {
    Column {
      Text("RefreshBox", TextRole::Title),
      Text("A controlled refresh gesture built on shared nested scrolling and overscroll.")
          .With(Foreground(theme.colors.on_surface_variant)),
    }.With(Spacing(theme.spacing.extra_small)),
    RefreshBox(
        ScrollView {
          Column {
            Row {
              Column {
                Text(status.Get()),
                Text::Format("Completed refreshes: {}", refresh_count.Get())
                    .With(Foreground(theme.colors.on_surface_variant)),
              }.With(Spacing(theme.spacing.extra_small)),
              Spacer(),
              Text(refreshing.Get() ? "UPDATING" : "READY", TextRole::Label)
                  .With(Foreground(theme.colors.primary)),
            }.With(
                Padding(compact ? theme.spacing.medium : theme.spacing.large),
                Background(theme.colors.surface_container_high),
                CornerRadius(theme.shapes.large),
                CrossAlign(CrossAxisAlignment::Center)
            ),
            Column {
              ForEach(activity_indices, [&](std::size_t index) {
                return Row {
                  Stack {
                    Text(std::to_string(index + 1), TextRole::Label)
                        .With(Foreground(theme.colors.on_secondary_container)),
                  }.With(
                      Frame{compact ? 36.0F : 40.0F, compact ? 36.0F : 40.0F},
                      Background(theme.colors.secondary_container),
                      CornerRadius(compact ? 18.0F : 20.0F),
                      Align(HorizontalAlignment::Center, VerticalAlignment::Center)
                  ),
                  Column {
                    Text::Format("Activity {}", index + 1),
                    Text(index == 0 ? "Latest content appears here after refresh."
                                    : "Retained content keeps its identity while displaced.")
                        .With(Foreground(theme.colors.on_surface_variant)),
                  }.With(Spacing(theme.spacing.extra_small), Grow()),
                }.With(
                    Padding(compact ? theme.spacing.small : theme.spacing.medium),
                    Spacing(theme.spacing.medium),
                    CrossAlign(CrossAxisAlignment::Center)
                );
              }),
            }.With(
                Padding(theme.spacing.extra_small),
                Spacing(theme.spacing.extra_small),
                Background(theme.colors.surface),
                CornerRadius(theme.shapes.large)
            ),
          }.With(
              Padding(theme.spacing.medium),
              Spacing(theme.spacing.medium),
              CrossAlign(CrossAxisAlignment::Stretch)
          ),
        }.With(ScrollBar()),
        refreshing
    )
        .OnRefresh([=] {
          refreshing = true;
          status = "Refreshing activity";
          tasks.Launch([=]() -> Task<void> {
            co_await Delay(900ms);
            refresh_count += 1;
            status = "Feed refreshed";
            refreshing = false;
          });
        })
        .With(
            Frame{.height = compact ? 520.0F : 560.0F},
            Background(theme.colors.surface_container_low),
            CornerRadius(theme.shapes.large)
        ),
  }.With(
      Padding(compact ? theme.spacing.medium : theme.spacing.extra_large),
      Spacing(theme.spacing.large),
      CrossAlign(CrossAxisAlignment::Stretch),
      Background(theme.colors.background)
  );
}

} // namespace

View App() {
  return MaterialTheme {
    RefreshBoxDemo(),
  };
}

const Application application{
    App,
    {
        .window = {
            .title = "HuxerUI RefreshBox",
            .initial_size = {720.0F, 760.0F},
        },
    }
};
