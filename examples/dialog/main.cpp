#include <huxerui/huxerui.h>

using namespace huxerui;

constexpr Color primary_text_color = Color::Rgb(27, 31, 36);

View DeclarativeDialogCard(State<bool> visible) {
  return Column {
    Text("Delete item?").With(FontSize(24.0F), Foreground(primary_text_color)),
    Text("This dialog is presented by a declarative modifier."),
    Row {
        Button("Cancel").OnClick([visible] { visible = false; }),
        Button("Delete").OnClick([visible] { visible = false; }),
    }.With(Spacing(12.0F)),
  }.With(Padding(24.0F), Spacing(16.0F), Background(Color::White()), CornerRadius(12.0F));
}

View CommandDialogCard(DialogContext dialog) {
  return Column {
    Text("Command dialog").With(FontSize(24.0F), Foreground(primary_text_color)),
    Text("DialogContext lets this content dismiss its own layer."),
    Button("Close").OnClick([dialog] { dialog.Dismiss(); }),
  }.With(Padding(24.0F), Spacing(16.0F), Background(Color::White()), CornerRadius(12.0F));
}

View App() {
  auto dialog_visible = UseState(false);
  auto dialogs = UseDialogs();

  return Column {
    Text("Dialog").With(FontSize(28.0F), Foreground(primary_text_color)),
    Text("Dialogs can be owned by state or opened as commands."),
    Button("Open declarative dialog").OnClick([dialog_visible] { dialog_visible = true; }),
    Button("Open command dialog").OnClick([dialogs] {
      dialogs.Show(
          [](DialogContext dialog) { return CommandDialogCard(dialog); },
          DialogOptions {
              .dismiss_on_outside_press = false,
          }
      );
    }),
  }.With(
      Padding(32.0F),
      Spacing(16.0F),
      Dialog {
          .visible = dialog_visible,
          .content = [dialog_visible] { return DeclarativeDialogCard(dialog_visible); },
          .dismiss_on_outside_press = true,
          .on_dismiss_request = [dialog_visible] { dialog_visible = false; },
      }
  );
}

HUXERUI_APP(
    App,
    {
        .title = "HuxerUI Dialog",
        .width = 520.0F,
        .height = 360.0F,
        .root_hooks = {
            InstallDialogs(),
        },
    }
)
