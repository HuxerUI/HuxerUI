#include "texture_demo.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <huxerui/huxerui.h>

using namespace huxerui;

namespace {

View TextureCard(const example::TextureDemoEntry& entry, const ThemeSpec& theme) {
  return Column {
    Stack {
      Image(entry.texture).Fit(ImageFit::Cover).With(Frame{.width = 320.0F, .height = 180.0F}),
      Text("HuxerUI overlay", TextRole::Label).With(
          Padding(theme.spacing.small), Foreground(theme.colors.inverse_on_surface), Background(theme.colors.scrim),
          CornerRadius(theme.shapes.small), Align(HorizontalAlignment::End, VerticalAlignment::Start)
      ),
    }.With(
        Frame{.width = 320.0F, .height = 180.0F}, Border{theme.colors.outline, 1.0F},
        CornerRadius(theme.shapes.large), ClipChildren()
    ),
    Text(entry.name, TextRole::Title),
    Text(entry.description).With(Foreground(theme.colors.on_surface_variant)),
  }.With(
      Frame{.width = 320.0F}, Spacing(theme.spacing.small), Padding(theme.spacing.medium),
      Background(theme.colors.surface_container_low), CornerRadius(theme.shapes.large)
  );
}

[[huxerui::composable]]
View ExternalTextureDemo() {
  const std::shared_ptr<example::TextureDemo> demo = example::UseTextureDemo();
  const ThemeSpec& theme = UseTheme();
  auto running = UseState(true);
  std::vector<View> cards;
  cards.reserve(demo->Entries().size());
  for (const example::TextureDemoEntry& entry : demo->Entries()) {
    cards.push_back(TextureCard(entry, theme).Key(entry.name));
  }

  View samples = Text(std::string(demo->Message())).With(
      Padding(theme.spacing.large), Foreground(theme.colors.on_surface_variant),
      Background(theme.colors.surface_container_low), CornerRadius(theme.shapes.large)
  );
  if (!cards.empty()) {
    samples = Flow {std::move(cards)}.With(Spacing(theme.spacing.medium), CrossAlign(CrossAxisAlignment::Start));
  }

  return ScrollView {
    Column {
      Text("ExternalTexture", TextRole::Title),
      Text(
          "The same Image and RenderScene path displays each platform producer. The overlay, border, and clipping "
          "remain ordinary HuxerUI drawing."
      ).With(Foreground(theme.colors.on_surface_variant)),
      Button(running.Get() ? "Pause producers" : "Resume producers").OnClick([demo, running] {
        const bool next = !running.Get();
        demo->SetRunning(next);
        running = next;
      }),
      std::move(samples),
    }.With(
        Padding(theme.spacing.extra_large), Spacing(theme.spacing.medium), CrossAlign(CrossAxisAlignment::Stretch),
        Background(theme.colors.background)
    ),
  }.With(ScrollBar());
}

} // namespace

View App() {
  return MaterialTheme {ExternalTextureDemo()};
}

const Application application{
    App,
    {
        .window = {
            .title = "HuxerUI ExternalTexture",
            .initial_size = {760.0F, 720.0F},
        },
        .root_hooks = {example::InstallTextureDemo},
    }
};
