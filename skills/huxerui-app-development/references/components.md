# Built-in Components

This is a decision-oriented catalog, not a replacement for the active SDK's public headers. Verify signatures before editing code.

## Content primitives

| Component | Important contract |
| --- | --- |
| `Text` | Takes `StringVariant` and optional `TextRole`; `.Style(TextStyle)` overrides text styling. `.Align(TextAlign)` and `.VerticalAlign(TextVerticalAlign)` place the paragraph inside its text rectangle. `Text::Format` supports literal and resource formats. |
| `Image` | Takes `ImageVariant` or `ExternalTexture`; `.Fit`, `.Align`, `.Sampling`, and `.Tint` are component-specific. |
| `Canvas` | Takes a `CanvasPainter` and paints in the size assigned by layout. |
| `Divider` | Horizontal by default; pass `Axis::Vertical` only when height is bounded. |
| `SelectionArea` | Wraps content that participates in text selection. |

Use the complete container, scrolling, virtualization, navigation-shell, and responsive-layout guidance in [layout-and-ui.md](layout-and-ui.md).

## Actions and choices

| Component | Controlled value and events |
| --- | --- |
| `Button(label)` | Emits click through `.OnClick(...)`; the constructor has no separate enabled state, so use `Enabled`. |
| `IconButton(icon, semantic_label)` | Requires an accessible semantic label and emits click. |
| `Chip(label[, selected])` | Optional controlled selection; `.OnChanged(bool)` requests a new value. Icon overloads are available. |
| `Checkbox([label,] checked)` | Controlled `bool`; `.OnChanged(bool)` requests a new value. |
| `RadioButton([label,] selected)` | Controlled `bool`; group exclusivity remains application-owned. |
| `Switch([label,] checked)` | Controlled `bool`; `.OnChanged(bool)` requests a new value. |
| `SegmentedButton(items, selected_index)` | Controlled index; items may have icon/label or icon-only with semantic label. |
| `Tabs(items, selected_index)` | Controlled index; `TabItem::Enabled` disables individual destinations. Page content is separately owned. |
| `Slider(value)` | Controlled `float`; configure `.Range` and optional `.Step`, then write `.OnChanged` values back. |

Do not rely on constructor overloads that accept `State<T>` to mutate state automatically; they read the current value. Bind `OnChanged` explicitly.

## Input and progress

- `TextField(TextEditingValue)` is controlled by the complete editing value. Configure `.Label`, `.Placeholder`, icons, `.Variant`, `.LineLimits`, `.MaxLength`, `.Validation`, `.Secure`, `.Align(TextAlign)`, `.VerticalAlign(TextVerticalAlign)`, and `.InputConfiguration`, then handle `.OnChanged` and optionally `.OnSubmitted`. Alignment applies to the editable paragraph, not the TextField View's placement.
- `TextFieldVariant` currently contains `Filled`, `Outlined`, and `Standard`.
- `ProgressCircle()` and `ProgressBar()` are indeterminate. Their `float` constructors are determinate.

## Common review points

- Add an accessible label to icon-only actions.
- Keep selected, checked, text, and progress values controlled.
- Do not hardcode Material assumptions when the current theme can be Flat or custom.
- Give `PlatformView`, `Canvas`, and vertical `Divider` meaningful constraints.
