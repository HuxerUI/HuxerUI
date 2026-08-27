# Components and Input

Components are controlled declarations that emit typed events.
Use application state for authoritative values and let mounted behavior retain transient interaction state.

## Text, images, and drawing

`Text` accepts literals, `StringResource`, formatted values, and `State<T>`.
Use `TextRole` for theme typography or provide an explicit `TextStyle`.

`ImageVariant` covers `ImageResource`, `ImageAsset`, and `VectorAsset`.
`Image` also accepts `ExternalTexture` through a separate overload because a live platform texture is not an application image value.
Configure fit, alignment, sampling, and tint with typed methods.

`ImageAsset::FromEncoded(Bytes)` and `RawAsset::FromBytes(Bytes)` take ownership of encoded or arbitrary binary data.
Use `CopyEncoded(std::span<const std::byte>)` and `CopyBytes(std::span<const std::byte>)` when the source is borrowed.
The returned byte views remain standard immutable spans rather than introducing another view type.

`Canvas` and `Path` provide custom platform-neutral drawing that is replayed by every renderer.

## Buttons and selection controls

- `Button` emits `OnClick`.
- `IconButton` requires an icon and semantic label.
- `Checkbox`, `RadioButton`, and `Switch` receive a controlled Boolean and emit `OnChanged`.
- `Chip` supports an optional icon and controlled selected state.
- `SegmentedButton` receives items and a controlled selected index.
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
