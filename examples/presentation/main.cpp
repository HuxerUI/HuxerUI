#include <huxerui/huxerui.h>

using namespace huxerui;

constexpr Color primary_text_color = Color::Rgb(27, 31, 36);
constexpr Color secondary_text_color = Color::Rgb(91, 98, 106);
constexpr Color surface_color = Color::White();

View DeclarativeDialogCard(State<bool> visible) {
  return Column {
    Text("Delete item?").With(FontSize(24.0F), Foreground(primary_text_color)),
    Text("This dialog is presented by a declarative modifier."),
    Row {
      Button("Cancel").OnClick([visible] { visible = false; }),
      Button("Delete").OnClick([visible] { visible = false; }),
    }.With(Spacing(12.0F)),
  }.With(Padding(24.0F), Spacing(16.0F), Background(surface_color), CornerRadius(12.0F));
}

View CommandDialogCard(DialogContext dialog) {
  return Column {
    Text("Command dialog").With(FontSize(24.0F), Foreground(primary_text_color)),
    Text("DialogContext lets this content dismiss its own layer."),
    Button("Close").OnClick([dialog] { dialog.Dismiss(); }),
  }.With(Padding(24.0F), Spacing(16.0F), Background(surface_color), CornerRadius(12.0F));
}

View BottomSheetCard(BottomSheetContext bottom_sheet) {
  const ThemeSpec& theme = UseTheme();
  return Column {
    Text("Bottom sheet", TextRole::Title),
    Text("BottomSheetContext closes the sheet without exposing its layer id."),
    Button("Close").OnClick([bottom_sheet] { bottom_sheet.Dismiss(); }),
  }.With(
      Frame{.min_width = 280.0F},
      Padding(theme.spacing.large),
      Spacing(theme.spacing.medium)
  );
}

View PopupCard(PopupContext popup) {
  return Column {
    Text("Anchored popup").With(FontSize(18.0F), Foreground(primary_text_color)),
    Text("PopupContext closes the popup without exposing its layer id."),
    Button("Close").OnClick([popup] { popup.Dismiss(); }),
  }.With(Padding(16.0F), Spacing(8.0F), Background(surface_color), CornerRadius(10.0F));
}

View App() {
  auto declarative_dialog_visible = UseState(false);
  auto toast = UseToast();
  auto dialog = UseDialog();
  auto bottom_sheet = UseBottomSheet();
  auto popup = UsePopup();
  auto menu = UseMenu();

  return Column {
    ScrollView {
      Column {
        Text("Presentation").With(FontSize(30.0F), Foreground(primary_text_color)),
        Text("Window-level feedback, modal content, and anchored surfaces.")
            .With(Foreground(secondary_text_color)),
        Text("Feedback and modal content").With(FontSize(20.0F), Foreground(primary_text_color)),
        Flow {
          Button("Show toast").OnClick([toast] { toast.Show("Changes saved", ToastOptions{2.5}); }),
          Button("Open alert").OnClick([dialog] {
            dialog.Show("Save changes?", "The current document has unsaved changes.", "Save");
          }),
          Button("Open custom dialog").OnClick([dialog] {
            dialog.Show(CommandDialogCard, DialogOptions{.dismiss_on_outside_press = false});
          }),
          Button("Open declarative dialog").OnClick([declarative_dialog_visible] {
            declarative_dialog_visible = true;
          }),
        }.With(Spacing(12.0F)),
        Button("Open bottom sheet").OnClick([bottom_sheet] { bottom_sheet.Show(BottomSheetCard); }),
        Text("Anchored content").With(FontSize(20.0F), Foreground(primary_text_color)),
        Text("Popup and Menu follow the final bounds of their anchor View.")
            .With(Foreground(secondary_text_color)),
        Flow {
          Button("Show popup").With(popup.Anchor()).OnClick([popup] { popup.Show(PopupCard); }),
          Button("Show menu").With(menu.Anchor()).OnClick([menu] {
            menu.Show({
                MenuItem("Rename", [] {}),
                MenuItem("Duplicate", [] {}),
                MenuItem(
                    "Move to",
                    {
                        MenuItem("Archive", [] {}),
                        MenuItem("Trash", [] {}),
                    }
                ),
                MenuSection{},
                MenuItem("Delete", [] {}),
            });
          }),
        }.With(Spacing(12.0F)),
      }.With(Spacing(16.0F), CrossAlign(CrossAxisAlignment::Start)),
    }.With(ScrollBar(), Grow()),
  }.With(
      Padding(32.0F),
      CrossAlign(CrossAxisAlignment::Stretch),
      Dialog {
          .visible = declarative_dialog_visible,
          .content = [declarative_dialog_visible] { return DeclarativeDialogCard(declarative_dialog_visible); },
          .dismiss_on_outside_press = true,
          .on_dismiss_request = [declarative_dialog_visible] { declarative_dialog_visible = false; },
      }
  );
}

View PresentationRoot() {
  return MaterialTheme(App);
}

const Application application{
    PresentationRoot,
    {
        .window = {
            .title = "HuxerUI Presentation",
            .initial_size = {720.0F, 560.0F},
        },
    }
};
