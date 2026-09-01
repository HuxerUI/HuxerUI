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
Use `RunFromCurrentInteraction` inside a synchronous component event when a circular reveal should originate at the pointer position or keyboard/accessibility activation center:

```cpp
Button("Next").OnClick([transition, page] {
  transition.RunFromCurrentInteraction(CircularRevealSceneTransition{}, [page] { page += 1; });
});
```

The implicit origin exists only for the duration of the interaction callback.
Use `RunAt` with retained geometry for asynchronous work, and use `Anchor` plus `Run` when the reveal belongs to stable View geometry rather than the triggering interaction.
When reduced motion is enabled, the runtime selects the documented reduced or immediate path rather than leaving each component to interpret the system setting independently.

## Tooltips

Attach `Tooltip` to an anchor that needs short contextual help.
Hover, focus, dismissal, placement, and delay are owned by the presentation service and active theme.
The hover delay begins again after each pointer movement over the anchor, and a visible hover-owned Tooltip hides immediately when the pointer moves.
Keyboard focus keeps its independently owned Tooltip visible.

## Toast and SnackBar

Use the typed Toast service for passive transient feedback.
Each call creates an independent non-interactive notification that can be dismissed through its returned id.

Use SnackBar when the feedback needs one optional action:

```cpp
auto snack_bar = UseSnackBar();

return Button("Delete")
    .OnClick([snack_bar] {
      DeleteItem();
      snack_bar.Show("Item deleted", "Undo", [snack_bar] {
        RestoreItem();
        snack_bar.Show("Item restored");
      });
    });
```

Only one SnackBar is active per window.
A new request atomically replaces the previous one, and the action dismisses its owning SnackBar before invoking the callback.
The default duration is four seconds; pass `SnackBarOptions{std::nullopt}` for an indefinite presentation that is dismissed explicitly or by its action.
Timed dismissal pauses while the surface or action is hovered, while the action is focused or pressed, and while the application is inactive.

## Dialog and BottomSheet

Dialogs and bottom sheets are modal presentations owned by root services.
Their content is an ordinary View factory that captures the caller's Environment.

The service owns modality, focus trapping, barrier interaction, animation, and focus restoration.
Application code owns controlled values and action outcomes.

## Popup and Menu

Popups are anchored non-modal layers with typed placement and dismissal policy.
Menus build on the same anchor and layer infrastructure while adding items, sections, keyboard navigation, semantic roles, and submenus.
`PopupHandle::Update` replaces an existing popup's content factory and captured Environment while retaining its layer id, anchor, placement, and dismissal policy.

Menu icons accept `ImageVariant` and use the resolved menu content tint unless their visual source deliberately provides its own color behavior.

## Layer ordering

Application content, non-modal presentation, modal presentation, selection UI, and debug UI have a defined root-owned order.
Layers preserve the Environment captured at presentation time and do not create another application Runtime.

Use public services rather than directly mutating the layer stack.
See [Architecture Design](../design/architecture.md) for ownership and [Animation and Scene Transition Design](../design/animation.md) for motion behavior.
