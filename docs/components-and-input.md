# Components and Input

## Text and selection

`Text` supports body, label, and title roles:

```cpp
Text("Heading", TextRole::Title);
Text::Format("Taps {}", count);
```

Static text is not selectable by default. Wrap related content in a `SelectionArea` to enable drag selection and copying across Text nodes:

```cpp
SelectionArea {
  Column {
    Text("Selectable heading", TextRole::Title),
    Text("This paragraph can be selected."),
  },
};
```

## Button, Checkbox, RadioButton, and Switch

Button, Checkbox, RadioButton, and Switch participate in focus traversal and share their pointer and keyboard activation paths. Checkbox, RadioButton, and Switch are controlled:

```cpp
auto checked = UseState(false);

return Row {
  Checkbox("Remember me", checked).OnChanged([checked](bool value) {
    checked = value;
  }),
  Switch("Notifications", checked).OnChanged([checked](bool value) {
    checked = value;
  }),
};
```

Checkbox, RadioButton, and Switch also retain their label-free constructors for custom composition. A labeled control owns its label, uses the Theme spacing between the visual control and text, and treats the complete control as one focusable and clickable target.

RadioButton represents one controlled choice rather than owning a group. Application state defines mutual exclusion, and activating an already selected RadioButton leaves the selection unchanged:

```cpp
auto choice = UseState(0);

return Row {
  RadioButton("Option A", choice == 0).OnChanged([choice](bool selected) {
    if (selected) {
      choice = 0;
    }
  }),
  RadioButton("Option B", choice == 1).OnChanged([choice](bool selected) {
    if (selected) {
      choice = 1;
    }
  }),
};
```

`Enabled(false)` is inherited by descendants. A disabled control remains a hit-test barrier but does not receive pointer, scroll, focus, or Click input.

Custom interactive views opt in to focus:

```cpp
CustomControl()
    .With(Focusable())
    .On<ViewEvents::FocusChanged>(HandleFocus)
    .On<ViewEvents::KeyDown>(HandleKey);
```

## Chip

Chip has action and selectable forms. An action Chip emits Click:

```cpp
Chip("Open filters").OnClick(OpenFilters);
```

A selectable Chip is controlled. Its `bool` constructor value defines the current selection, and `OnChanged` requests the next value:

```cpp
auto selected = UseState(false);

return Chip(selected ? "Selected" : "Selectable", selected)
    .OnChanged([selected](bool value) {
      selected = value;
    });
```

Chip also accepts a leading image resource or resolved image asset while retaining its required text label:

```cpp
Chip(app_resources::images::filter, "Filters").OnClick(OpenFilters);

Chip(vector_icon, "Selectable", selected)
    .OnChanged([selected](bool value) {
      selected = value;
    });
```

`OnChanged` delegates to `On<ToggleEvents::Changed>`. Both forms participate in focus traversal and use the active Theme's indication and component style. `ChipStyle` owns the icon size and spacing. Vector icons follow the current label color, while raster assets preserve their encoded colors. Use `Enabled(false)` for a disabled Chip. Chip intentionally retains a visible label; compose a custom image action when an action should be icon-only.

## SegmentedButton

SegmentedButton presents a compact set of side-by-side choices and keeps selection controlled by the owner:

```cpp
auto period = UseState<std::size_t>(0);

return SegmentedButton({"Day", "Week", "Month"}, period)
    .OnChanged([period](std::size_t index) {
      period = index;
    });
```

Use `SegmentedButtonItem` when a segment includes an icon or visually displays only an icon:

```cpp
SegmentedButton(
    {
        SegmentedButtonItem("List"),
        SegmentedButtonItem(app_resources::images::grid, "Grid"),
        SegmentedButtonItem::IconOnly(app_resources::images::map, "Map"),
    },
    mode
).OnChanged([mode](std::size_t index) {
  mode = index;
});
```

The label passed to `IconOnly` is required semantic content and is not drawn. `OnChanged` delegates to `On<SegmentedButtonEvents::Changed>`. Left and Right move through the choices with wrapping, while Home and End select the first and last choice. Use `Enabled(false)` to disable the complete control. `SegmentedButtonStyle` owns shared geometry, icon sizing and spacing, and selected and unselected colors. Use Chip when choices are independently selectable.

SegmentedButton is intended for a small set of short choices, usually two to five. A larger or more descriptive choice set is clearer as RadioButton rows, Chip content, or a Menu.

## Tabs

Tabs represents selection among peer destinations while leaving the corresponding page content and lifecycle with the application. Selection is controlled by an index:

```cpp
auto selected = UseState<std::size_t>(0);

return Tabs({"Overview", "Activity", "Settings"}, selected)
    .OnChanged([selected](std::size_t index) {
      selected = index;
    });
```

Use `TabItem` for icons, icon-only presentation, or an individually disabled destination:

```cpp
Tabs(
    {
        TabItem(app_resources::images::home, "Home"),
        TabItem::IconOnly(app_resources::images::search, "Search"),
        std::move(TabItem("Reports")).Enabled(false),
    },
    selected
).OnChanged([selected](std::size_t index) {
  selected = index;
});
```

The semantic label of an icon-only item is required but not drawn. Left and Right move with wrapping, Home and End move to the first and last enabled item, and every keyboard path skips disabled items. More tabs than the available width scroll horizontally, and a newly selected item is revealed automatically.

`TabsStyle` owns label, indicator, and divider appearance; item metrics; indication; indicator motion; and the theme's width policy. Flat tabs keep their content widths and use an item-wide indicator. Material primary tabs divide available width equally until their natural content needs horizontal scrolling, use a 3 dp content-wide indicator with a 24 dp minimum width, and draw the standard divider when the row does not overflow. Tabs does not mount, cache, or transition page content; those responsibilities belong to a future navigation container rather than this selection control.

## Divider

Divider is horizontal by default and expands across a bounded width. Pass `Axis::Vertical` for a vertical divider:

```cpp
Column {
  Text("First"),
  Divider(),
  Text("Second"),
};

Row {
  Text("Left"),
  Divider(Axis::Vertical).With(Frame{.height = 24.0F}),
  Text("Right"),
};
```

`DividerStyle` supplies the Theme color and thickness. `Frame`, `Padding`, and `Background` remain available for local geometry, inset, and color overrides. A vertical divider needs a bounded height, an explicit `Frame`, or a stretching parent layout.

## ProgressCircle

An empty constructor creates indeterminate progress. A value from `0` to `1` creates determinate progress:

```cpp
ProgressCircle();
ProgressCircle(0.65F);
ProgressCircle(progress);
```

Indeterminate progress advances through retained animation state. Material Theme uses its trackless pulsing-arc motion, while Flat Theme keeps the denser sweep treatment. Reduced motion themes keep the retained phase static.

## ProgressBar

An empty constructor creates an indeterminate progress bar. A value from `0` to `1` creates determinate progress:

```cpp
ProgressBar();
ProgressBar(0.65F);
ProgressBar(progress);
```

ProgressBar is a controlled display component and does not emit events. Its default width, height, colors, corner radius, track gap, stop indicator, and indeterminate animation come from `ProgressBarStyle`; layout modifiers can override its dimensions.

`ProgressBarIndeterminateMotion::Sweep` moves one fixed-width segment and is the Flat Theme default. `Segmented` uses independent head and tail positions for two segments and is the Material Theme default. `ProgressBarStyle::animation_duration` is the number of seconds per indeterminate loop. Smaller values move faster; a non-positive or non-finite duration keeps a representative static indicator without requesting frames.

## Slider

Slider is a controlled single-value input. It uses a `0` to `1` range by default; `Range` and `Step` configure component-specific behavior:

```cpp
Slider(volume)
    .Range(0.0F, 100.0F)
    .Step(1.0F)
    .OnChanged([volume](float value) { volume = value; });
```

Pointer and touch input update the value while dragging. Arrow keys adjust by `Step`, or by one percent of the range when no step is set; Home and End select the range endpoints. The owner must apply `OnChanged` values to the next composition.

`OnChanged` is the convenience wrapper for `On<SliderEvents::Changed>`.

`SliderStyle` controls the split track, enabled and disabled colors, thumb dimensions, track gap, discrete tick and stop indicators, focus-ring policy, and interaction animation. Layout modifiers can override the component dimensions. Flat Theme retains a compact track, conventional thumb, and node focus ring. Material Theme uses its taller track, narrow handle, component-specific disabled colors, and handle-width focus treatment without drawing a focus ring around the complete slider bounds.

## Image

Image displays raster ImageAsset values, vector VectorAsset values, or an ImageResource that resolves either format automatically:

```cpp
Image(app_resources::images::logo)
    .Fit(ImageFit::Contain)
    .With(Frame{.width = 160.0F, .height = 120.0F});
```

UseImage returns a raster asset and UseVectorImage returns a vector asset when application code needs the concrete value.
Vector assets can also be constructed with VectorAsset::Create and painted by Canvas.
Sampling applies only to raster images; Tint applies only to vector images.

## Controlled TextField

`TextField` is controlled by a complete `TextEditingValue`. The owner should store the entire emitted value so selection and IME composition remain authoritative:

```cpp
auto value = UseState(TextEditingValue::FromText(""));

return TextField(value)
    .Placeholder("Name")
    .OnChanged([value](const TextEditingValue& next) {
      value = next;
    });
```

Single-line and multiline fields use the same component:

```cpp
TextField(value)
    .LineLimits(TextFieldLineLimits::MultiLine(3, 8))
    .MaxLength(200)
    .Placeholder("Message")
    .OnChanged([value](const TextEditingValue& next) {
      value = next;
    });
```

An unconstrained multiline field grows with its wrapped content. A maximum line count or a fixed parent height enables the internal viewport and keeps the caret visible. `MaxLength()` counts grapheme clusters rather than UTF-8 bytes or UTF-16 code units.

Secure input remains single-line, draws one mask glyph per grapheme, disables Copy and Cut, and requests native password behavior:

```cpp
TextField(password)
    .Secure()
    .MaxLength(64)
    .Placeholder("Password")
    .OnChanged([password](const TextEditingValue& next) {
      password = next;
    });
```

## Validation

Validation reports presentation state without filtering or mutating input:

```cpp
const ValidationResult result = Validate(
    email.Get().text,
    Required(),
    EmailAddress()
);

return TextField(email)
    .Placeholder("Email")
    .Validation(result)
    .OnChanged([email](const TextEditingValue& next) {
      email = next;
    });
```

Rules return valid, invalid, or pending results. Applications decide whether to validate on change, focus loss, or submission and can pass `ValidationResult::None()` before a field is touched.

## Submission actions

`TextInputAction::Next` submits and moves to the next focusable control without wrapping. `Done`, `Go`, `Search`, and `Send` submit through `OnSubmitted`; on mobile, terminal actions dismiss the soft keyboard. `Default` resolves to `Done` for single-line fields and `Newline` for multiline fields.

Android, iOS, macOS, and Windows use the same text-input session and command protocol. Platform adapters handle native IME lifecycle and coordinate conversion while the C++ runtime owns controlled value synchronization, selection, composition, undo, redo, and submission.

For protocol and platform details, see the [text input design](design/text-input.md).
