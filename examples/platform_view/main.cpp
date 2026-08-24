#include "platform_text_field.h"

#include <string>
#include <utility>

#include <huxerui/huxerui.h>

using namespace huxerui;

#if defined(__ANDROID__)
constexpr float platform_text_field_height = 48.0F;
#else
constexpr float platform_text_field_height = 28.0F;
#endif

[[huxerui::scope]]
View PlatformViewDemo() {
  auto value = UseState<std::string>("Editable PlatformView text");
  auto visible = UseState(true);
  const ThemeSpec& theme = UseTheme();

  View platform_content;
  if (visible.Get()) {
    platform_content = Stack {
      example::PlatformTextField(value.Get())
          .On<example::PlatformTextFieldEvents::Changed>([value](std::string next) { value = std::move(next); })
          .With(Frame{.height = platform_text_field_height}),
      Stack {
        Button("HuxerUI overlay").OnClick([value] { value = "Updated from overlay"; }),
      }.With(
          Frame{.height = 64.0F},
          Align(HorizontalAlignment::End, VerticalAlignment::Start)
      ),
    }.With(
        Frame{.height = 64.0F},
        Align(HorizontalAlignment::Stretch, VerticalAlignment::Center),
        Background(theme.colors.surface_container_low)
    );
  } else {
    platform_content = Text("The PlatformView is unmounted.").With(
        Frame{.height = 64.0F},
        Align(HorizontalAlignment::Center, VerticalAlignment::Center),
        Foreground(theme.colors.on_surface_variant),
        Background(theme.colors.surface_container_low)
    );
  }

  return Column {
    Text("PlatformView", TextRole::Title),
    Text("The PlatformTextField participates in HuxerUI layout, rendering order, state, and events."),
    std::move(platform_content),
    Text("Controlled value: " + value.Get(), TextRole::Label),
    Column {
      Button("Set from HuxerUI").OnClick([value] { value = "Updated by HuxerUI"; }),
      Button(visible.Get() ? "Unmount PlatformView" : "Mount PlatformView").OnClick([visible] {
        visible = !visible.Get();
      }),
    }.With(
        Spacing(theme.spacing.medium),
        CrossAlign(CrossAxisAlignment::Stretch)
    ),
  }.With(
      Padding(theme.spacing.extra_large),
      Spacing(theme.spacing.medium),
      CrossAlign(CrossAxisAlignment::Stretch),
      Background(theme.colors.background)
  );
}

View App() {
  return MaterialTheme {PlatformViewDemo()};
}

const Application application{
    App,
    {
        .window = {
            .title = "HuxerUI PlatformView",
            .initial_size = {720.0F, 440.0F},
        },
        .root_hooks = {example::InstallPlatformTextField},
    }
};
