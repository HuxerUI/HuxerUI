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
  Checkbox(checked).OnChanged([checked](bool value) {
    checked = value;
  }),
  Switch(checked).OnChanged([checked](bool value) {
    checked = value;
  }),
};
```

RadioButton represents one controlled choice rather than owning a group. Application state defines mutual exclusion, and activating an already selected RadioButton leaves the selection unchanged:

```cpp
auto choice = UseState(0);

return Row {
  RadioButton(choice == 0).OnChanged([choice](bool selected) {
    if (selected) {
      choice = 0;
    }
  }),
  RadioButton(choice == 1).OnChanged([choice](bool selected) {
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

Android, macOS, and Windows use the same text-input session and command protocol. Platform adapters handle native IME lifecycle and coordinate conversion while the C++ runtime owns controlled value synchronization, selection, composition, undo, redo, and submission.

For protocol and platform details, see the [text input design](design/text-input.md).
