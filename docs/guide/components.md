# Components and Input

Components are controlled declarations that emit typed events.
Use application state for authoritative values and let mounted behavior retain transient interaction state.

## Text, images, and drawing

`Text` accepts literals, `StringResource`, formatted values, and `State<T>`.
Use `TextRole` for theme typography or provide an explicit `TextStyle`.
Use `Align(TextAlign::...)` for horizontal paragraph alignment and `VerticalAlign(TextVerticalAlign::...)` when a framed Text should place its paragraph within extra height.
Vertical alignment does not change intrinsic text measurement.

`ImageVariant` covers `ImageResource`, `ImageAsset`, and `VectorAsset`.
`Image` also accepts `ExternalTexture` through a separate overload because a live platform texture is not an application image value.
Configure fit, alignment, sampling, and tint with typed methods.

`ImageAsset::FromEncoded(Bytes)` and `RawAsset::FromBytes(Bytes)` take ownership of encoded or arbitrary binary data.
Use `CopyEncoded(std::span<const std::byte>)` and `CopyBytes(std::span<const std::byte>)` when the source is borrowed.
The returned byte views remain standard immutable spans rather than introducing another view type.

`Canvas` and `Path` provide custom platform-neutral drawing that is replayed by every renderer.
Filled rectangles use `DrawRect()`, while lines, arcs, borders, and paths share `StrokeStyle` for width, caps, joins, miter limits, and optional dashes.
`Path::ArcTo()` continues an active contour with an endpoint-based elliptical arc, using explicit radii, x-axis rotation in radians, arc size, direction, and endpoint.

```cpp
return Canvas([](PaintContext& paint, Size size) {
  paint.DrawLine({0.0F, size.height * 0.5F}, {size.width, size.height * 0.5F}, Color::Black(),
                 StrokeStyle{.width = 2.0F, .cap = StrokeCap::Round, .dash_pattern = {8.0F, 4.0F}});
});
```

Connected arcs can participate in fills, strokes, shadows, and clips:

```cpp
Path sector;
sector.MoveTo({24.0F, 24.0F})
    .LineTo({44.0F, 24.0F})
    .ArcTo({20.0F, 12.0F}, 0.0F, ArcSize::Large, ArcDirection::Clockwise, {8.0F, 36.0F})
    .Close();
paint.FillPath(sector, Color::Black());
```

Use `Path::Contains(point, fill_rule)` when custom interaction must test the same local filled geometry.
Pass the fill rule used to paint the path; open contours receive the same implicit closing edge, and points on the boundary are included.
The query tests fill geometry rather than stroke width.

Dash lengths and offsets use the local logical units of the painted geometry.
An empty pattern is solid, entries alternate between painted and skipped lengths, and an odd-length pattern repeats to form an even cycle.
Each Path contour restarts the pattern; `DrawLine`, `DrawArc`, and `DrawBorder` define stable starting points so changing `dash_offset` is deterministic.
Use `DrawBorder()` to stroke rectangle geometry; `DrawRect()` intentionally remains a fill operation.

## Buttons and selection controls

- `Button` emits `OnClick`.
- `IconButton` requires an icon and semantic label.
- `Checkbox`, `RadioButton`, and `Switch` receive a controlled Boolean and emit `OnChanged`.
- `Chip` supports an optional icon and controlled selected state.
- `SegmentedButton` receives items and a controlled selected index.
- `Select` receives a finite item range and a controlled selected index.
- `Slider` receives a controlled value and can define range and step.
- `ProgressCircle` and `ProgressBar` support determinate and indeterminate presentation.

```cpp
[[huxerui::composable]]
View Controls() {
  auto enabled = UseState(false);
  auto volume = UseState(0.5F);

  return Column {
    Switch("Enabled", enabled).OnChanged([enabled](bool value) {
      enabled = value;
    }),
    Slider(volume)
        .Range(0.0F, 1.0F)
        .Step(0.05F)
        .OnChanged([volume](float value) {
          volume = value;
        }),
  }.With(Spacing(12.0F));
}
```

Disabled behavior is configured with the shared enabled modifier where supported.
Disabled components remain visible but do not emit activation or value-change events.

## Keyboard events

`KeyEvent::key` identifies a portable key independently of localized character text.
It distinguishes main-row and numeric-keypad keys, left/right modifiers, punctuation, international keys, and named function keys.
Use `text` only when the layout-resolved UTF-8 character matters; it may be empty, is not a replacement for TextField input, and is always empty on key release.

Use `ViewEvents::KeyDown` and `ViewEvents::KeyUp` for focused-View behavior that should run after the component's built-in key handling.
Return `true` when the event is handled so Runtime and the platform do not apply later defaults.
Return `false` to allow defaults such as focus traversal, activation, context menus, or native host behavior.

Use `ViewEvents::KeyIntercept` for a shortcut or parent policy that must run before a focused component.
Runtime visits the active focus-scope route from its root to the focused View and stops at the first handler that returns `true`; it does not bubble the event back through ancestors.

```cpp
return page.On<ViewEvents::KeyIntercept>([](const KeyEvent& event) {
  if (event.type == KeyEventType::Down && event.modifiers.control && event.key == Key::S) {
    Save();
    return true;
  }
  return false;
});
```

Prefer `OnClick` for ordinary control activation because it already unifies pointer, keyboard, and accessibility input.
Do not recreate Button, Select, Menu, or text-editing key behavior in an application handler.

## Select

`Select` is a compact controlled choice component for a finite, non-empty data set.
It receives an application-owned selected index and emits `OnChanged(std::size_t)` without changing that value internally.

```cpp
[[huxerui::composable]]
View DensityPicker() {
  auto selected = UseState<std::size_t>(1);
  const std::vector<std::string> options{"Compact", "Comfortable", "Spacious"};

  return Select(options, selected, [](const std::string& option) {
    return Text(option).Key(option);
  }).Label("Density")
    .OnChanged([selected](std::size_t index) {
      selected = index;
    });
}
```

The content factory supplies both the selected trigger content and each popup choice.
Its root View must publish a non-empty semantic label; `Text` already does so, while composite content should use `Semantics{.label = ...}` on its root.
Use `Label(...)` for the Select control's accessible name; the selected root label remains its read-only current value.
Apply `Enabled{false}` to that root for an individually disabled choice.
The root is one Select interaction target, so nested buttons or other independent controls are unsupported.

Use `.Key(...)` on the factory result when data can insert, remove, or reorder while the popup is open.
Without a key, item identity follows the current index, matching ordinary unkeyed sibling reconciliation.
The item set must not be empty and the selected index must remain in range; invalid declarations throw `std::invalid_argument`.

Up and Down move through enabled choices without wrapping, Home and End move to an enabled edge, and Enter or Space requests the active choice.
Opening focuses the selected choice when enabled, otherwise the first enabled choice; a list with no enabled choices has no active item.
Escape, Back, outside press, and selecting the already controlled value close the popup without emitting a duplicate change.
Disabling the Select while it is open closes the popup without emitting a change.
`Validation` presents application-owned validation state without changing selection rules.

## Tabs and indexed pages

`Tabs` is a controlled destination selector.
It does not own page state or page content.

```cpp
Tabs({"Overview", "Activity", "Settings"}, selected).OnChanged([selected](std::size_t index) {
  selected = index;
});
```

Use the indexed-page API when peer pages must retain local state while selection changes.
Use `NavigationStack` when destinations form a push and pop history.

## Application navigation

`NavigationBar` presents primary destinations horizontally.
`NavigationPane` presents the same destination model vertically and accepts `expanded = true` when labels should remain visible.

```cpp
const std::vector<NavigationItem> destinations{
    NavigationItem(home_icon, "Home"),
    NavigationItem(search_icon, "Search"),
};

return NavigationPane(destinations, selected, true).OnChanged([selected](std::size_t index) {
  selected = index;
});
```

`DrawerLayout` combines application content with an optional `StartDrawer`, `EndDrawer`, or both.
The application controls each drawer's open state.
Responsive persistent or modal presentation is selected from viewport width while keeping one declarative drawer subtree.

`TopAppBar` accepts a title, optional leading content, and action Views.
Applications decide which action opens a drawer; `DrawerLayout` does not search for or mutate an arbitrary button.

## NavigationStack

The factory form stores page factories so covered pages retain mounted state.
The typed-route form uses a controlled `NavigationPath<Route>` and a resolver that creates a page for each route value.

Route values are application data.
They may include typed parameters and can be encoded for Web URL history or application activation without introducing a separate screen registry.

Nested stacks use their nearest controller.
Capture or provide the intended root controller when an operation must replace a higher-level flow.

See [Navigation Design](../design/navigation.md) for controller, transition, Back, URL, and activation contracts.

## Gestures

Built-in controls and scrolling use the shared gesture arbitration system.
Custom content can observe clicks, repeated taps, long press, drag, and multi-pointer transform through typed gesture modifiers.

```cpp
return Canvas(content)
    .With(TransformGesture{})
    .On<TransformEvents::Changed>([=](const TransformEvent& event) {
      offset += event.pan;
      scale *= event.scale;
      rotation += event.rotation;
    });
```

Transform events report incremental pan, scale, and clockwise rotation around the current centroid.
The application retains the authoritative accumulated transform.
Gesture callbacks receive stable logical coordinates and explicit cancellation.
Do not combine raw pointer handling with a built-in recognizer to recreate the same state machine.

`PointerEvent::changed_button` identifies the button added by `Down` or removed by `Up`, while `pressed_buttons` is a `PointerButton` flag mask describing the complete post-event state.
Use `IsButtonPressed()` for chords and raw custom interaction; primary-button input alone participates in built-in click, selection, drag, and scrolling behavior.
Use `ViewEvents::Pointer` only when an application needs the complete raw Down, Move, Up, and Cancel lifecycle for the deepest eligible target.
It is a void notification; use `PointerIntercept` when custom recognition must take ownership of the sequence.
Attach `ViewEvents::ContextMenuRequested` when a View owns context actions instead of reconstructing a right-click sequence from raw Down.
The event reports a window-local position for secondary-button input and the focused View center for the Context Menu key or Shift+F10.
`ViewEvents::PointerIntercept` may take exclusive ownership of any button stream by returning `true`; pending built-in recognizers and the raw target receive `Cancel` when another participant wins.

Use `ViewEvents::Hover` for mouse or pen presence without joining pointer-sequence ownership:

```cpp
return content.On<ViewEvents::Hover>([hovered](const HoverEvent& event) {
  switch (event.type) {
  case HoverEventType::Enter:
  case HoverEventType::Move:
    hovered = true;
    break;
  case HoverEventType::Leave:
    hovered = false;
    break;
  }
});
```

`position` is local to the receiving View and `window_position` uses window logical coordinates.
Nested Views with Hover handlers each receive their own containment lifecycle; this is direct delivery, not event bubbling.
Disabled Views still receive Hover, while touch input does not create Hover events.
An exact duplicate pointer position does not emit another Move, but recomposition, layout, and presentation changes still resolve Enter or Leave under a stationary pointer.
A `PlatformView` owns hover over its native content.

For content that remains visible while hovered, set `State` on Enter or Move and clear it on Leave.
For content that appears only after departure, update that `State` on Leave instead.
For a delayed tooltip-like affordance, restart a lifecycle-bound `TaskHandle` on Enter and Move, await `Delay(500ms)`, and cancel the task and hide the affordance on Move or Leave as appropriate.
`Delay` resumes on the owning UI thread, so the task may update `State` directly without `Post`.
Hover deliberately has no separate stopped-moving event or built-in delay policy.

Use `PointerCursor` to declare a portable cursor without adding a pointer handler or retained extension:

```cpp
return Canvas(painter).With(PointerCursor(PointerCursorKind::Crosshair));
```

The deepest explicit declaration under a mouse or pen wins.
`PointerCursorKind::Default` explicitly restores the platform default for that region instead of inheriting an ancestor declaration.
Controls do not impose a cursor convention; apply the modifier where the application's interaction calls for one.
A cursor kind may come from `State`, so custom content can compute the appropriate kind and let ordinary local recomposition update the declaration.
Assigning the same state value does not recompose or resend the cursor.
Platforms map portable kinds to the closest native cursor, while a `PlatformView` keeps ownership of the cursor over its native content.
Platforms without a traditional pointer cursor may ignore the declaration.

`DragSource` transfers an immutable application value to one exact typed `DropTarget` while preserving the ordinary DragGesture recognition rules.
The application handles `DropEvents<T>::Dropped` to perform the authoritative data mutation.
An optional preview is ordinary View content presented in a non-interactive Layer.

```cpp
return CardView(card)
    .With(DragSource(CardTransfer{card.id}))
    .On<DragSourceEvents::Ended>([](const DragDropResult& result) {
      ReportDropResult(result.dropped);
    });
```

Use a long-press DragGesture configuration for reorder behavior inside scrolling content.
Drag-and-drop is in-process; native files, URLs, and PlatformView targets are not inferred from an application payload.
See [Typed Drag-and-Drop Design](../design/drag-drop.md) for ownership, target selection, preview, and auto-scroll behavior.

## TextField

`TextField` is controlled by a complete `TextEditingValue`, not only a string.
The value preserves text, selection, affinity, and active composition across updates.

```cpp
[[huxerui::composable]]
View NameField() {
  auto value = UseState(TextEditingValue{""});

  return TextField(value)
      .Label("Name")
      .Placeholder("Enter your name")
      .Variant(TextFieldVariant::Outlined)
      .OnChanged([value](TextEditingValue next) {
        value = next;
      });
}
```

Available variants are `Standard`, `Filled`, and `Outlined`.
The active theme decides whether the visual label is shown; its semantic label remains available to accessibility independently.

Use `LeadingIcon`, `TrailingIcon`, `LineLimits`, `MaxLength`, `Secure`, `InputConfiguration`, and `Validation` for typed configuration.
`TextFieldLineLimits::SingleLine()` is the default; multiline fields can define a minimum and optional maximum line count.
`Align(TextAlign::...)` applies horizontal alignment to editable text, placeholder, selection, caret, hit testing, and input-method geometry.
`VerticalAlign(TextVerticalAlign::...)` places the editable region within the field; single-line fields default to `Center` and multiline fields default to `Top`.

`OnChanged` reports the complete requested editing value.
`OnSubmitted` reports the submitted text when the configured platform action is performed.

## Validation and secure input

Validation communicates application-owned domain state.
It does not reject edits or replace input filtering.

Secure fields redact their text from semantics and platform accessibility while preserving editing behavior through the platform text-input bridge.

## Selection and clipboard

Static text selection, TextField selection, clipboard actions, and platform IME behavior share one Runtime editing model.
The platform adapter translates UTF-16 offsets or toolkit-specific conventions at the boundary while application text remains UTF-8.

For editing invariants and platform protocols, see [Text Input and TextField Design](../design/text-input.md).
