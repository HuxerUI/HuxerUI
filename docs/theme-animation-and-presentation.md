# Theme, Animation, and Presentation

## Theme providers

HuxerUI includes Flat and Material light and dark themes:

```cpp
View App() {
  return MaterialTheme(AppContent);
}
```

Providers can be nested to form complete theme boundaries:

```cpp
return Column {
  HUXERUI_THEME(MaterialDarkTheme, DarkContent()),
  HUXERUI_THEME(FlatTheme, FlatContent()),
};
```

`HUXERUI_THEME` is optional syntax sugar for an inline View expression or a component call with arguments. Theme functions also accept a component factory directly.

`ThemeSpec` contains semantic color, typography, shape, spacing, elevation, motion, and interaction tokens. Component styles use the same Environment mechanism and can be overridden for one subtree:

```cpp
template <class Factory>
View AccentTheme(Factory&& content) {
  ThemeDefinition definition;
  definition.Set(ButtonStyle{
      .background = Color::Rgb(207, 34, 46),
      .label_style = TextStyle{Font::System(14.0F), Color::White()},
      .padding = EdgeInsets::Symmetric(16.0F, 8.0F),
      .corner_radius = 12.0F,
  });
  return Theme(std::move(definition), std::forward<Factory>(content));
}
```

Explicit modifiers such as `Background`, `Foreground`, and `FontSize` are applied after Theme resolution and therefore win.

To customize Material semantic tokens while retaining Material component mapping:

```cpp
template <class Factory>
View BrandTheme(Factory&& content) {
  ThemeSpec theme = MaterialLightThemeSpec();
  theme.colors.primary = Color::Rgb(130, 80, 210);
  theme.colors.on_primary = Color::White();
  return MaterialTheme(std::move(theme), std::forward<Factory>(content));
}
```

## Indications

Interactive built-ins derive hover, focus, pressed, disabled, and ripple or state-overlay treatment from the nearest Theme. Pointer and keyboard activation share the same semantic state transitions.

An explicit `Indication` modifier can replace the default interaction visual for a custom control. `NoIndication` disables it deliberately.

## Presentation animation

`Offset`, `Opacity`, `Scale`, and `Rotation` accept immediate values or `AnimateTo()` targets:

```cpp
auto transformed = UseState(false);

return Button("Transform")
    .With(
        Scale{
            AnimateTo(
                transformed ? 1.2F : 1.0F,
                TweenSpec{0.24, Easing::EaseOut}
            )
        },
        Rotation{
            AnimateTo(
                transformed ? 12.0F : 0.0F,
                SpringSpec{}
            )
        }
    )
    .OnClick([transformed] {
      transformed = !transformed;
    });
```

Presentation transforms do not change measured size or parent layout. They transform the View background, content, children, foreground extensions, clipping, and pointer hit region together. `TransformOrigin` uses normalized coordinates.

Animation state is retained by the mounted node extension. Compatible recomposition retargets from the current presentation value rather than restarting from the previous declaration. Reduced-motion themes resolve animations immediately where appropriate.

## Toast

Toast is a per-window root service:

```cpp
auto toast = UseToast();

return Button("Saved").OnClick([toast] {
  toast.Show("Saved");
});
```

Toast captures the current Environment when shown, draws above application content, passes input through, and dismisses after its configured duration.

## Dialog

Declarative Dialog keeps visibility in application state:

```cpp
Button("Open").With(
    Dialog{
        .visible = visible,
        .content = ConfirmDialog,
        .dismiss_on_outside_press = true,
        .on_dismiss_request = [visible] {
          visible = false;
        },
    }
);
```

Command-oriented presentation uses the per-window Dialog service:

```cpp
auto dialog = UseDialog();

return Button("Open").OnClick([dialog] {
  dialog.Show(ConfirmDialog);
});
```

Modal layers trap focus and restore the previously focused node when dismissed. The topmost modal layer controls outside-press dismissal and its scrim.

For lifecycle and rendering details, see the [architecture design](design/architecture.md).

