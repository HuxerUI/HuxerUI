#include <huxerui/huxerui.h>

#include <array>
#include <optional>
#include <string_view>

using namespace huxerui;

namespace {

constexpr std::array<std::string_view, 4> lyrics{
    "The sky turns blue at the edge of town",
    "Streetlights fade as the train rolls down",
    "Over the river and through the rain",
    "I'll follow the morning home again",
};

View LyricsOverlay(State<std::size_t> line) {
  const std::size_t current = line;
  return Stack {
    Column {
      Text(lyrics[current], TextRole::Title).With(Foreground(Color::Rgb(249, 249, 254))),
      Row {
        Text(lyrics[(current + 1) % lyrics.size()]).With(Foreground(Color::Rgb(189, 200, 222)), Grow()),
        Button("Next").OnClick([line] {
          const std::size_t current_line = line;
          line = (current_line + 1) % lyrics.size();
        }),
      }.With(Spacing(10.0F), CrossAlign(CrossAxisAlignment::Center)),
    }.With(
        Padding(16.0F),
        Spacing(6.0F),
        CrossAlign(CrossAxisAlignment::Stretch),
        Align(HorizontalAlignment::Stretch, VerticalAlignment::Stretch),
        Background(Color::Rgb(22, 28, 45, 0.86F)),
        WindowDragRegion{}
    ),
  }.With(Padding(10.0F));
}

} // namespace

View App() {
  const auto application = UseApplication();
  const auto support = application.WindowSupport();
  auto overlay = UseState(std::optional<WindowHandle>{});
  auto line = UseState(std::size_t{0});
  auto on_top = UseState(false);
  auto pass_through = UseState(false);
  const std::optional<WindowHandle>& current = overlay;
  const bool open = current && current->IsOpen();
  UseWindow().OnCloseRequest([application] {
    application.Quit();
    return true;
  });

  return Column {
    Text("Desktop lyrics", TextRole::Title),
    Text("Drag the lyrics panel to move it. Switch on click-through to use the window below it."),
    Row {
      Button(open ? "Show overlay" : "Open overlay").OnClick([application, support, overlay, line, on_top, pass_through] {
        const std::optional<WindowHandle>& window = overlay;
        if (window && window->IsOpen()) {
          window->Show();
          return;
        }
        overlay = application.OpenWindow([line] { return LyricsOverlay(line); }, {
            .title = "Lyrics overlay",
            .initial_size = {540.0F, 140.0F},
            .chrome_mode = WindowChromeMode::None,
            .transparent_background = support.transparent_background,
            .always_on_top = support.always_on_top,
            .activatable = !support.non_activating,
            .resizable = false,
        });
        on_top = support.always_on_top;
        pass_through = false;
      }),
      Button("Next lyric").OnClick([line] {
        const std::size_t current_line = line;
        line = (current_line + 1) % lyrics.size();
      }),
      Button("Close overlay").OnClick([overlay] {
        const std::optional<WindowHandle>& window = overlay;
        if (window) window->Close();
      }).With(Enabled(open)),
    }.With(Spacing(10.0F)),
    Row {
      Button(pass_through ? "Restore pointer input" : "Pass clicks through").OnClick([overlay, pass_through] {
        const std::optional<WindowHandle>& window = overlay;
        const bool enabled = !pass_through;
        if (window && window->SetPointerPassThrough(enabled)) pass_through = enabled;
      }).With(Enabled(open && support.pointer_pass_through)),
      Button(on_top ? "Use ordinary level" : "Keep on top").OnClick([overlay, on_top] {
        const std::optional<WindowHandle>& window = overlay;
        const bool enabled = !on_top;
        if (window && window->SetAlwaysOnTop(enabled)) on_top = enabled;
      }).With(Enabled(open && support.always_on_top)),
    }.With(Spacing(10.0F)),
  }.With(Padding(24.0F), Spacing(14.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

const Application application{
    App,
    {
        .window = {.title = "Desktop Lyrics", .initial_size = {620.0F, 280.0F}},
        .show_debug_overlay = false,
    },
};
