# Theme, Animation, and Presentation

## Theme

`ThemeSpec` groups color, typography, shape, spacing, elevation, motion, interaction, and semantic brightness. Focus-ring policy belongs to its interaction scheme. Start with the built-in visual system that matches the application:

```cpp
return MaterialTheme {
  AppContent(),
};
```

The complete built-in boundaries are `FlatTheme`, `FlatDarkTheme`, `MaterialTheme`, and `MaterialDarkTheme`. Do not recreate one with `ThemeDefinition{ThemeSpec}`. That constructor establishes a new complete theme boundary but does not install the Flat or Material component-style mappings.

Use this order:

- Use a built-in theme directly when its defaults fit.
- Modify a built-in `ThemeSpec` for brand colors, typography, shapes, spacing, elevation, motion, or interaction tokens.
- Override a complete typed component style only when tokens cannot express the component-specific change.
- Define a new typed style for an app-side component only when the component owns genuinely new visual policy.
- Build a new theme system only when the application intentionally does not use Flat or Material.

`UseTheme()` reads the current `ThemeSpec`. Do not hardcode a Material visual into app components that must work under Flat or custom themes.

## System appearance

`UseSystemThemeBrightness()` from `<huxerui/environment.h>` reads and observes the current window host's effective `ThemeBrightness::Light` or `Dark` preference during composition. It has no ordered state slot and may be read conditionally. A nested Theme does not override the host preference; hosts without a preference use Light.

Keep follow-system versus explicit light/dark selection in application state. For example, use an optional override in the application root:

```cpp
auto theme_override = UseState(std::optional<ThemeBrightness>{});
const std::optional<ThemeBrightness>& selected = theme_override;
const ThemeBrightness brightness = selected ? *selected : UseSystemThemeBrightness();
ThemeSpec theme = brightness == ThemeBrightness::Dark
    ? MaterialDarkThemeSpec() : MaterialLightThemeSpec();
return MaterialTheme {
  theme,
  SettingsContent(theme_override),
};
```

The application-owned settings content assigns `std::nullopt` for System or an explicit brightness. An explicit override stops the system subscription; returning to System reads the latest preference. Use a shared application model when several windows should share this selection. HuxerUI provides no theme-selection policy enum or persistence.

`ThemeSpec::brightness` describes the selected theme; changing it alone does not transform its palette. Start from the matching light/dark factory, then customize tokens. It is distinct from `SystemBarContentBrightness`, which describes system icon/text foreground contrast. Theme selection does not automatically recolor native title bars or embedded platform controls.

## Customize built-in tokens

Use `FlatLightThemeSpec`, `FlatDarkThemeSpec`, `MaterialLightThemeSpec`, or `MaterialDarkThemeSpec` as the mutable starting point. Pass the result to `FlatTheme` or `MaterialTheme`; the selected theme system then rebuilds its component styles from those tokens.

```cpp
ThemeSpec theme = MaterialLightThemeSpec();
theme.colors.primary = Color::Rgb(32, 96, 180);
theme.shapes.medium = 10.0F;

return MaterialTheme {
  theme,
  AppContent(),
};
```

For a branded dark Material theme, customize `MaterialDarkThemeSpec()` and still pass it to `MaterialTheme`. The `MaterialDarkTheme` convenience is for the unmodified built-in dark definition. Flat follows the same pattern.

`primary_container`/`on_primary_container` and `tertiary_container`/`on_tertiary_container` are tonal surface/foreground pairs, distinct from the stronger primary and tertiary pairs. Customize both sides together to preserve contrast in light and dark themes.

## Typed style catalog

Component styles are typed Environment values. Verify their current fields in the active SDK headers before constructing a replacement.

| Area | Typed styles | Public header |
| --- | --- | --- |
| Text and controls | `TextStyle`, `ButtonStyle`, `IconButtonStyle`, `ChipStyle`, `DividerStyle`, `SegmentedButtonStyle`, `TabsStyle`, `TreeViewStyle`, `SelectStyle`, `TextFieldStyle`, `ComboBoxStyle`, `CheckboxStyle`, `RadioButtonStyle`, `SwitchStyle`, `ProgressCircleStyle`, `ProgressBarStyle`, `SliderStyle` | `text.h`, `theme.h` |
| Date and time | `DatePickerStyle`, `TimePickerStyle` | `theme.h` |
| Scrolling | `ScrollBarStyle` | `modifier.h` |
| Navigation | `NavigationStyle`, `TopAppBarStyle`, `NavigationBarStyle`, `NavigationPaneStyle`, `DrawerStyle` | `navigation.h` |
| Presentation | `ToastStyle`, `SnackBarStyle`, `TooltipStyle`, `DialogStyle`, `BottomSheetStyle`, `MenuStyle` | `presentation.h` |
| System bars | `SystemBarsAppearance` | `window.h` |

These values own component-specific geometry, color, typography, indication, disabled state, and motion as appropriate. Prefer them to copying a built-in component or hardcoding a parallel visual implementation.

`TextFieldVariantStyle` is nested configuration inside `TextFieldStyle`; it is not a separate Theme override key.

Flat pickers use compact outlined geometry and rounded-square date selections; Material pickers use larger tonal surfaces and circular date selections. TimePicker distinguishes active-field, AM/PM-period, and dial selection colors. Preserve these separate roles when overriding its style instead of applying one selection background everywhere.

### Surface style value types

Use `VisualFill` for a component or popup background that may be a color, gradient, or image fill. Use `Border` for an ordinary rectangular surface border and `std::optional<Border>` when the absence of a border is meaningful. Use `CornerRadii` for the geometry of a component surface; it is a plain value used directly by styles rather than a View modifier.

Keep `Color` for foreground text and icons, color schemes, indicators, dividers, scrims, tracks, and colors that participate directly in mixing, opacity calculations, or custom animation. Do not introduce a combined surface-appearance wrapper or move an ordinary component background or border into `Indication`.

For View decoration, use the separate `CornerRadius` modifier described in [modifiers.md](modifiers.md#corner-radii).

## Override component styles

`ThemeDefinition::Set(value)` replaces the complete value for that type; it is not a field-level patch. Initialize every field whose behavior matters. A style's `Default()` is the generic fallback baseline, not a copy of the current Flat or Material style, so using it and changing one field does not preserve the rest of that built-in style.

For an application-wide override, start from the selected built-in definition so all other Flat or Material values remain installed:

```cpp
ThemeSpec tokens = MaterialLightThemeSpec();
ThemeDefinition definition = MaterialThemeDefinition(tokens);
definition.Set(DividerStyle{
    .color = tokens.colors.outline,
    .thickness = 2.0F,
});

return Theme {
  definition,
  AppContent(),
};
```

Use `FlatThemeDefinition(tokens)` for the equivalent Flat boundary. Do not use `ThemeDefinition{tokens}` as a shortcut for either built-in system.

For a local subtree override, use an empty definition inside an existing theme. It contributes only the typed values you set and inherits the outer `ThemeSpec` and all unspecified styles:

```cpp
ThemeDefinition overrides;
overrides.Set(DividerStyle{
    .color = Color::Rgb(180, 180, 180),
    .thickness = 1.0F,
});

return Theme {
  overrides,
  SettingsContent(),
};
```

Do not put a bare `ThemeDefinition{}` at the application root merely to choose colors. Use a built-in theme and tokens instead. A bare definition is appropriate for local typed overrides or intentionally app-defined style values that inherit an outer theme.

## Indication

`Indication` defines transient focus, hover, press, and ripple behavior; it does not own a component's normal surface background or border. Each `IndicationLayer` can carry a `VisualFill`, border, corner radii, placement, and enter/exit animation. Ripple is color-based and can paint behind or above content. `IndicationGeometry` controls optional layer size and clip radii.

Use component defaults unless a custom interaction treatment is part of the app design. Keep focus visible, do not rely on hover alone, and preserve disabled behavior.

## Animation

Animation specs are `SnapSpec`, `TweenSpec`, `SpringSpec`, or `KeyframeSpec`. `AnimationPlayback` controls delay, iteration count, and restart/reverse behavior.

Use `AnimateTo` with `Opacity`, `Offset`, `Scale`, `Rotation`, or `Transition` for declarative presentation animation. These modifiers do not replace layout measurement. Use `MotionController` inside retained behavior when a custom progression must advance from `FrameInfo`.

Honor `FrameInfo::reduced_motion` or the resolved environment motion policy. Do not drive animation through state writes on every frame.

`UseSceneTransition()` accepts TransitionSpec for built-in or custom effects around an application mutation. Pass a [bound ViewHandle](modifiers.md#mounted-view-targets) to `RunFromView` to capture its presentation center before the mutation. Use `RunAt` for an explicit window-logical point.
Inside a synchronous Click, component event, Menu action, keyboard activation, or accessibility activation, `RunFromCurrentInteraction` uses the exact pointer position or the activated View center without changing semantic event signatures:

```cpp
Button("Next").OnClick([transition, page] {
  transition.RunFromCurrentInteraction(
      TransitionSpec{CircularRevealTransition{}, TweenSpec{0.36, Easing::EaseInOut}},
      [page] { page += 1; }
  );
});
```

The implicit origin ends when the interaction callback returns and the method throws `std::logic_error` outside that scope.
Do not store a global last-pointer position, manually record raw Pointer Down, or add Point to OnClick/OnChanged solely to start a transition; asynchronous work retains geometry and calls `RunAt` explicitly.
Reduced motion is handled by the resolved handle; still keep the mutation correct without the visual effect.

Custom effects are copied, equality-comparable values with a pure `TransitionFrame Evaluate(const TransitionContext&) const` method.
TransitionSample contains transform, opacity, and optional ClipShape; use ClipShape factories instead of constructing render commands.
Attach PageTransition{push, pop, replace} to a NavigationStack page root with With().
It overrides the complete NavigationStyle::motion policy, and an empty PageTransition selects immediate motion.
Push selects the incoming page, Pop selects the departing page, and Replace selects the incoming page.
Use TransitionSpec::Reversed() to exchange both samples and drawing order; active descriptions remain frozen during an operation.
Use `SharedElement(key)` for corresponding content and `SharedBounds(key)` for different content that crossfades inside moving bounds. Both live in `animation.h` and attach with `.With(...)`. Navigation pairs its participating pages automatically; local mutations use `UseSharedTransition()`, a stable `container.With(handle.Scope())`, and `handle.Run(AnimationSpec, synchronous_mutation)`. Mark descendants, keep keys unique per participating side, and do not match both an ancestor and descendant. `BoundsTransform(value)` accepts a copied, comparable, pure `Rect Evaluate(Rect, Rect, float) const` that preserves endpoints.

Local scope input is blocked during playback; keep retarget controls outside it. Nested local scopes follow active outer ownership; nested NavigationStacks are separate boundaries. Missing, partially ancestor-clipped, native, external-texture, or unsupported transform content skips pairing. Consult the selected SDK's themes and presentation guide for examples and limitations.

## Presentation services

- `UseToast()` shows timed messages.
- `UseDialog()` shows standard or custom modal content and returns a layer ID.
- `UseBottomSheet()` shows bottom-attached modal content.
- `UsePopup()` anchors arbitrary content to a node or point.
- `UseMenu()` anchors menu entries with enabled, checked, nested, icon, and icon-tint state.
- `Tooltip` is a modifier for hover/long-press help.

Set `DialogOptions::initial_focus` to a [ViewHandle](modifiers.md#mounted-view-targets) bound inside the Dialog. `Show()` captures it for one attempt before ordinary first-eligible focus; unavailable targets fall back without later retries. Content updates do not reapply it, and closing a nested Dialog restores the current prior focus through the existing modal behavior.

Layer content is outside the ordinary application subtree but retains its captured live Environment. Open layers follow theme changes without replacing their identifiers, content state, focus, or timeout progress; after a provider unmounts, retained layers keep its last values. Keep modal dismissal and owner state explicit, and do not retain a presentation context after dismissal. Presentation handles belong to their window and cannot present from an application task with no live window context.
Set `PopupOptions::retain_anchor_focus` only when pointer interaction with non-focusable popup content must keep an editor or other keyboard session on its mounted anchor; focusable popup descendants still receive focus.

For Popup and Menu, `Show(target, ...)` follows the bound View's full bounds through layout, scrolling, and presentation transforms. Popup also supports `ShowAtAnchor(target, local_bounds, ...)` for a node-local rectangle. Use `UpdateAnchor(id, local_bounds)` to move that rectangle in place. It returns false for a stale id or another anchor mode. `ShowAt(window_point, ...)` uses a fixed window-DIP position.

Prefer theme style overrides (`ToastStyle`, `DialogStyle`, `BottomSheetStyle`, `MenuStyle`, `TooltipStyle`) over copying built-in presentation implementations.
