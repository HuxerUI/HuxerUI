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

To customize built-in semantic tokens while retaining that Theme's complete component mapping:

```cpp
template <class Factory>
View BrandTheme(Factory&& content) {
  ThemeSpec theme = MaterialLightThemeSpec();
  theme.colors.primary = Color::Rgb(130, 80, 210);
  theme.colors.on_primary = Color::White();
  return MaterialTheme(std::move(theme), std::forward<Factory>(content));
}
```

`FlatTheme(theme, content)` provides the same token-to-style rebuild path for a branded Flat Theme.

The Material theme maps stable Material 3 roles rather than copying a private token table into each control. Its color scheme includes surface-container levels, `on_surface_variant`, `outline`, and `secondary_container`; typography and shape schemes expose the roles used by the built-in controls and presentation surfaces. Material controls then resolve their own geometry and state treatment: Buttons use a 40-unit container, Chip uses a 32-unit outlined or selected tonal container, SegmentedButton joins equal-width 40-unit segments with a selected tonal container, Divider uses a one-unit line derived from the outline role, Checkbox, RadioButton, and Switch retain a 48-unit interaction target around their smaller visuals, and TextField defaults to the 56-unit Filled treatment, provides the Material Outlined treatment, and retains a Standard alternative for explicit cross-theme use. TextField owns its floating labels, decorative icons, and hover, focus, error, and disabled colors. Determinate ProgressCircle uses a round-capped separated track, while its indeterminate form uses the trackless six-second pulsing-arc motion. ProgressBar uses a separated track, stop indicator, and two-segment emphasized motion. Slider owns its split-track gap, narrow stateful handle, ticks, stop indicator, and component-specific disabled colors. Flat controls keep their denser geometry and simpler progress sweep while using the same public components and typed styles. A control disabled directly uses its component state colors. A disabled container instead applies one group-opacity boundary, so descendants retain their normal colors and are not dimmed twice.

Built-in Theme elevation shadows use a zero two-dimensional offset. Elevation controls their falloff through blur radius, while the shadow color controls opacity. An explicit `Shadow` modifier remains available when custom content needs a directional drop shadow.

## Indications

Interactive built-ins derive hover, focus, pressed, disabled, and ripple or state-overlay treatment from the nearest Theme. Pointer and keyboard activation share the same semantic state transitions. `ButtonStyle::indication` can replace the theme-wide indication when a filled container requires a different state-layer color. `ChipStyle::selected_indication` replaces its normal indication while selected and falls back to it when omitted. Component-owned transparent actions, such as Dialog actions and Menu items, likewise use the indication stored in their component style.

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

`Easing::Linear` keeps constant speed. `Easing::EaseIn` starts at zero speed and accelerates with a cubic curve, while `Easing::EaseOut` starts at its highest speed and decelerates to zero. Matching their endpoint speed with an intervening linear Tween allows a motion sequence to accelerate, cruise, and decelerate without a velocity jump.

Animation state is retained by the mounted node extension. Compatible recomposition retargets from the current presentation value rather than restarting from the previous declaration. Reduced-motion themes resolve animations immediately where appropriate.

Dialog, BottomSheet, Menu, and Toast use the same retained Layer transition machinery when their active style enables motion. Dialog resolves fade, scale, or slide policy from `DialogStyle`, while BottomSheet fades the modal barrier and translates its sheet from the bottom edge. Dismissal disables content input immediately and removes the retained layer only after its exit animation completes.

## Toast

Toast is a per-window root service:

```cpp
auto toast = UseToast();

return Button("Saved").OnClick([toast] {
  toast.Show("Saved");
});
```

Toast captures the current Environment when shown, draws above application content, passes input through, and dismisses after its configured duration. `ToastStyle` controls text and surface styling, width, viewport padding, top or bottom placement, shadow, and optional motion.

## Dialog

Declarative Dialog keeps visibility in application state:

```cpp
Button("Open").With(
    Dialog {
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
  dialog.Show("Network unavailable", "Check your connection and try again.");
});
```

The shortcut creates a standard themed Dialog with one default positive action. Supply positive and negative labels when both actions are needed:

```cpp
dialog.Show("Save changes?", "The current document has unsaved changes.", "Save", SaveDocument);

dialog.Show(
    "Delete item?",
    "This action cannot be undone.",
    "Delete",
    "Cancel",
    DeleteItem
);
```

Each built-in Theme installs complete Dialog, BottomSheet, Menu, and Toast styles in addition to its control styles. Presentation services resolve those typed values without branching on Theme identity. Popup remains an arbitrary anchored-content primitive and therefore does not impose a themed surface on its content.

`StringVariant` lets standard Dialog, Toast, and Menu values retain either direct text or `StringResource` until they are composed in the captured Environment. Empty standard Dialog action labels currently fall back to `OK` and `Cancel`; framework-owned localization remains part of the planned built-in resource bundle. `DialogStyle` owns the standard surface, typography, content geometry, positive and negative action appearance and indication, vertical placement, scrim, and optional `PresentationMotion`, so Material, Flat, and future platform themes can keep one semantic request while presenting it differently.

Modal layers trap focus and restore the previously focused node after its exit transition completes. The topmost modal layer controls outside-press dismissal and its scrim. Setting `dismiss_on_cancel` to `false` consumes Cancel without dismissing the presentation, so Back or Escape cannot close content behind it or leave the native window. `DialogStyle` and `BottomSheetStyle` independently define their scrim and motion, while BottomSheet also owns its surface, maximum width, per-corner shape, drag handle, and shadow.

For lifecycle and rendering details, see the [architecture design](design/architecture.md).

## Typed presentation services

Command-oriented, per-window services are the primary API for temporary presentation.

Custom Dialog content remains available when the standard title, message, and action model is not sufficient:

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

BottomSheet is a separate typed service because its bottom placement, adaptive width, surface, slide motion, and drag behavior differ from Dialog. A visible theme drag handle captures downward movement, follows the pointer, settles after a short or cancelled drag, and uses the same `on_dismiss_request` contract after crossing its distance threshold. A callback that keeps the layer visible causes the sheet to settle back to the edge:

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
      menu.Show({
          MenuItem("Rename", [] {}),
          MenuItem(
              app_resources::images::move,
              app_resources::strings::move_to,
              {
                MenuItem("Archive", [] {}),
                MenuSection{},
                MenuItem("Trash", [] {}),
              }
          ),
      });
    });
```

Popup exposes arbitrary anchored content and configurable outside-press and focus behavior. `AnchorPlacement` separates the preferred side from cross-axis alignment; `gap`, `offset`, viewport clamping, and automatic opposite-side fallback complete the platform-neutral positioning contract without Android-specific Gravity terminology. Point-based `ShowAt()` supports context menus without a View anchor.

Menu accepts a recursive sequence of semantic `MenuItem` values rather than an arbitrary View factory. An action item ends in a callback, while a submenu item ends in another entry sequence using the same syntax. Optional leading images accept either `ImageResource` or `ImageAsset`, and labels accept either ordinary text or `StringResource`; resource values resolve while the captured layer Environment is composed. `MenuSection{}` marks a logical boundary between items without wrapping them in another container. `MenuStyle::separator_mode` lets a theme omit separators or place them at section boundaries or between all items, while the separator color, thickness, and padding remain ordinary typed style values. The built-in Material theme omits separators while Flat themes place them between items. Leaf actions automatically dismiss the complete open menu chain, and submenu items can be opened repeatedly from the same declaration.

Only the root menu owns the transparent outside-press barrier. Submenus are content-only anchored layers, which keeps every visible ancestor interactive and lets sibling submenus replace one another without blocking their parent. Back closes the deepest open level, the default outside-press behavior or a leaf action closes the complete chain, and focus is restored when the root closes. A custom `on_dismiss_request` remains a request callback and decides whether to close the menu. Disabled and checked items, Material ripple, and Flat state-overlay feedback are provided consistently by the framework. Arbitrary custom anchored content belongs in Popup.

Menu surfaces use the widest item's natural width plus optional themed content padding, subject to `MenuStyle::minimum_width` and viewport constraints. The built-in Flat and Material themes keep items flush with the surface while retaining each item's internal padding and hit area. Material uses a 112-unit minimum surface width and a 48-unit minimum item height for touch-friendly compact menus, while Flat retains denser desktop metrics. The surface clips item drawing and hit testing to its rounded bounds, so edge-to-edge hover and pressed feedback cannot escape the first or last corner. Set `MenuOptions::width` only when one menu needs an explicit surface width. Items without an image or checked marker do not reserve a hidden leading slot, and section separators stretch only after the surface width has been resolved.

`MenuStyle::item_indication` controls item hover and pressed feedback independently from ordinary Buttons. `MenuStyle::motion` controls root and submenu fade, scale, or slide treatment. An empty optional motion attaches no retained animation extension, while Material themes use a short scale-and-fade transition.

Each Popup or Menu handle owns at most one active entry, so calling `Show()` or `ShowAt()` again replaces its previous entry. `PopupContext` can dismiss arbitrary popup content directly; Menu actions dismiss automatically and do not expose layer identity to the item model.

All typed handles capture the current Environment when obtained and can be retained by event callbacks. Dialog, BottomSheet, Popup, and Menu share one internal LayerController and LayerStack; their separate `UseXxx()` names express user-facing semantics rather than separate runtimes or rendering paths.

The API does not add `UsePresentation()`, expose a public generic Modal mode, or require temporary presentation to be declared as ordinary application content. Toast, Dialog, BottomSheet, Popup, and Menu are window-level entries mounted outside the application root while retaining caller Environment values.

## Debug overlay

`AppOptions::show_debug_overlay` controls a persistent built-in System layer. It defaults to enabled in Debug builds and disabled in Release builds, and Runtime installs it after application RootHooks. A compact `DEBUG` corner ribbon toggles an upper-left performance panel showing painted-frame rate, average and maximum frame-commit time, process CPU utilization, process-memory footprint, average damaged area, and viewport size. CPU utilization is normalized across the platform-reported logical processor count.

The ribbon, panel, metric cards, text, layout, styling, and interaction are ordinary HuxerUI Views in their own layer scope. The ribbon is one rotated component whose ends are clipped by the viewport, so its background, label, shadow, and interaction state share one transform. Runtime records frame metrics without introducing a second scheduler, while each native adapter optionally supplies cumulative process CPU time and its preferred current process-memory footprint. Opening the panel does not recompose the application root, transparent System-layer regions do not block application input, and closing the panel stops timed process sampling while leaving the ribbon mounted.
