#include <huxerui/huxerui.h>

#include <ui_gallery_resources.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace huxerui;

constexpr std::size_t actions_page = 0;
constexpr std::size_t selection_page = 1;
constexpr std::size_t text_input_page = 2;
constexpr std::size_t feedback_page = 3;
constexpr std::size_t navigation_page = 4;
constexpr std::size_t content_page = 5;
constexpr std::size_t layout_page = 6;
constexpr std::size_t motion_page = 7;

const char* PageName(std::size_t page) {
  switch (page) {
  case actions_page:
    return "Actions";
  case selection_page:
    return "Selection";
  case text_input_page:
    return "Text input";
  case feedback_page:
    return "Feedback";
  case navigation_page:
    return "Navigation";
  case content_page:
    return "Content";
  case layout_page:
    return "Layout";
  case motion_page:
    return "Motion";
  default:
    return "Actions";
  }
}

const char* ViewportName(ViewportClass viewport_class) {
  switch (viewport_class) {
  case ViewportClass::Compact:
    return "Compact";
  case ViewportClass::Medium:
    return "Medium";
  case ViewportClass::Expanded:
    return "Expanded";
  }
  return "Unknown";
}

View Tag(std::string label, Color color) {
  return Text(std::move(label)).With(
      Padding(EdgeInsets::Symmetric(10.0F, 6.0F)),
      Background(color),
      Foreground(Color::White()),
      CornerRadius(8.0F)
  );
}

constexpr float pi = std::numbers::pi_v<float>;
constexpr float circle_kappa = 0.55228475F;

Color WithAlpha(Color color, float alpha) {
  color.alpha = alpha;
  return color;
}

Path CirclePath(Point center, float radius) {
  const float control = radius * circle_kappa;
  return Path{}
      .MoveTo({center.x, center.y - radius})
      .CubicTo(
          {center.x + control, center.y - radius},
          {center.x + radius, center.y - control},
          {center.x + radius, center.y}
      )
      .CubicTo(
          {center.x + radius, center.y + control},
          {center.x + control, center.y + radius},
          {center.x, center.y + radius}
      )
      .CubicTo(
          {center.x - control, center.y + radius},
          {center.x - radius, center.y + control},
          {center.x - radius, center.y}
      )
      .CubicTo(
          {center.x - radius, center.y - control},
          {center.x - control, center.y - radius},
          {center.x, center.y - radius}
      )
      .Close();
}

void PaintOrbit(PaintContext& paint, Size size, const ColorScheme& colors) {
  const float extent = std::min(size.width, size.height);
  if (extent <= 0.0F) {
    return;
  }
  const Point center{size.width * 0.5F, size.height * 0.5F};
  const float radius = extent * 0.34F;
  const Path field = CirclePath(center, radius);

  paint.DrawPathShadow(field, Color::Rgb(0, 0, 0, 0.28F), {}, radius * 0.14F);
  paint.FillPath(field, colors.inverse_surface);
  paint.DrawArc(center, radius * 0.70F, -pi * 0.10F, pi * 1.28F, colors.primary,
                StrokeStyle{.width = radius * 0.055F, .cap = StrokeCap::Round});
  paint.DrawArc(center, radius * 0.46F, pi * 0.80F, pi * 1.12F, colors.secondary_container,
                StrokeStyle{.width = radius * 0.045F, .cap = StrokeCap::Round});
  paint.DrawArc(center, radius * 0.86F, pi * 0.58F, pi * 0.58F, colors.inverse_on_surface,
                StrokeStyle{.width = radius * 0.025F, .cap = StrokeCap::Round});
  paint.DrawCircle(center, radius * 0.14F, colors.primary);
  paint.DrawCircle(
      {center.x + std::cos(-pi * 0.10F) * radius * 0.70F, center.y + std::sin(-pi * 0.10F) * radius * 0.70F},
      radius * 0.09F,
      colors.on_primary
  );
  paint.DrawCircle(
      {center.x + std::cos(pi * 0.80F) * radius * 0.46F, center.y + std::sin(pi * 0.80F) * radius * 0.46F},
      radius * 0.065F,
      colors.secondary_container
  );
}

void PaintDataGraphic(PaintContext& paint, Size size, const ColorScheme& colors, float progress) {
  const Rect bounds{0.0F, 0.0F, size.width, size.height};
  const float inset = 22.0F;
  const float baseline = size.height - inset;
  const float right = size.width - inset;
  const float final_y = inset + (1.0F - progress) * (size.height - inset * 2.0F);

  paint.DrawLinearGradient(
      bounds,
      LinearGradient{
          .start = {0.0F, 0.0F},
          .end = {1.0F, 1.0F},
          .stops = {
              {0.0F, colors.surface_container_high},
              {1.0F, colors.surface_container_low},
          },
      },
      CornerRadii{18.0F}
  );
  for (float fraction : {0.25F, 0.5F, 0.75F}) {
    const float y = inset + (size.height - inset * 2.0F) * fraction;
    paint.DrawLine({inset, y}, {right, y}, WithAlpha(colors.on_surface_variant, 0.18F),
                   StrokeStyle{.width = 1.0F});
  }

  Path line;
  line.MoveTo({inset, baseline * 0.72F})
      .CubicTo(
          {size.width * 0.22F, size.height * 0.25F},
          {size.width * 0.33F, size.height * 0.82F},
          {size.width * 0.48F, size.height * 0.46F}
      )
      .CubicTo(
          {size.width * 0.64F, size.height * 0.08F},
          {size.width * 0.78F, size.height * 0.72F},
          {right, final_y}
      );
  Path area = line;
  area.LineTo({right, baseline}).LineTo({inset, baseline}).Close();

  paint.PushPathClip(area);
  paint.DrawLinearGradient(
      bounds,
      LinearGradient{
          .start = {0.0F, 0.0F},
          .end = {0.0F, 1.0F},
          .stops = {
              {0.0F, WithAlpha(colors.primary, 0.62F)},
              {1.0F, WithAlpha(colors.primary, 0.05F)},
          },
      }
  );
  paint.PopClip();
  paint.StrokePath(line, colors.primary,
                   StrokeStyle{.width = 4.0F, .cap = StrokeCap::Round, .join = StrokeJoin::Round});
  paint.DrawCircle({right, final_y}, 7.0F, colors.background);
  paint.DrawCircle({right, final_y}, 4.0F, colors.primary);
}

void PaintPathStudy(PaintContext& paint, Size size, const ColorScheme& colors) {
  const float inset = std::min(size.width, size.height) * 0.10F;
  Path shape;
  shape.MoveTo({inset, size.height * 0.66F})
      .CubicTo(
          {size.width * 0.20F, -8.0F},
          {size.width * 0.72F, size.height + 8.0F},
          {size.width - inset, size.height * 0.34F}
      )
      .LineTo({size.width - inset, size.height - inset})
      .LineTo({inset, size.height - inset})
      .Close();

  paint.DrawPathShadow(shape, Color::Rgb(0, 0, 0, 0.24F), {}, 18.0F);
  paint.PushPathClip(shape);
  paint.DrawRadialGradient(
      {0.0F, 0.0F, size.width, size.height},
      RadialGradient{
          .center = {0.28F, 0.22F},
          .radius = {0.85F, 1.10F},
          .stops = {
              {0.0F, colors.secondary_container},
              {0.56F, colors.primary},
              {1.0F, colors.inverse_surface},
          },
      }
  );
  paint.PushTransform(Transform2D{1.0F, 0.0F, 0.0F, 1.0F, size.width * 0.04F, 0.0F});
  paint.StrokePath(
      Path{}
          .MoveTo({size.width * 0.20F, size.height * 0.53F})
          .QuadraticTo({size.width * 0.50F, size.height * 0.16F}, {size.width * 0.76F, size.height * 0.47F}),
      Color::Rgb(255, 255, 255, 0.64F),
      StrokeStyle{.width = 9.0F, .cap = StrokeCap::Round, .join = StrokeJoin::Round}
  );
  paint.PopTransform();
  paint.PopClip();
  paint.StrokePath(shape,
                   LinearGradient{
                       .start = {0.0F, 0.0F},
                       .end = {1.0F, 0.0F},
                       .stops = {{0.0F, colors.on_primary}, {1.0F, colors.secondary_container}},
                   },
                   StrokeStyle{.width = 2.0F, .cap = StrokeCap::Round, .join = StrokeJoin::Round});
}

[[huxerui::composable]]
View OrbitCanvasPreview() {
  const ThemeSpec& theme = UseTheme();
  auto started = UseState(false);
  Lifecycle([started] { started = true; });
  const ColorScheme colors = theme.colors;
  const bool animated = started.Get() && !theme.motion.reduced_motion;
  return Canvas([colors](PaintContext& paint, Size size) { PaintOrbit(paint, size, colors); })
      .With(
          Grow(),
          Rotation(
              animated
                  ? AnimateTo(
                        -360.0F,
                        TweenSpec{3.15, Easing::Linear},
                        AnimationPlayback{.iterations = std::nullopt}
                    )
                  : AnimateTo(0.0F, SnapSpec{})
          )
      );
}

[[huxerui::composable]]
View DataCanvasPreview() {
  const ThemeSpec& theme = UseTheme();
  auto progress = UseState(0.68F);
  const ColorScheme colors = theme.colors;
  const float value = progress.Get();
  return Column {
    Canvas([colors, value](PaintContext& paint, Size size) { PaintDataGraphic(paint, size, colors, value); })
        .With(Grow()),
    Row {
      Slider(progress).OnChanged([progress](float next) { progress = next; }),
      Text::Format("{}%", static_cast<int>(value * 100.0F)),
    }.With(Spacing(theme.spacing.medium), CrossAlign(CrossAxisAlignment::Center)),
  }.With(Spacing(theme.spacing.small), CrossAlign(CrossAxisAlignment::Stretch));
}

[[huxerui::composable]]
View PathCanvasPreview() {
  const ColorScheme colors = UseTheme().colors;
  return Canvas([colors](PaintContext& paint, Size size) { PaintPathStudy(paint, size, colors); }).With(Grow());
}

[[huxerui::composable]]
View CanvasStudio() {
  const ThemeSpec& theme = UseTheme();
  auto selected = UseState<std::size_t>(0);
  return Column {
    SegmentedButton({"Orbit", "Data", "Path"}, selected)
        .OnChanged([selected](std::size_t index) { selected = index; }),
    IndexedPages(
        {
            OrbitCanvasPreview(),
            DataCanvasPreview(),
            PathCanvasPreview(),
        },
        selected
    ).With(
        Frame{.height = 280.0F},
        Padding(theme.spacing.small),
        Background(theme.colors.surface_container),
        CornerRadius(theme.shapes.large)
    ),
  }.With(Spacing(theme.spacing.medium), CrossAlign(CrossAxisAlignment::Stretch));
}

[[huxerui::composable]]
View GallerySection(const char* title, const char* description, View content) {
  const ThemeSpec& theme = UseTheme();
  return Column {
    Text(title, TextRole::Title),
    Text(description).With(Foreground(theme.colors.on_surface_variant)),
    std::move(content),
  }.With(
      Padding(theme.spacing.large),
      Spacing(theme.spacing.medium),
      Background(theme.colors.surface_container_low),
      CornerRadius(theme.shapes.extra_large),
      CrossAlign(CrossAxisAlignment::Stretch)
  );
}

[[huxerui::composable]]
View ActionsDemo() {
  const ThemeSpec& theme = UseTheme();
  auto chip_selected = UseState(false);
  return Column {
    GallerySection(
        "Buttons",
        "Text and icon actions share typed click events, indication, focus, disabled state, and tooltips.",
        Flow {
          Button("Primary action").OnClick([] {}),
          Button("Disabled").OnClick([] {}).With(Enabled(false)),
          IconButton(ui_gallery::images::link, "Open link").OnClick([] {}).With(Tooltip("Open link")),
          IconButton(ui_gallery::images::lock, "Unavailable secure action")
              .OnClick([] {})
              .With(Enabled(false), Tooltip("Secure actions are unavailable")),
        }.With(Spacing(theme.spacing.medium), CrossAlign(CrossAxisAlignment::Center))
    ),
    GallerySection(
        "Chips",
        "Action and selectable chips adapt their surface, icon, semantics, and indication to the active Theme.",
        Flow {
          Chip(ui_gallery::images::list, "Action").OnClick([] {}),
          Chip(chip_selected ? "Selected" : "Selectable", chip_selected)
              .OnChanged([chip_selected](bool selected) { chip_selected = selected; }),
          Chip("Disabled", false).OnChanged([](bool) {}).With(Enabled(false)),
        }.With(Spacing(theme.spacing.small), CrossAlign(CrossAxisAlignment::Center))
    ),
    GallerySection(
        "Pointer cursors",
        "Hover these regions with a mouse or pen to see portable cursor declarations.",
        Flow {
          Tag("Hand", theme.colors.primary).With(PointerCursor(PointerCursorKind::Hand)),
          Tag("Crosshair", theme.colors.primary).With(PointerCursor(PointerCursorKind::Crosshair)),
          Tag("Grab", theme.colors.primary).With(PointerCursor(PointerCursorKind::Grab)),
          Tag("Horizontal resize", theme.colors.primary)
              .With(PointerCursor(PointerCursorKind::ResizeHorizontal)),
        }.With(Spacing(theme.spacing.small), CrossAlign(CrossAxisAlignment::Center))
    ),
  }.With(Spacing(theme.spacing.medium), CrossAlign(CrossAxisAlignment::Stretch));
}

[[huxerui::composable]]
View SelectionDemo() {
  const ThemeSpec& theme = UseTheme();
  auto checkbox_checked = UseState(true);
  auto radio_choice = UseState(0);
  auto switch_checked = UseState(false);
  auto segment = UseState<std::size_t>(0);
  auto tab = UseState<std::size_t>(0);
  auto density = UseState<std::size_t>(1);
  auto value = UseState(0.35F);
  const std::vector<std::string> density_options{"Compact", "Comfortable", "Spacious"};

  return Column {
    GallerySection(
        "Toggles",
        "Checkbox, RadioButton, and Switch expose controlled values and requested changes.",
        Column {
          Flow {
            Checkbox(checkbox_checked ? "Checked" : "Unchecked", checkbox_checked)
                .OnChanged([checkbox_checked](bool checked) { checkbox_checked = checked; }),
            Switch(switch_checked ? "On" : "Off", switch_checked)
                .OnChanged([switch_checked](bool checked) { switch_checked = checked; }),
          }.With(Spacing(theme.spacing.medium), CrossAlign(CrossAxisAlignment::Center)),
          Flow {
            RadioButton("Option A", radio_choice == 0).OnChanged([radio_choice](bool selected) {
              if (selected) {
                radio_choice = 0;
              }
            }),
            RadioButton("Option B", radio_choice == 1).OnChanged([radio_choice](bool selected) {
              if (selected) {
                radio_choice = 1;
              }
            }),
          }.With(Spacing(theme.spacing.medium), CrossAlign(CrossAxisAlignment::Center)),
        }.With(Spacing(theme.spacing.small), CrossAlign(CrossAxisAlignment::Stretch))
    ),
    GallerySection(
        "Grouped selection",
        "SegmentedButton, Tabs, and Select keep selection controlled across visible, navigational, and compact sets.",
        Column {
          SegmentedButton(
              {
                  SegmentedButtonItem("Day"),
                  SegmentedButtonItem(ui_gallery::images::list, "Week"),
                  SegmentedButtonItem::IconOnly(ui_gallery::images::grid, "Month"),
              },
              segment
          ).OnChanged([segment](std::size_t index) { segment = index; }),
          Tabs(
              {
                  TabItem(ui_gallery::images::content, "Overview"),
                  TabItem(ui_gallery::images::feedback, "Activity"),
                  std::move(TabItem("Disabled")).Enabled(false),
              },
              tab
          ).OnChanged([tab](std::size_t index) { tab = index; }),
          Select(density_options, density, [](const std::string& label) {
            return Text(label).Key(label);
          }).Label("Density")
            .OnChanged([density](std::size_t index) { density = index; }),
          Text::Format(
              "Selected segment: {} · selected tab: {} · density: {}",
              segment + 1, tab + 1, density_options[density]
          ),
        }.With(Spacing(theme.spacing.medium), CrossAlign(CrossAxisAlignment::Stretch))
    ),
    GallerySection(
        "Continuous value",
        "Slider reports a controlled value with optional range and step constraints.",
        Row {
          Slider(value).Step(0.05F).OnChanged([value](float next) { value = next; }),
          Text::Format("{}%", static_cast<int>(value * 100.0F)),
        }.With(Spacing(theme.spacing.medium), CrossAlign(CrossAxisAlignment::Center))
    ),
  }.With(Spacing(theme.spacing.medium), CrossAlign(CrossAxisAlignment::Stretch));
}

[[huxerui::composable]]
View TextInputDemo() {
  const ThemeSpec& theme = UseTheme();
  auto repository_url = UseState(TextEditingValue::FromText("https://github.com/HuxerUI/HuxerUI"));
  auto display_name = UseState(TextEditingValue::FromText("HuxerUI"));
  auto password = UseState(TextEditingValue::FromText(""));
  auto message = UseState(TextEditingValue::FromText(""));

  return Column {
    GallerySection(
        "Field variants",
        "Filled, outlined, and standard fields share editing, selection, composition, icons, and floating labels.",
        Column {
          TextField(display_name)
              .Align(TextAlign::Center)
              .Label("Display name")
              .Placeholder("Your name")
              .OnChanged([display_name](const TextEditingValue& value) { display_name = value; }),
          TextField(repository_url)
              .Label("HTTPS URL")
              .Placeholder("https://github.com/owner/repo")
              .LeadingIcon(ui_gallery::images::link)
              .Variant(TextFieldVariant::Standard)
              .OnChanged([repository_url](const TextEditingValue& value) { repository_url = value; }),
          TextField(password)
              .Secure()
              .MaxLength(64)
              .Label("Password")
              .Placeholder("Enter password")
              .LeadingIcon(ui_gallery::images::lock)
              .Variant(TextFieldVariant::Outlined)
              .Validation(Validate(password.Get().text, Required("Password is required")))
              .OnChanged([password](const TextEditingValue& value) { password = value; }),
        }.With(Spacing(theme.spacing.medium), CrossAlign(CrossAxisAlignment::Stretch))
    ),
    GallerySection(
        "Multiline editing",
        "Line limits and maximum length constrain presentation while TextEditingValue remains authoritative.",
        TextField(message)
            .LineLimits(TextFieldLineLimits::MultiLine(3, 5))
            .MaxLength(240)
            .Label("Message")
            .Placeholder("Write a message")
            .LeadingIcon(ui_gallery::images::message)
            .Variant(TextFieldVariant::Outlined)
            .OnChanged([message](const TextEditingValue& value) { message = value; })
    ),
  }.With(Spacing(theme.spacing.medium), CrossAlign(CrossAxisAlignment::Stretch));
}

[[huxerui::composable]]
View GalleryDialogCard(State<bool> visible) {
  const ThemeSpec& theme = UseTheme();
  const DialogStyle& style = UseEnvironment<DialogStyle>();
  return Column {
    Text("Declarative dialog", TextRole::Title),
    Text("The Dialog retained modifier follows application state."),
    Button("Close").OnClick([visible] { visible = false; }),
  }.With(
      Frame{.min_width = style.minimum_width, .max_width = style.maximum_width},
      Padding(theme.spacing.large),
      Spacing(theme.spacing.medium),
      Background(style.background),
      CornerRadius(style.corner_radius),
      style.shadow
  );
}

[[huxerui::composable]]
View GalleryBottomSheet(BottomSheetContext bottom_sheet) {
  const ThemeSpec& theme = UseTheme();
  return Column {
    Text("Bottom sheet", TextRole::Title),
    Text("Drag the handle or use the action below to dismiss this sheet."),
    Button("Close").OnClick([bottom_sheet] { bottom_sheet.Dismiss(); }),
  }.With(Padding(theme.spacing.large), Spacing(theme.spacing.medium));
}

[[huxerui::composable]]
View GalleryPopup(PopupContext popup) {
  const ThemeSpec& theme = UseTheme();
  return Column {
    Text("Anchored popup", TextRole::Title),
    Text("Popup placement follows the final bounds of its anchor."),
    Button("Close").OnClick([popup] { popup.Dismiss(); }),
  }.With(
      Frame{.min_width = 240.0F},
      Padding(theme.spacing.medium),
      Spacing(theme.spacing.small),
      Background(theme.colors.surface_container_high),
      CornerRadius(theme.shapes.large)
  );
}

[[huxerui::composable]]
View FeedbackDemo(MenuHandle menu) {
  const ThemeSpec& theme = UseTheme();
  auto progress = UseState(0.35F);
  auto declarative_dialog_visible = UseState(false);
  auto toast = UseToast();
  auto dialog = UseDialog();
  auto bottom_sheet = UseBottomSheet();
  auto popup = UsePopup();

  return Column {
    Text(
        "Context menu is available across this page. Right-click anywhere, or focus an item and press Shift+F10.",
        TextRole::Label
    ).With(
        Padding(theme.spacing.medium),
        Foreground(theme.colors.on_secondary_container),
        Background(theme.colors.secondary_container),
        CornerRadius(theme.shapes.large)
    ),
    GallerySection(
        "Progress",
        "Determinate and indeterminate indicators share Theme motion, color, and accessibility semantics.",
        Column {
          Flow {
            ProgressCircle(),
            Text("Indeterminate"),
            ProgressCircle(progress),
            Text::Format("{}%", static_cast<int>(progress * 100.0F)),
            Button("Advance").OnClick([progress] {
              progress.Update([](float& value) { value = value >= 0.95F ? 0.15F : value + 0.2F; });
            }),
          }.With(Spacing(theme.spacing.small), CrossAlign(CrossAxisAlignment::Center)),
          Row {
            ProgressBar(),
            Text("Indeterminate"),
          }.With(Spacing(theme.spacing.small), CrossAlign(CrossAxisAlignment::Center)),
          Row {
            ProgressBar(progress),
            Text::Format("{}%", static_cast<int>(progress * 100.0F)),
          }.With(Spacing(theme.spacing.small), CrossAlign(CrossAxisAlignment::Center)),
        }.With(Spacing(theme.spacing.small), CrossAlign(CrossAxisAlignment::Stretch))
    ),
    GallerySection(
        "Window-level presentation",
        "Toast, Dialog, and BottomSheet present feedback outside the application layout tree.",
        Flow {
          Button("Show toast").OnClick([toast] { toast.Show("Changes saved"); }),
          Button("Open alert").OnClick([dialog] {
            dialog.Show("Save changes?", "The current document has unsaved changes.", "Save", "Cancel");
          }),
          Button("Open declarative dialog").OnClick([declarative_dialog_visible] {
            declarative_dialog_visible = true;
          }),
          Button("Open bottom sheet").OnClick([bottom_sheet] { bottom_sheet.Show(GalleryBottomSheet); }),
        }.With(Spacing(theme.spacing.small), CrossAlign(CrossAxisAlignment::Center))
    ),
    GallerySection(
        "Anchored presentation",
        "Popup and Menu support stable anchor geometry or an explicit window-local interaction point.",
        Flow {
          Button("Show popup").With(popup.Anchor()).OnClick([popup] { popup.Show(GalleryPopup); }),
          Button("Show menu").With(menu.Anchor()).OnClick([menu] {
            menu.Show({
                MenuItem(ui_gallery::images::content, "Open", [] {}),
                MenuItem("Duplicate", [] {}),
                MenuItem(
                    "Move to",
                    {
                        MenuItem("Archive", [] {}),
                        MenuItem("Trash", [] {}),
                    }
                ),
                MenuSection{},
                std::move(MenuItem("Delete", [] {})).Enabled(false),
            });
          }),
        }.With(Spacing(theme.spacing.small), CrossAlign(CrossAxisAlignment::Center))
    ),
  }.With(
      Spacing(theme.spacing.medium),
      CrossAlign(CrossAxisAlignment::Stretch),
      Dialog {
          .visible = declarative_dialog_visible,
          .content = [declarative_dialog_visible] { return GalleryDialogCard(declarative_dialog_visible); },
          .dismiss_on_outside_press = true,
          .dismiss_on_cancel = true,
          .on_dismiss_request = [declarative_dialog_visible] { declarative_dialog_visible = false; },
      }
  );
}

[[huxerui::composable]]
View GalleryStackDetails() {
  const ThemeSpec& theme = UseTheme();
  const NavigationController navigation = UseNavigation();
  return Column {
    Text("Details", TextRole::Title),
    Text::Format("Retained stack depth: {}", navigation.Depth()),
    Button("Back").OnClick([navigation] { navigation.Pop(); }),
  }.With(
      Padding(theme.spacing.medium),
      Spacing(theme.spacing.small),
      Background(theme.colors.surface_container)
  );
}

[[huxerui::composable]]
View GalleryStackRoot() {
  const ThemeSpec& theme = UseTheme();
  const NavigationController navigation = UseNavigation();
  return Column {
    Text("Root destination", TextRole::Title),
    Text::Format("Retained stack depth: {}", navigation.Depth()),
    Button("Push details").OnClick([navigation] { navigation.Push(GalleryStackDetails); }),
  }.With(
      Padding(theme.spacing.medium),
      Spacing(theme.spacing.small),
      Background(theme.colors.surface_container)
  );
}

[[huxerui::composable]]
View NavigationDemo() {
  const ThemeSpec& theme = UseTheme();
  auto destination = UseState<std::size_t>(0);
  return Column {
    GallerySection(
        "Destination selection",
        "NavigationBar shares NavigationItem data and controlled selection with the expanded pane on the left.",
        Column {
          NavigationBar(
              {
                  NavigationItem(ui_gallery::images::actions, "Home"),
                  NavigationItem(ui_gallery::images::content, "Library"),
                  NavigationItem(ui_gallery::images::feedback, "Activity"),
              },
              destination
          ).OnChanged([destination](std::size_t index) { destination = index; }),
          Text::Format("Selected destination: {}", destination.Get() + 1),
        }.With(Spacing(theme.spacing.small), CrossAlign(CrossAxisAlignment::Stretch))
    ),
    GallerySection(
        "History navigation",
        "NavigationStack owns retained push and pop history independently from destination selectors.",
        NavigationStack(GalleryStackRoot).With(Frame{.height = 180.0F})
    ),
    GallerySection(
        "Responsive application shell",
        "This gallery itself demonstrates TopAppBar, NavigationPane, StartDrawer, EndDrawer, and DrawerLayout.",
        Text("Resize the window to move navigation and appearance tools between permanent and modal drawers.")
    ),
  }.With(Spacing(theme.spacing.medium), CrossAlign(CrossAxisAlignment::Stretch));
}

[[huxerui::composable]]
View ContentDemo() {
  const ThemeSpec& theme = UseTheme();
  return Column {
    GallerySection(
        "Text and selection",
        "Text roles resolve through Theme, while SelectionArea adds shared pointer and keyboard selection behavior.",
        SelectionArea(
            Column {
              Text("Title role", TextRole::Title),
              Text("Body role supports selectable application content."),
              Text("Label role", TextRole::Label).With(Foreground(theme.colors.on_surface_variant)),
            }.With(Spacing(theme.spacing.small))
        )
    ),
    GallerySection(
        "Images",
        "Image accepts resources, decoded assets, vectors, textures, fitting, alignment, sampling, and tint.",
        Flow {
          Image(ui_gallery::images::content)
              .Fit(ImageFit::Contain)
              .Tint(theme.colors.primary)
              .With(Frame{.width = 72.0F, .height = 72.0F}),
          Image(ui_gallery::images::motion)
              .Fit(ImageFit::Contain)
              .Tint(theme.colors.error)
              .With(Frame{.width = 72.0F, .height = 72.0F}),
          Image(ui_gallery::images::grid)
              .Fit(ImageFit::Contain)
              .Tint(theme.colors.on_secondary_container)
              .With(
                  Frame{.width = 72.0F, .height = 72.0F},
                  Padding(18.0F),
                  Background(theme.colors.secondary_container),
                  CornerRadius(theme.shapes.large)
              ),
        }.With(Spacing(theme.spacing.large), CrossAlign(CrossAxisAlignment::Center))
    ),
    GallerySection(
        "Canvas studio",
        "Canvas combines gradients, paths, clipping, shadows, transforms, animation, and data-driven repainting.",
        CanvasStudio()
    ),
  }.With(Spacing(theme.spacing.medium), CrossAlign(CrossAxisAlignment::Stretch));
}

[[huxerui::composable]]
View LayoutDemo() {
  const ThemeSpec& theme = UseTheme();
  auto preview_page = UseState<std::size_t>(0);
  return Column {
    GallerySection(
        "Linear and wrapping layout",
        "Column, Row, Flow, Spacer, Grow, and Divider describe reusable constraint-based structure.",
        Column {
          Row {
            Tag("Fixed", theme.colors.error),
            Text("Grow").With(
                Padding(EdgeInsets::Symmetric(10.0F, 6.0F)),
                Background(theme.colors.primary),
                Foreground(theme.colors.on_primary),
                CornerRadius(theme.shapes.medium),
                Grow()
            ),
            Tag("Trailing", theme.colors.secondary),
          }.With(Spacing(theme.spacing.small), CrossAlign(CrossAxisAlignment::Center)),
          Row {
            Text("Horizontal"),
            Divider(Axis::Vertical).With(Frame{.height = 24.0F}),
            Text("Vertical"),
          }.With(Spacing(theme.spacing.medium), CrossAlign(CrossAxisAlignment::Center)),
          Flow {
            Tag("Android", theme.colors.primary),
            Tag("iOS", theme.colors.error),
            Tag("macOS", theme.colors.secondary),
            Tag("Windows", theme.colors.secondary),
            Tag("Linux", theme.colors.primary),
            Tag("Web", theme.colors.error),
          }.With(Spacing(theme.spacing.small), CrossAlign(CrossAxisAlignment::Center)),
        }.With(Spacing(theme.spacing.medium), CrossAlign(CrossAxisAlignment::Stretch))
    ),
    GallerySection(
        "Shape geometry",
        "Corner radii resolve against final bounds; the full shape produces circles and capsules "
        "consistently across platforms.",
        Column {
          Row {
            Text("Full")
                .Align(TextAlign::Center)
                .VerticalAlign(TextVerticalAlign::Center)
                .With(
                    Frame{.width = 48.0F, .height = 48.0F},
                    Foreground(theme.colors.on_primary),
                    Background(theme.colors.primary),
                    CornerRadius(theme.shapes.full)
                ),
            Text("Adaptive capsule")
                .VerticalAlign(TextVerticalAlign::Center)
                .With(
                    Frame{.height = 48.0F},
                    Padding(EdgeInsets::Symmetric(theme.spacing.medium, 0.0F)),
                    Foreground(theme.colors.on_secondary_container),
                    Background(theme.colors.secondary_container),
                    CornerRadius(theme.shapes.full),
                    Grow()
                ),
          }.With(Spacing(theme.spacing.medium), CrossAlign(CrossAxisAlignment::Center)),
          Row {
            Text("SECURE SESSION").With(Foreground(theme.colors.primary)),
            Spacer().With(Grow()),
            Text("256-bit encrypted").With(Foreground(theme.colors.on_surface_variant)),
          }.With(
              Frame{.height = 40.0F},
              Padding(EdgeInsets::Symmetric(theme.spacing.medium, theme.spacing.small)),
              Background(theme.colors.surface_container_low),
              Border{theme.colors.outline, 1.0F},
              CornerRadius(theme.shapes.full),
              CrossAlign(CrossAxisAlignment::Center)
          ),
        }.With(Spacing(theme.spacing.medium), CrossAlign(CrossAxisAlignment::Stretch))
    ),
    GallerySection(
        "Stack and retained pages",
        "Stack overlays aligned children; IndexedPages retains peers while presenting one selected child.",
        Column {
          Stack {
            Column {}.With(
                Frame{.height = 112.0F},
                Background(theme.colors.secondary_container),
                CornerRadius(theme.shapes.large)
            ),
            Tag("Centered overlay", theme.colors.primary)
                .With(Align(HorizontalAlignment::Center, VerticalAlignment::Center)),
          },
          Flow {
            Button("First").OnClick([preview_page] { preview_page = 0; }),
            Button("Second").OnClick([preview_page] { preview_page = 1; }),
          }.With(Spacing(theme.spacing.small)),
          IndexedPages(
              {
                  Tag("First retained page", theme.colors.primary),
                  Tag("Second retained page", theme.colors.error),
              },
              preview_page
          ).With(Frame{.height = 56.0F}),
        }.With(Spacing(theme.spacing.medium), CrossAlign(CrossAxisAlignment::Stretch))
    ),
    GallerySection(
        "Virtual scrolling",
        "VirtualList and VirtualGrid realize only viewport items while preserving keyed item state where required.",
        Column {
          VirtualList(20, [theme](std::size_t index) {
            return Text::Format("Virtual row {}", index + 1)
                .With(
                    Padding(theme.spacing.small),
                    Background(index % 2 == 0 ? theme.colors.surface_container : theme.colors.surface_container_high)
                )
                .Key(index);
          })
              .ItemExtent(42.0F)
              .With(Frame{.height = 168.0F}, ScrollBar()),
          VirtualGrid(18, [theme](std::size_t index) {
            return Text::Format("{}", index + 1)
                .With(
                    Padding(theme.spacing.small),
                    Background(theme.colors.secondary_container),
                    CornerRadius(theme.shapes.medium)
                )
                .Key(index);
          })
              .Columns(GridColumns::Adaptive(96.0F))
              .RowExtent(52.0F)
              .With(Frame{.height = 180.0F}, ScrollBar()),
        }.With(Spacing(theme.spacing.medium), CrossAlign(CrossAxisAlignment::Stretch))
    ),
  }.With(Spacing(theme.spacing.medium), CrossAlign(CrossAxisAlignment::Stretch));
}

[[huxerui::composable]]
View MotionDemo() {
  const ThemeSpec& theme = UseTheme();
  auto transformed = UseState(false);
  auto alternate_scene = UseState(false);
  auto scene_transition = UseSceneTransition();

  return Column {
    GallerySection(
        "View motion",
        "Presentation modifiers animate without recomposing every frame and respect the Theme reduced-motion policy.",
        Column {
          Button(transformed ? "Reset" : "Animate").OnClick([transformed] { transformed = !transformed; }),
          Flow {
            Tag("Scale + rotation", theme.colors.primary).With(
                Scale(AnimateTo(transformed ? 1.16F : 1.0F, TweenSpec(theme.motion.normal, Easing::EaseOut))),
                Rotation(AnimateTo(transformed ? 8.0F : 0.0F, SpringSpec()))
            ),
            Tag("Offset + opacity", theme.colors.error).With(
                Offset(AnimateTo(Point{transformed ? 20.0F : 0.0F, 0.0F}, SpringSpec())),
                Opacity(AnimateTo(transformed ? 0.55F : 1.0F, TweenSpec(theme.motion.normal, Easing::EaseInOut)))
            ),
          }.With(Spacing(theme.spacing.extra_large), CrossAlign(CrossAxisAlignment::Center)),
        }.With(Spacing(theme.spacing.large), CrossAlign(CrossAxisAlignment::Stretch))
    ),
    GallerySection(
        "Scene transition",
        "Scene transitions capture the committed frame around an application-owned state mutation.",
        Column {
          Button("Change scene").OnClick([alternate_scene, scene_transition] {
            scene_transition.RunFromCurrentInteraction(
                CircularRevealSceneTransition{}, [alternate_scene] { alternate_scene = !alternate_scene; }
            );
          }),
          Text(alternate_scene ? "Alternate scene" : "Initial scene", TextRole::Title).With(
              Frame{.height = 120.0F},
              Padding(theme.spacing.large),
              Background(
                  alternate_scene ? theme.colors.surface_container_highest : theme.colors.secondary_container
              ),
              CornerRadius(theme.shapes.extra_large),
              Align(HorizontalAlignment::Center, VerticalAlignment::Center)
          ),
        }.With(Spacing(theme.spacing.medium), CrossAlign(CrossAxisAlignment::Stretch))
    ),
    GallerySection(
        "Surface effects",
        "Border, corner radius, shadow, background, and foreground compose as ordered presentation modifiers.",
        Flow {
          Text("Border").With(
              Padding(theme.spacing.medium),
              Border{theme.colors.primary, 2.0F},
              CornerRadius(theme.shapes.medium)
          ),
          Text("Shadow").With(
              Padding(theme.spacing.medium),
              Background(theme.colors.surface_container_high),
              CornerRadius(theme.shapes.medium),
              Shadow{Color::Rgb(0, 0, 0, 0.28F), {0.0F, 5.0F}, 14.0F, 0.0F}
          ),
        }.With(Spacing(theme.spacing.large), CrossAlign(CrossAxisAlignment::Center))
    ),
  }.With(Spacing(theme.spacing.medium), CrossAlign(CrossAxisAlignment::Stretch));
}

[[huxerui::composable]]
View ToolValue(const char* label, const char* value) {
  const ThemeSpec& theme = UseTheme();
  return Row {
    Text(label, TextRole::Label).With(Foreground(theme.colors.on_surface_variant)),
    Spacer(),
    Text(value),
  }.With(CrossAlign(CrossAxisAlignment::Center));
}

[[huxerui::composable]]
View GalleryToolsContent(
    State<std::size_t> selected_page,
    State<std::size_t> theme_family,
    State<bool> dark_mode
) {
  const ThemeSpec& theme = UseTheme();
  auto family_transition = UseSceneTransition();
  auto brightness_transition = UseSceneTransition();
  return Column {
    Text("Tools", TextRole::Title),
    Text("Appearance", TextRole::Label).With(Foreground(theme.colors.on_surface_variant)),
    SegmentedButton({"Material", "Flat"}, theme_family)
        .OnChanged([family_transition, theme_family](std::size_t index) {
          family_transition.RunFromCurrentInteraction(
              CircularRevealSceneTransition{}, [theme_family, index] { theme_family = index; }
          );
        }),
    Switch(dark_mode ? "Dark mode" : "Light mode", dark_mode)
        .OnChanged([brightness_transition, dark_mode](bool enabled) {
          brightness_transition.RunFromCurrentInteraction(
              CircularRevealSceneTransition{}, [dark_mode, enabled] { dark_mode = enabled; }
          );
        }),
    Divider(),
    Text("Context", TextRole::Label).With(Foreground(theme.colors.on_surface_variant)),
    ToolValue("Page", PageName(selected_page.Get())),
    ToolValue("Viewport", ViewportName(UseViewportClass())),
  }.With(Spacing(theme.spacing.medium), CrossAlign(CrossAxisAlignment::Stretch));
}

[[huxerui::composable]]
View GalleryNavigation(State<std::size_t> selected_page, State<bool> start_open) {
  const ThemeSpec& theme = UseTheme();
  return Column {
    Column {
      Text("HuxerUI", TextRole::Title),
      Text("UI Gallery", TextRole::Label).With(Foreground(theme.colors.on_surface_variant)),
    }.With(
        Padding(EdgeInsets::Symmetric(theme.spacing.large, theme.spacing.medium)),
        Spacing(2.0F)
    ),
    Text("COMPONENTS", TextRole::Label).With(
        Padding(EdgeInsets::Symmetric(theme.spacing.large, theme.spacing.small)),
        Foreground(theme.colors.on_surface_variant)
    ),
    NavigationPane(
        {
            NavigationItem(ui_gallery::images::actions, "Actions")
                .SelectedIcon(ui_gallery::images::actions_selected),
            NavigationItem(ui_gallery::images::selection, "Selection")
                .SelectedIcon(ui_gallery::images::selection_selected),
            NavigationItem(ui_gallery::images::text_input, "Text input")
                .SelectedIcon(ui_gallery::images::text_input_selected),
            NavigationItem(ui_gallery::images::feedback, "Feedback")
                .SelectedIcon(ui_gallery::images::feedback_selected),
            NavigationItem(ui_gallery::images::navigation, "Navigation")
                .SelectedIcon(ui_gallery::images::navigation_selected),
            NavigationItem(ui_gallery::images::content, "Content")
                .SelectedIcon(ui_gallery::images::content_selected),
            NavigationItem(ui_gallery::images::layout, "Layout")
                .SelectedIcon(ui_gallery::images::layout_selected),
            NavigationItem(ui_gallery::images::motion, "Motion")
                .SelectedIcon(ui_gallery::images::motion_selected),
        },
        selected_page,
        true
    ).OnChanged([selected_page, start_open](std::size_t index) {
      selected_page = index;
      start_open = false;
    }).With(Grow()),
    Text("Each destination retains its local state and scroll position.", TextRole::Label)
        .With(
            Padding(EdgeInsets::Symmetric(theme.spacing.large, theme.spacing.medium)),
            Foreground(theme.colors.on_surface_variant)
        ),
  }.With(
      Background(theme.colors.surface_container_low),
      CrossAlign(CrossAxisAlignment::Stretch)
  );
}

[[huxerui::composable]]
View GalleryTools(State<std::size_t> selected_page, State<std::size_t> theme_family, State<bool> dark_mode) {
  const ThemeSpec& theme = UseTheme();
  return GalleryToolsContent(selected_page, theme_family, dark_mode).With(
      Padding(theme.spacing.large),
      Background(theme.colors.surface_container_low)
  );
}

View GalleryPage(const char* description, View content, float padding) {
  return ScrollView {
    Column {
      Text(description),
      std::move(content),
    }.With(Padding(padding), Spacing(padding), CrossAlign(CrossAxisAlignment::Stretch))
  }.With(ScrollBar());
}

[[huxerui::composable]]
View FeedbackPage(float padding) {
  auto menu = UseMenu();
  return GalleryPage(
      "Progress and window-level presentation through layers and anchored surfaces.", FeedbackDemo(menu), padding
  ).On<ViewEvents::ContextMenuRequested>([menu](Point position) {
    menu.ShowAt(position, {
        MenuItem("Refresh", [] {}),
        MenuItem("Inspect", [] {}),
        MenuItem("Share", [] {}),
    });
  });
}

[[huxerui::composable]]
View GalleryMain(
    State<std::size_t> selected_page,
    State<bool> start_open,
    State<bool> end_open
) {
  const ThemeSpec& theme = UseTheme();
  const ViewportClass viewport_class = UseViewportClass();

  std::optional<View> leading;
  if (viewport_class == ViewportClass::Compact) {
    leading = IconButton(ui_gallery::images::menu, "Open navigation")
                  .OnClick([start_open, end_open] {
                    end_open = false;
                    start_open = true;
                  })
                  .With(Tooltip("Open navigation"));
  }
  std::vector<View> actions;
  if (viewport_class != ViewportClass::Expanded) {
    actions.push_back(
        IconButton(ui_gallery::images::tune, "Open tools")
            .OnClick([start_open, end_open, viewport_class] {
              if (viewport_class == ViewportClass::Compact) {
                start_open = false;
              }
              end_open = true;
            })
            .With(Tooltip("Open tools"))
    );
  }

  return Column {
    TopAppBar(PageName(selected_page.Get()), std::move(leading), std::move(actions)),
    IndexedPages(
        {
            GalleryPage(
                "Buttons, icon actions, chips, indication, disabled state, and contextual help.",
                ActionsDemo(),
                theme.spacing.large
            ),
            GalleryPage(
                "Boolean, exclusive, grouped, tabbed, and continuous controlled selection.",
                SelectionDemo(),
                theme.spacing.large
            ),
            GalleryPage(
                "Single-line, secure, validated, and multiline editing across all field variants.",
                TextInputDemo(),
                theme.spacing.large
            ),
            FeedbackPage(theme.spacing.large),
            GalleryPage(
                "Destination selection, retained history, app bars, and responsive drawers.",
                NavigationDemo(),
                theme.spacing.large
            ),
            GalleryPage(
                "Themed text, selection, resources, vector images, and custom canvas drawing.",
                ContentDemo(),
                theme.spacing.large
            ),
            GalleryPage(
                "Linear, wrapping, overlay, retained, scrolling, and virtualized layout.",
                LayoutDemo(),
                theme.spacing.large
            ),
            GalleryPage(
                "Retained View animation, frame-level scene transitions, and surface effects.",
                MotionDemo(),
                theme.spacing.large
            ),
        },
        selected_page
    ).With(Grow()),
  }.With(
      Background(theme.colors.background),
      CrossAlign(CrossAxisAlignment::Stretch)
  );
}

[[huxerui::composable]]
View GalleryShell(
    State<std::size_t> selected_page,
    State<std::size_t> theme_family,
    State<bool> dark_mode,
    State<bool> start_open,
    State<bool> end_open
) {
  const ThemeSpec& theme = UseTheme();
  DrawerStyle drawer_style = UseEnvironment<DrawerStyle>();
  drawer_style.preferred_width = 272.0F;
  drawer_style.minimum_width = 240.0F;
  drawer_style.minimum_content_width = 560.0F;
  return ProvideEnvironment(
      std::move(drawer_style),
      DrawerLayout {
        GalleryMain(selected_page, start_open, end_open).With(Background(theme.colors.background)),

        StartDrawer {
          GalleryNavigation(selected_page, start_open),
        }.Open(start_open).OnOpenChanged([start_open](bool open) { start_open = open; }),

        EndDrawer {
          GalleryTools(selected_page, theme_family, dark_mode),
        }.Open(end_open).OnOpenChanged([end_open](bool open) { end_open = open; }),
      }
  );
}

View App() {
  auto selected_page = UseState<std::size_t>(0);
  auto theme_family = UseState<std::size_t>(0);
  auto dark_mode = UseState(false);
  auto start_open = UseState(false);
  auto end_open = UseState(false);

  if (theme_family == 1) {
    if (dark_mode) {
      return FlatDarkTheme {GalleryShell(selected_page, theme_family, dark_mode, start_open, end_open)};
    }
    return FlatTheme {GalleryShell(selected_page, theme_family, dark_mode, start_open, end_open)};
  }

  if (dark_mode) {
    return MaterialDarkTheme {GalleryShell(selected_page, theme_family, dark_mode, start_open, end_open)};
  }
  return MaterialTheme {GalleryShell(selected_page, theme_family, dark_mode, start_open, end_open)};
}

const Application application{
    App,
    {
        .window = {
            .title = "HuxerUI UI Gallery",
            .initial_size = {1200.0F, 760.0F},
            .content_mode = WindowContentMode::EdgeToEdge,
        },
        .viewport_breakpoints = ViewportBreakpoints{720.0F, 1040.0F},
    }
};
