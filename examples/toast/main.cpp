#include <huxerui/huxerui.h>

using namespace huxerui;

constexpr Color primary_text_color = Color::Rgb(27, 31, 36);

View App() {
  auto toast = UseToast();

  return Column {
    Text("Toast").With(FontSize(28.0F), Foreground(primary_text_color)),
    Text(
        "Toast is presented by the window LayerHost and "
        "dismisses itself after the configured duration."
    ),
    Button("Show toast").OnClick([toast] { toast.Show("Changes saved", ToastOptions { 2.5 }); }),
  }.With(Padding(32.0F), Spacing(16.0F));
}

HUXERUI_APP(
    App,
    {
        .title = "HuxerUI Toast",
        .width = 520.0F,
        .height = 360.0F,
        .root_hooks = {
            InstallToast(),
        },
    }
)
