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

## Button, Checkbox, and Switch

Button, Checkbox, and Switch participate in focus traversal and share their pointer and keyboard activation paths. Checkbox and Switch are controlled:

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

Indeterminate progress advances through retained animation state. Reduced motion themes keep it static.

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
