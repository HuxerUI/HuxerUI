#include "native_text_field.h"

#include <string>
#include <utility>

#include <huxerui/huxerui.h>

using namespace huxerui;

[[huxerui::scope]]
View PlatformViewDemo() {
  auto value = UseState<std::string>("Editable native text");
  auto visible = UseState(true);
  const ThemeSpec& theme = UseTheme();

  View native_content;
  if (visible.Get()) {
    native_content = Stack {
      example::NativeTextField(value.Get())
          .On<example::NativeTextFieldEvents::Changed>([value](std::string next) { value = std::move(next); })
          .With(Frame{.height = 28.0F}),
      Stack {
        Text("HuxerUI overlay", TextRole::Label).With(
            Padding(EdgeInsets::Symmetric(theme.spacing.small, theme.spacing.extra_small)),
            Foreground(theme.colors.on_primary),
            Background(theme.colors.primary)
        ),
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
    native_content = Text("The native view is unmounted.").With(
        Frame{.height = 64.0F},
        Align(HorizontalAlignment::Center, VerticalAlignment::Center),
        Foreground(theme.colors.on_surface_variant),
        Background(theme.colors.surface_container_low)
    );
  }

  return Column {
    Text("PlatformView", TextRole::Title),
    Text("The AppKit text field participates in HuxerUI layout, rendering order, state, and events."),
    std::move(native_content),
    Text("Controlled value: " + value.Get(), TextRole::Label),
    Row {
      Button("Set from HuxerUI").OnClick([value] { value = "Updated by HuxerUI"; }),
      Button(visible.Get() ? "Unmount native view" : "Mount native view").OnClick([visible] {
        visible = !visible.Get();
      }),
    }.With(Spacing(theme.spacing.medium)),
  }.With(
      Padding(theme.spacing.extra_large),
      Spacing(theme.spacing.medium),
      CrossAlign(CrossAxisAlignment::Stretch),
      Background(theme.colors.background)
  );
}

View App() {
  return MaterialTheme(PlatformViewDemo);
}

HUXERUI_APP(
    App,
    {
        .window = {
            .title = "HuxerUI PlatformView",
            .initial_size = {720.0F, 440.0F},
        },
        .root_hooks = {example::InstallNativeTextField()},
    }
)
