# Themes and Presentation

## Theme providers

HuxerUI ships Flat and Material light and dark theme definitions.
Theme providers are transparent Environment boundaries: they supply colors, typography, component styles, motion, and presentation policy without adding layout geometry.

```cpp
return MaterialTheme {
  Content(),
};
```

```cpp
return FlatDarkTheme {
  Content(),
};
```

Pass a customized `ThemeSpec` when an application needs different tokens while preserving the selected theme system.
Nested themes affect only their subtree.

## Component styles

Each built-in component resolves its typed style from the closest Theme.
Styles cover geometry, color, typography, indication, and motion owned by that component.

Flat and Material are independent systems, not a shared style with a few color substitutions.
For example, a Flat `TextFieldStyle` can use an outlined surface without a visible floating label while keeping the label in semantics.

## Interaction indication

Interactive components derive visual state from shared interaction facts such as hovered, focused, pressed, dragged, selected, and disabled.
The active indication can paint background, border, content tint, focus ring, or ripple without changing layout.

Application code can provide a custom `Indication` or remove transient indication where that is semantically appropriate.
Normal component appearance remains in its component style rather than being duplicated as an interaction state.

## Animation

Retained animation updates mounted presentation state without recomposing application state every frame.
Motion controllers, theme-owned transitions, and scene transitions use the same frame scheduling and reduced-motion policy.

Scene transitions can customize entering and exiting presentation while keeping old and new scenes synchronized.
When reduced motion is enabled, the runtime selects the documented reduced or immediate path rather than leaving each component to interpret the system setting independently.

## Tooltips

Attach `Tooltip` to an anchor that needs short contextual help.
Hover, focus, dismissal, placement, and delay are owned by the presentation service and active theme.

## Toast

Use the typed toast service for transient non-modal feedback.
The returned handle can dismiss or replace the presentation without exposing layer internals.

## Dialog and BottomSheet

Dialogs and bottom sheets are modal presentations owned by root services.
Their content is an ordinary View factory that captures the caller's Environment.

The service owns modality, focus trapping, barrier interaction, animation, and focus restoration.
Application code owns controlled values and action outcomes.

## Popup and Menu

Popups are anchored non-modal layers with typed placement and dismissal policy.
Menus build on the same anchor and layer infrastructure while adding items, sections, keyboard navigation, semantic roles, and submenus.

Menu icons accept `ImageVariant` and use the resolved menu content tint unless their visual source deliberately provides its own color behavior.

## Layer ordering

Application content, non-modal presentation, modal presentation, selection UI, and debug UI have a defined root-owned order.
Layers preserve the Environment captured at presentation time and do not create another application Runtime.

Use public services rather than directly mutating the layer stack.
See [Architecture Design](../design/architecture.md) for ownership and [Animation and Scene Transition Design](../design/animation.md) for motion behavior.
