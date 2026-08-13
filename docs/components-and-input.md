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

Keyboard-visible focus uses `InteractionScheme::focus_ring`, `focus_ring_width`, and `focus_ring_offset`. The offset is the clear gap between the control and an outside ring; it affects paint bounds without changing measurement or layout. Material uses a 3-unit ring with a 2-unit gap, while Flat uses a 2-unit ring with the same gap.

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

Checkbox, RadioButton, and Switch also retain their label-free constructors for custom composition. A labeled control owns its label, uses the Theme spacing between the visual control and text, and treats the complete control as one focusable and clickable target. Checkbox paints its checked marker from the framework's tintable vector resource, so its geometry does not depend on the platform font.

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

## IconButton

IconButton is the standard momentary action when the visible content is only an icon:

```cpp
IconButton(app::images::search, "Search").OnClick(OpenSearch);

IconButton(vector_icon, "Unavailable action")
    .OnClick(PerformAction)
    .With(Enabled(false));
```

The semantic label is required and is not drawn.
IconButton accepts image resources, raster assets, and vector assets, participates in focus traversal, and emits the same `ViewEvents::Click` event as Button.
`IconButtonStyle` independently owns icon size, minimum interactive size, state-layer size, corner radius, colors, and indication.
Material uses a 24-unit icon inside a 48-unit interaction target with a 40-unit circular state layer, while Flat uses denser 20-, 40-, and 32-unit geometry.
Vector icons follow the style foreground, while raster assets preserve their encoded colors and use the Theme's disabled opacity.
IconButton is intentionally not selectable; use a controlled component such as Chip when the action has persistent selected state.

## Tooltip

Tooltip is a retained modifier because it describes supplementary information for an existing target rather than introducing a layout component:

```cpp
IconButton(app::images::search, "Search")
    .OnClick(OpenSearch)
    .With(Tooltip("Search"));
```

It appears after the active Theme's hover delay, immediately for keyboard-visible focus, or after a touch long press.
Moving from the target onto the tooltip surface keeps it visible, while leaving both regions uses the short exit delay.
A recognized long press cancels the target activation and keeps the tooltip visible for the Theme's touch duration.
Escape or Back dismisses the active tooltip, and only one tooltip is presented in a window at a time.
Disabled targets still expose their tooltip through pointer hover but do not become focusable or accept touch input.

The message contributes a semantic hint to the target, while the visual tooltip surface is hidden from accessibility to avoid duplicate announcements.
`TooltipStyle` owns its surface, text, geometry, placement, and timing, and can be overridden as an ordinary typed Theme value.
Tooltip deliberately accepts plain `StringVariant` content only; use Popup when an anchored surface needs rich or interactive content.

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
Chip(app::images::filter, "Filters").OnClick(OpenFilters);

Chip(vector_icon, "Selectable", selected)
    .OnChanged([selected](bool value) {
      selected = value;
    });
```

`OnChanged` delegates to `On<ToggleEvents::Changed>`. Both forms participate in focus traversal and use the active Theme's indication and component style. `ChipStyle` owns the icon size and spacing. Vector icons follow the current label color, while raster assets preserve their encoded colors. Use `Enabled(false)` for a disabled Chip. Chip intentionally retains a visible label; use IconButton when an action should be icon-only.

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
        SegmentedButtonItem(app::images::grid, "Grid"),
        SegmentedButtonItem::IconOnly(app::images::map, "Map"),
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
        TabItem(app::images::home, "Home"),
        TabItem::IconOnly(app::images::search, "Search"),
        std::move(TabItem("Reports")).Enabled(false),
    },
    selected
).OnChanged([selected](std::size_t index) {
  selected = index;
});
```

The semantic label of an icon-only item is required but not drawn. Left and Right move with wrapping, Home and End move to the first and last enabled item, and every keyboard path skips disabled items. More tabs than the available width scroll horizontally, and a newly selected item is revealed automatically.

Tabs publishes one TabList collection whose real retained item nodes expose the Tab role, accessible label, zero-based collection index, selected state, enabled state, and Activate action.
The visual label and icon do not create duplicate semantic descendants.
Selection changes preserve semantic item identity while updating the controlled selected state.

`TabsStyle` owns label, indicator, and divider appearance; item metrics; indication; indicator motion; and the theme's width policy. Flat tabs keep their content widths and use an item-wide indicator. Material primary tabs divide available width equally until their natural content needs horizontal scrolling, use a 3 dp content-wide indicator with a 24 dp minimum width, and draw the standard divider when the row does not overflow. Tabs does not mount, cache, or transition page content; those responsibilities belong to a future navigation container rather than this selection control.

## TopAppBar

TopAppBar presents a required title with optional leading content and trailing action Views:

```cpp
return TopAppBar(
    "Library",
    IconButton(images::menu, "Open navigation").OnClick(OpenNavigation),
    {
        IconButton(images::search, "Search").OnClick(OpenSearch),
        IconButton(images::more, "More actions").OnClick(OpenActions),
    }
);
```

The title is a StringVariant owned by TopAppBar, while leading and action slots remain ordinary Views that own their events, enabled state, focus, semantics, and visual customization.
TopAppBar does not synthesize Back, drawer, or overflow actions and does not depend on NavigationController.
Use `TitleAlignment(TopAppBarTitleAlignment::Center)` for a center-aligned small bar.

TopAppBar keeps leading and actions vertically centered, constrains the title to the remaining single-line width, and clips action content to the bar when a caller supplies more than the available width.
Applications should normally expose no more than three direct actions and place secondary operations in an explicit Menu anchored to the final IconButton.
Automatic overflow would require a structured command model and is not inferred from arbitrary action Views.

TopAppBarStyle owns the container background, title style, fixed content height, edge padding, title inset, and slot spacing.
Material uses a 64-unit surface container and Flat uses a denser 48-unit surface; neither theme adds a static shadow.
Slot Views retain their own component styles, so an application may explicitly color a leading IconButton when its design differs from the ordinary action color.

## NavigationBar and NavigationPane

NavigationBar and NavigationPane are controlled destination selectors.
They emit a selected index but do not own page history or construct destination content:

```cpp
auto selected = UseState<std::size_t>(0);

return NavigationBar(
    {
        NavigationItem(images::home, "Home"),
        NavigationItem(images::search, "Search"),
        NavigationItem(images::settings, "Settings"),
    },
    selected
).OnChanged([selected](std::size_t index) {
  selected = index;
});
```

NavigationPane uses the same NavigationItem model and is compact by default.
NavigationBar and compact NavigationPane items require icons because their labels are secondary or semantic rather than the only visible content.
Expanded NavigationPane items may be label-only.
Pass `true` as the third constructor argument to show the icon-and-label form:

```cpp
NavigationPane(items, selected, true)
    .OnChanged([selected](std::size_t index) {
      selected = index;
    });
```

NavigationItem accepts ImageResource, ImageAsset, or VectorAsset icons, supports a selected icon, and can be disabled.
Arrow keys move along the control's axis, Home and End select an edge item, and disabled items are skipped.
NavigationBarStyle and NavigationPaneStyle keep geometry, selection indication, colors, and motion in Theme.

Both controls publish one Navigation collection containing Button items with accessible labels, zero-based collection indices, selected state, enabled state, and Activate actions.
Compact and expanded NavigationPane presentations retain the same semantic structure and item identity.
Internal icons, indicators, and visible labels are excluded from item descendants because each item already provides its complete accessible name.

## DrawerLayout

DrawerLayout keeps its main content and optional logical-edge drawers in the application tree:

```cpp
auto start_open = UseState(false);
auto end_open = UseState(false);

return DrawerLayout {
  MainContent(),
  StartDrawer {
    StartContent(),
  }.Open(start_open).OnOpenChanged([start_open](bool open) {
    start_open = open;
  }),
  EndDrawer {
    EndContent(),
  }.Open(end_open).OnOpenChanged([end_open](bool open) {
    end_open = open;
  }),
};
```

Open is the controlled state used whenever a drawer is modal.
Buttons request modal opening by updating that state, and Back emits DrawerEvents::OpenChanged so the owner supplies the next value.
DrawerLayout presents both drawers modally in Compact viewports, keeps Start persistently inline and End modal in Medium viewports, and keeps both persistently inline in Expanded viewports.
Persistent inline drawers are visible regardless of Open and do not mutate it, so resizing back to a modal structure preserves the owner's requested state.
Modal drawers support edge dragging and scrim dismissal, animate above content, use the theme-owned modal shape and shadow, and confine focus while open.
Inline drawers participate in layout without a scrim, focus trap, corner radius, shadow, or Back handling.
When the available width cannot preserve the configured minimum content width, End falls back to modal placement before Start.
A drawer that falls back to modal placement is visible only when its controlled Open state is true.
If both controlled states are open when a viewport becomes Compact, the modal drawers stack with End above Start and Back closes them in that order.
DrawerStyle owns preferred and minimum drawer widths, minimum content width, modal reveal, background, scrim, shadow, shape, gesture edge, and motion.
Start and End describe logical edges so future right-to-left direction support does not require a new public drawer API.

Use DrawerLayout for application content that participates in ordinary layout and Environment ownership.
Use Dialog, BottomSheet, Popup, Menu, or another presentation service for content owned by the window LayerStack.

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

## BarChart and DonutChart

BarChart and DonutChart are data-driven Canvas components that share ChartDataPoint:

```cpp
BarChart({
    {"Jan", 24.0F},
    {"Feb", 42.0F, Color::Rgb(5, 150, 105)},
    {"Mar", 31.0F},
}).With(Frame{.height = 240.0F});

DonutChart(
    {
        {"Desktop", 60.0F},
        {"Mobile", 30.0F},
        {"Tablet", 10.0F},
    },
    DonutChartOptions{
        .center_label = "Sessions",
        .accessibility_label = "Traffic sources",
    }
).With(Frame{.height = 260.0F});
```

Each data point requires a non-empty label and a finite non-negative value. Its optional color overrides the theme-aware default palette. BarChart supports an explicit maximum, grid density, bar width, corner radius, and value-label visibility. DonutChart supports inner radius, segment gap, center label, and responsive legend configuration. A DonutChart total must be positive.

Mouse and pen hover highlight the active bar, donut segment, or legend row and show a compact label, value, and percentage card. Hover state is retained in a NodeExtension, so moving between data points rerecords only the chart foreground instead of recomposing or repainting the base Canvas. Set `show_hover_info` to false in either options type when the chart is purely decorative.

Both components publish Image semantics with the configured accessibility label and a complete label-value summary. They have no intrinsic size, matching Canvas; use Frame, Grow, or bounded parent constraints. Data remains controlled by the application, and recomposition with new data rerecords only the chart's PaintSequence.

## Image

Image displays raster ImageAsset values, vector VectorAsset values, or an ImageResource that resolves either format automatically:

```cpp
Image(app::images::logo)
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
    .Label("Name")
    .Placeholder("Enter your name")
    .OnChanged([value](const TextEditingValue& next) {
      value = next;
    });
```

`Label()` and `Placeholder()` are independent.
An empty unfocused field displays the label in the input line and hides the placeholder.
Focus or non-empty text moves the label to the selected variant's floating position, and an empty focused field then displays the placeholder as the editing hint.
Omitting `Label()` preserves the ordinary placeholder-only behavior.
Material defaults to the 56-unit Filled variant with a bottom state indicator.
Flat defaults to the compact Standard variant.
All three variants can be selected explicitly:

```cpp
TextField(value)
    .Label("Repository")
    .Variant(TextFieldVariant::Standard);
```

Standard uses a transparent container and bottom state indicator.
Filled adds a top-rounded container fill to the same indicator geometry, while Outlined uses a complete outline interrupted by its floating label.

Leading and trailing icons accept `ImageResource`, `ImageAsset`, or `VectorAsset`:

```cpp
TextField(value)
    .Label("Account")
    .Placeholder("Email or username")
    .LeadingIcon(account_icon)
    .TrailingIcon(status_icon);
```

TextField icons are decorative and do not create separate pointer, keyboard, or accessibility actions.
Their independent leading and trailing sizes, spacing, and state colors come from `TextFieldStyle`.
Vector icons follow the enabled, focused, error, and disabled TextField colors, while raster assets preserve their encoded colors.

An ordinary TextField publishes its committed value and normalized UTF-16 selection to the semantic frame.
Accessibility SetText and SetSelection actions use the same controlled `OnChanged` flow, reducer, limits, layout invalidation, and active native-input synchronization as other edits.
A read-only field keeps selection available but rejects replacement, while a secure field exposes neither its value nor selection and never advertises SetSelection.

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
    .Label("Password")
    .Placeholder("Enter password")
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
    .Label("Email")
    .Placeholder("name@example.com")
    .Validation(result)
    .OnChanged([email](const TextEditingValue& next) {
      email = next;
    });
```

Rules return valid, invalid, or pending results. Validation messages are StringVariant values resolved when TextField is composed; `Required()` and `EmailAddress()` use localized framework resources, while passing a literal or StringResource overrides either default. Applications decide whether to validate on change, focus loss, or submission and can pass `ValidationResult::None()` before a field is touched.

## Submission actions

`TextInputAction::Next` submits and moves to the next focusable control without wrapping. `Done`, `Go`, `Search`, and `Send` submit through `OnSubmitted`; on mobile, terminal actions dismiss the soft keyboard. `Default` resolves to `Done` for single-line fields and `Newline` for multiline fields.

Android, iOS, macOS, and Windows use the same text-input session and command protocol. Platform adapters handle native IME lifecycle and coordinate conversion while the C++ runtime owns controlled value synchronization, selection, composition, undo, redo, and submission.

For protocol and platform details, see the [text input design](design/text-input.md).
