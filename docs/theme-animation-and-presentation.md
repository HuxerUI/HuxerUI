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

Modal layers trap focus and restore the previously focused node when dismissed. The topmost modal layer controls outside-press dismissal and its scrim. Setting `dismiss_on_cancel` to `false` consumes Cancel without dismissing the presentation, so Back or Escape cannot close content behind it or leave the native window. BottomSheet resolves its scrim from the captured `ThemeSpec`; `DialogStyle` affects Dialog only.

For lifecycle and rendering details, see the [architecture design](design/architecture.md).

## Typed presentation services

Command-oriented, per-window services are the primary API for temporary presentation.

Dialog remains explicit and ergonomic rather than being replaced by a generic presentation mode:

```cpp
auto dialog = UseDialog();

return Button("Delete").OnClick([dialog] {
  dialog.Show([](DialogContext context) {
    return Column {
      Text("Delete this item?"),
      Button("Cancel").OnClick([context] {
        context.Dismiss();
      }),
    };
  });
});
```

BottomSheet is a separate typed service because bottom placement, sizing, motion, and future drag behavior differ from Dialog:

```cpp
auto bottom_sheet = UseBottomSheet();

return Button("Actions").OnClick([bottom_sheet] {
  bottom_sheet.Show([](BottomSheetContext context) {
    return Column {
      Text("Actions"),
      Button("Close").OnClick([context] {
        context.Dismiss();
      }),
    };
  });
});
```

Popup and Menu bind an anchor through a retained modifier and show content from the event that opens it:

```cpp
auto popup = UsePopup();

return Button("Account")
    .With(popup.Anchor())
    .OnClick([popup] {
      popup.Show([](PopupContext context) {
        return Button("Close account popup").OnClick([context] {
          context.Dismiss();
        });
      });
    });
```

```cpp
auto menu = UseMenu();

return Button("More")
    .With(menu.Anchor())
    .OnClick([menu] {
      menu.Show([](MenuContext context) {
        return Button("Rename").OnClick([context] {
          context.Dismiss();
        });
      });
    });
```

Popup exposes arbitrary anchored content and configurable outside-press and focus behavior. Menu reuses its positioning foundation while adding a transparent barrier, focus containment, focus restoration, and menu-oriented dismissal policy. Each Popup or Menu handle owns at most one active entry, so calling `Show()` or `ShowAt()` again replaces its previous entry. Their typed contexts dismiss the current entry directly, so application state does not need to retain a `LayerId` or recompose solely to close transient content. The supplied content owns its surface and item styling. Point-based `ShowAt()` supports context menus without a View anchor.

All typed handles capture the current Environment when obtained and can be retained by event callbacks. Dialog, BottomSheet, Popup, and Menu share one internal LayerController and LayerStack; their separate `UseXxx()` names express user-facing semantics rather than separate runtimes or rendering paths.

The API does not add `UsePresentation()`, expose a public generic Modal mode, or require temporary presentation to be declared as ordinary application content. Toast, Dialog, BottomSheet, Popup, and Menu are window-level entries mounted outside the application root while retaining caller Environment values.

## Debug overlay

`AppOptions::show_debug_overlay` controls a persistent built-in System layer. It defaults to enabled in Debug builds and disabled in Release builds, and Runtime installs it after application RootHooks. A compact `DEBUG` corner ribbon toggles an upper-left performance panel showing painted-frame rate, average and maximum frame-commit time, process CPU utilization, process-memory footprint, average damaged area, and viewport size. CPU utilization is normalized across the platform-reported logical processor count.

The ribbon, panel, metric cards, text, layout, styling, and interaction are ordinary HuxerUI Views in their own layer scope. The ribbon is one rotated component whose ends are clipped by the viewport, so its background, label, shadow, and interaction state share one transform. Runtime records frame metrics without introducing a second scheduler, while each native adapter optionally supplies cumulative process CPU time and its preferred current process-memory footprint. Opening the panel does not recompose the application root, transparent System-layer regions do not block application input, and closing the panel stops timed process sampling while leaving the ribbon mounted.

