#include <huxerui/huxerui.h>

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace huxerui;

constexpr std::size_t controls_page = 0;
constexpr std::size_t layout_page = 1;
constexpr std::size_t motion_page = 2;

const char* PageName(std::size_t page) {
  switch (page) {
  case controls_page:
    return "Controls";
  case layout_page:
    return "Layout";
  case motion_page:
    return "Motion";
  default:
    return "Controls";
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

VectorAsset ListIcon() {
  static const VectorAsset icon = VectorAsset::Create({18.0F, 18.0F}, [](VectorBuilder& builder) {
    for (float y : {4.0F, 9.0F, 14.0F}) {
      builder.StrokePath(
          Path{}.MoveTo({3.0F, y}).LineTo({15.0F, y}),
          Color::Black(),
          2.0F,
          StrokeCap::Round
      );
    }
  });
  return icon;
}

VectorAsset GridIcon() {
  static const VectorAsset icon = VectorAsset::Create({18.0F, 18.0F}, [](VectorBuilder& builder) {
    for (float y : {3.0F, 10.0F}) {
      for (float x : {3.0F, 10.0F}) {
        builder.FillPath(Path::RoundedRect({x, y, 5.0F, 5.0F}, CornerRadii{1.0F}), Color::Black());
      }
    }
  });
  return icon;
}

VectorAsset LinkIcon() {
  static const VectorAsset icon = VectorAsset::Create({18.0F, 18.0F}, [](VectorBuilder& builder) {
    builder.StrokePath(Path::RoundedRect({1.0F, 6.0F, 7.0F, 6.0F}, CornerRadii{3.0F}), Color::Black(), 2.0F);
    builder.StrokePath(Path::RoundedRect({10.0F, 6.0F, 7.0F, 6.0F}, CornerRadii{3.0F}), Color::Black(), 2.0F);
    builder.StrokePath(Path{}.MoveTo({6.0F, 9.0F}).LineTo({12.0F, 9.0F}), Color::Black(), 2.0F, StrokeCap::Round);
  });
  return icon;
}

VectorAsset LockIcon() {
  static const VectorAsset icon = VectorAsset::Create({18.0F, 18.0F}, [](VectorBuilder& builder) {
    builder.StrokePath(
        Path{}
            .MoveTo({5.0F, 8.0F})
            .LineTo({5.0F, 6.0F})
            .CubicTo({5.0F, 2.0F}, {13.0F, 2.0F}, {13.0F, 6.0F})
            .LineTo({13.0F, 8.0F}),
        Color::Black(),
        2.0F,
        StrokeCap::Round
    );
    builder.FillPath(Path::RoundedRect({3.0F, 7.0F, 12.0F, 9.0F}, CornerRadii{2.0F}), Color::Black());
  });
  return icon;
}

VectorAsset MessageIcon() {
  static const VectorAsset icon = VectorAsset::Create({18.0F, 18.0F}, [](VectorBuilder& builder) {
    builder.FillPath(Path::RoundedRect({2.0F, 2.0F, 14.0F, 11.0F}, CornerRadii{2.0F}), Color::Black());
    builder.FillPath(
        Path{}.MoveTo({5.0F, 12.0F}).LineTo({5.0F, 16.0F}).LineTo({9.0F, 12.0F}).Close(),
        Color::Black()
    );
  });
  return icon;
}

VectorAsset MenuIcon() {
  static const VectorAsset icon = VectorAsset::Create({24.0F, 24.0F}, [](VectorBuilder& builder) {
    for (float y : {6.0F, 11.0F, 16.0F}) {
      builder.FillPath(Path::RoundedRect({3.0F, y, 18.0F, 2.0F}, {}), Color::Black());
    }
  });
  return icon;
}

VectorAsset TuneIcon() {
  static const VectorAsset icon = VectorAsset::Create({24.0F, 24.0F}, [](VectorBuilder& builder) {
    for (Rect rect : {
             Rect{3.0F, 5.0F, 10.0F, 2.0F},
             Rect{15.0F, 3.0F, 2.0F, 6.0F},
             Rect{17.0F, 5.0F, 4.0F, 2.0F},
             Rect{3.0F, 11.0F, 4.0F, 2.0F},
             Rect{7.0F, 9.0F, 2.0F, 6.0F},
             Rect{9.0F, 11.0F, 12.0F, 2.0F},
             Rect{3.0F, 17.0F, 6.0F, 2.0F},
             Rect{11.0F, 15.0F, 2.0F, 6.0F},
             Rect{13.0F, 17.0F, 8.0F, 2.0F},
         }) {
      builder.FillPath(Path::RoundedRect(rect, {}), Color::Black());
    }
  });
  return icon;
}

View Panel(View content) {
  const ThemeSpec& theme = UseTheme();
  return std::move(content).With(
      Padding(theme.spacing.medium),
      Background(theme.colors.surface_container_low),
      CornerRadius(theme.shapes.extra_large)
  );
}

[[huxerui::scope]]
View ControlsDemo() {
  const ThemeSpec& theme = UseTheme();
  auto checkbox_checked = UseState(true);
  auto chip_selected = UseState(false);
  auto period = UseState<std::size_t>(0);
  auto radio_choice = UseState(0);
  auto switch_checked = UseState(false);
  auto progress = UseState(0.35F);
  auto repository_url = UseState(TextEditingValue::FromText("https://github.com/huxerui/huxerui"));
  auto password = UseState(TextEditingValue::FromText(""));
  auto message = UseState(TextEditingValue::FromText(""));

  return Panel(
      Column {
        Text("Controls", TextRole::Title),
        Flow {
          Button("Button").OnClick([] {}),
          Button("Disabled").With(Enabled(false)).OnClick([] {}),
          IconButton(LinkIcon(), "Open link").OnClick([] {}),
          IconButton(LockIcon(), "Disabled secure action").OnClick([] {}).With(Enabled(false)),
        }.With(Spacing(theme.spacing.medium), CrossAlign(CrossAxisAlignment::Center)),
        Flow {
          Chip(ListIcon(), "Action").OnClick([] {}),
          Chip(chip_selected ? "Selected" : "Selectable", chip_selected)
              .OnChanged([chip_selected](bool selected) { chip_selected = selected; }),
          Chip("Disabled", false).OnChanged([](bool) {}).With(Enabled(false)),
        }.With(Spacing(theme.spacing.small), CrossAlign(CrossAxisAlignment::Center)),
        Column {
          SegmentedButton(
              {
                  SegmentedButtonItem("Day"),
                  SegmentedButtonItem(ListIcon(), "Week"),
                  SegmentedButtonItem::IconOnly(GridIcon(), "Month"),
              },
              period
          ).OnChanged([period](std::size_t index) { period = index; }),
          Text(period == 0 ? "Day view" : period == 1 ? "Week view" : "Month view"),
        }.With(Spacing(theme.spacing.small), CrossAlign(CrossAxisAlignment::Start)),
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
        Row {
          Slider(progress).Step(0.05F).OnChanged([progress](float value) { progress = value; }),
          Text::Format("{}%", static_cast<int>(progress * 100.0F)),
        }.With(Spacing(theme.spacing.small), CrossAlign(CrossAxisAlignment::Center)),
        TextField(repository_url)
            .Label("HTTPS URL")
            .Placeholder("https://github.com/owner/repo")
            .LeadingIcon(LinkIcon())
            .Variant(TextFieldVariant::Standard)
            .OnChanged([repository_url](const TextEditingValue& value) { repository_url = value; }),
        TextField(password)
            .Secure()
            .MaxLength(64)
            .Label("Password")
            .Placeholder("Enter password")
            .LeadingIcon(LockIcon())
            .Validation(Validate(password.Get().text, Required("Password is required")))
            .OnChanged([password](const TextEditingValue& value) { password = value; }),
        TextField(message)
            .LineLimits(TextFieldLineLimits::MultiLine(3, 5))
            .MaxLength(240)
            .Label("Message")
            .Placeholder("Write a message")
            .LeadingIcon(MessageIcon())
            .Variant(TextFieldVariant::Outlined)
            .OnChanged([message](const TextEditingValue& value) { message = value; }),
      }.With(Spacing(theme.spacing.medium), CrossAlign(CrossAxisAlignment::Stretch))
  );
}

View LayoutDemo() {
  const ThemeSpec& theme = UseTheme();
  return Panel(
      Column {
        Text("Layout", TextRole::Title),
        Divider(),
        Row {
          Tag("Fixed", theme.colors.error),
          Text("Grow").With(
              Padding(EdgeInsets::Symmetric(10.0F, 6.0F)),
              Background(theme.colors.primary),
              Foreground(theme.colors.on_primary),
              CornerRadius(theme.shapes.medium),
              Grow()
          ),
          Tag("Trailing", Color::Rgb(26, 127, 55)),
        }.With(Spacing(theme.spacing.small), CrossAlign(CrossAxisAlignment::Center)),
        Row {
          Text("Horizontal"),
          Divider(Axis::Vertical).With(Frame{.height = 24.0F}),
          Text("Vertical"),
        }.With(Spacing(theme.spacing.medium), CrossAlign(CrossAxisAlignment::Center)),
        Flow {
          Tag("Android", Color::Rgb(26, 127, 55)),
          Tag("macOS", theme.colors.primary),
          Tag("Windows", Color::Rgb(130, 80, 223)),
          Tag("Declarative", theme.colors.error),
          Tag("Native", theme.colors.primary),
          Tag("C++", Color::Rgb(26, 127, 55)),
        }.With(Spacing(theme.spacing.small), CrossAlign(CrossAxisAlignment::Center)),
      }.With(Spacing(theme.spacing.medium), CrossAlign(CrossAxisAlignment::Stretch))
  );
}

[[huxerui::scope]]
View MotionDemo() {
  const ThemeSpec& theme = UseTheme();
  auto transformed = UseState(false);

  return Panel(
      Column {
        Text("Motion", TextRole::Title),
        Flow {
          Button(transformed ? "Reset" : "Transform").OnClick([transformed] { transformed = !transformed; }),
          Tag("Scale + rotation", Color::Rgb(130, 80, 223)).With(
              Scale(AnimateTo(transformed ? 1.2F : 1.0F, TweenSpec(theme.motion.normal, Easing::EaseOut))),
              Rotation(AnimateTo(transformed ? 10.0F : 0.0F, SpringSpec()))
          ),
        }.With(Spacing(theme.spacing.large), CrossAlign(CrossAxisAlignment::Center)),
      }.With(Spacing(theme.spacing.medium), CrossAlign(CrossAxisAlignment::Stretch))
  );
}

View ToolValue(const char* label, const char* value) {
  const ThemeSpec& theme = UseTheme();
  return Row {
    Text(label, TextRole::Label).With(Foreground(theme.colors.on_surface_variant)),
    Spacer(),
    Text(value),
  }.With(CrossAlign(CrossAxisAlignment::Center));
}

View GalleryToolsContent(std::size_t selected_page, State<std::size_t> theme_family, State<bool> dark_mode) {
  const ThemeSpec& theme = UseTheme();
  return Column {
    Text("Tools", TextRole::Title),
    Text("Appearance", TextRole::Label).With(Foreground(theme.colors.on_surface_variant)),
    SegmentedButton({"Material", "Flat"}, theme_family)
        .OnChanged([theme_family](std::size_t index) { theme_family = index; }),
    Switch(dark_mode ? "Dark mode" : "Light mode", dark_mode)
        .OnChanged([dark_mode](bool enabled) { dark_mode = enabled; }),
    Divider(),
    Text("Context", TextRole::Label).With(Foreground(theme.colors.on_surface_variant)),
    ToolValue("Page", PageName(selected_page)),
    ToolValue("Viewport", ViewportName(UseViewportClass())),
  }.With(Spacing(theme.spacing.medium), CrossAlign(CrossAxisAlignment::Stretch));
}

View GalleryNavigation(State<std::size_t> selected_page, State<bool> start_open) {
  const ThemeSpec& theme = UseTheme();
  return Column {
    Text("HuxerUI", TextRole::Title),
    Text("UI Gallery", TextRole::Label).With(Foreground(theme.colors.on_surface_variant)),
    Divider(),
    NavigationPane(
      {
        NavigationItem("Controls"),
        NavigationItem("Layout"),
        NavigationItem("Motion"),
      },
      selected_page,
      true
    ).OnChanged([selected_page, start_open](std::size_t index) {
      selected_page = index;
      start_open = false;
    }),
    Spacer(),
    Text("StartDrawer keeps destination state in the application tree.", TextRole::Label)
        .With(Foreground(theme.colors.on_surface_variant)),
  }.With(
      Padding(theme.spacing.large),
      Spacing(theme.spacing.medium),
      Background(theme.colors.surface_container_low),
      CrossAlign(CrossAxisAlignment::Stretch)
  );
}

View GalleryTools(std::size_t selected_page, State<std::size_t> theme_family, State<bool> dark_mode) {
  const ThemeSpec& theme = UseTheme();
  return GalleryToolsContent(selected_page, theme_family, dark_mode).With(
      Padding(theme.spacing.large),
      Background(theme.colors.surface_container_low)
  );
}

View GalleryPage(std::size_t selected_page) {
  switch (selected_page) {
  case controls_page:
    return ControlsDemo();
  case layout_page:
    return LayoutDemo();
  case motion_page:
    return MotionDemo();
  default:
    return ControlsDemo();
  }
}

View GalleryMain(
    State<std::size_t> selected_page,
    State<bool> start_open,
    State<bool> end_open
) {
  const ThemeSpec& theme = UseTheme();
  const ViewportClass viewport_class = UseViewportClass();

  std::optional<View> leading;
  if (viewport_class == ViewportClass::Compact) {
    leading = IconButton(MenuIcon(), "Open navigation")
                  .OnClick([start_open, end_open] {
                    end_open = false;
                    start_open = true;
                  });
  }
  std::vector<View> actions;
  if (viewport_class != ViewportClass::Expanded) {
    actions.push_back(IconButton(TuneIcon(), "Open tools").OnClick([start_open, end_open, viewport_class] {
      if (viewport_class == ViewportClass::Compact) {
        start_open = false;
      }
      end_open = true;
    }));
  }

  return Column {
    TopAppBar("UI Gallery", std::move(leading), std::move(actions)),
    Column {
      Text("Controls, layout, and motion in one responsive workspace", TextRole::Label)
          .With(Foreground(theme.colors.on_surface_variant)),
      Tabs({"Controls", "Layout", "Motion"}, selected_page.Get())
          .OnChanged([selected_page](std::size_t index) { selected_page = index; }),
      ScrollView {
        Column {
          GalleryPage(selected_page.Get()),
        }.With(Padding(theme.spacing.small), CrossAlign(CrossAxisAlignment::Stretch))
      }.With(ScrollBar(), Grow()),
    }.With(
        Padding(theme.spacing.large),
        Spacing(theme.spacing.medium),
        CrossAlign(CrossAxisAlignment::Stretch),
        Grow()
    ),
  }.With(
      Background(theme.colors.background),
      CrossAlign(CrossAxisAlignment::Stretch)
  );
}

View GalleryShell(
    State<std::size_t> selected_page,
    State<std::size_t> theme_family,
    State<bool> dark_mode,
    State<bool> start_open,
    State<bool> end_open
) {
  const ThemeSpec& theme = UseTheme();
  return DrawerLayout {
    GalleryMain(selected_page, start_open, end_open).With(Background(theme.colors.background)),

    StartDrawer {
      GalleryNavigation(selected_page, start_open),
    }.Open(start_open).OnOpenChanged([start_open](bool open) { start_open = open; }),

    EndDrawer {
      GalleryTools(selected_page.Get(), theme_family, dark_mode),
    }.Open(end_open).OnOpenChanged([end_open](bool open) { end_open = open; }),
  };
}

View App() {
  auto selected_page = UseState<std::size_t>(0);
  auto theme_family = UseState<std::size_t>(0);
  auto dark_mode = UseState(false);
  auto start_open = UseState(false);
  auto end_open = UseState(false);

  if (theme_family == 1) {
    return dark_mode ? FlatDarkTheme(GalleryShell, selected_page, theme_family, dark_mode, start_open, end_open)
                     : FlatTheme(GalleryShell, selected_page, theme_family, dark_mode, start_open, end_open);
  }

  return dark_mode ? MaterialDarkTheme(GalleryShell, selected_page, theme_family, dark_mode, start_open, end_open)
                   : MaterialTheme(GalleryShell, selected_page, theme_family, dark_mode, start_open, end_open);
}

HUXERUI_APP(
    App,
    {
        .title = "HuxerUI UI Gallery",
        .width = 1200.0F,
        .height = 760.0F,
        .viewport_breakpoints = ViewportBreakpoints{720.0F, 1040.0F},
    }
)
