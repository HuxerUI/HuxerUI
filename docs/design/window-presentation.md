# Window Presentation

Each UiWindow owns one layer stack alongside its application content. Built-in presentation services and application-owned window services use the same controller. The [application guide](../guide/themes-and-presentation.md) describes presentation APIs and style configuration.

## Layer ownership and ordering

The internal RuntimeRoot hosts the window system-bar backplane, application content boundary, and layer stack. The backplane and layers use the full viewport. The application content receives the safe-area policy defined in [Window Insets and System Bars](window-insets.md).

LayerController owns entries, IDs, and attachment order. UiWindow owns their mounted scopes, layout, interaction, and committed rendering. Disconnecting the controller clears its factories and retires retained handles. Each entry has an independent RecomposeScope. An entry attached during application composition can participate in that frame. Mutations after the layer snapshot schedule another frame. Mounted geometry and semantics use the same captured layer identity, exit state, and modal-group identity.

`LayerLevel::Presentation` hosts dialogs, sheets, popups, and menus. `Notification` hosts toasts, snack bars, and tooltips. `System` hosts window diagnostic content. Entries in one level follow attachment order. UiWindow's private text-selection overlay paints after the public layer stack.

## Captured Environment and appearance

A typed presentation handle captures the caller's effective Environment. Each entry observes its own Theme, locale, resources, and custom values. Layer composition uses the existing [Environment dependency model](view-composition.md#environment-boundaries), including the retained provider chain after the source view disappears.

The service owns request identity and behavioral options. Theme styles supply surfaces, spacing, placement, and motion as data. The layer's own composition resolves those values and passes visual layout values to the stack. A change of style can update a visible entry without replacing it or reviving a dismissed entry. Active animation retains its timing until the next transition.

An entry without exit motion is removed immediately. With configured motion, dismissal makes its content non-interactive and retains its visual state until the exit completes. A modal barrier and focus trap stay active until removal. Presentation styles use the animation engine described in [Animation](animation.md#timing-and-playback).

## Pointer, Back, and focus

`PassThrough` layers skip pointer targeting. `Content` layers admit input over their content and allow uncovered areas to reach lower targets. A `Barrier` intercepts pointer input outside its content and may request dismissal. Outside-press dismissal and a colored barrier require `Barrier`.

UiWindow offers Back to the text-selection overlay and then walks the public layers from top to bottom. A layer's cancel policy passes the request through, consumes it, or dismisses the entry. A dismissal callback takes precedence over removal by the controller. Visible interactive presentations configured to reject cancel consume Back. Passive feedback passes it through to [application and navigation handling](navigation.md#back-event-model).

The topmost enabled focus trap in paint order contains focus. It can belong to a modal layer or an in-tree modal surface such as a drawer. Closing a nested trap restores the prior mounted focus when the node remains valid and enabled. An initial Dialog focus target is tried once when the trap first mounts. A late content update cannot repeat that attempt.

## Anchored presentation

Popup and Menu accept a bound ViewHandle, its local rectangle, or a fixed window point. Anchor geometry comes from final layout and presentation transforms. Nested anchors resolve before the frame commits. Replacement of the node bound to the same handle in one reconciliation preserves the entry. An anchor with no mounted target after reconciliation dismisses it.

Popup can change the anchor rectangle without replacing the entry. Updating content carries the caller's captured Environment. Anchor focus is retained only when requested, the current anchor already owns focus, and the popup provides no focusable target.

Menu sections express logical group boundaries, with their rendering supplied by Theme. The root menu owns the outside-press barrier. Content-only submenus leave the parent menu interactive. Back closes the deepest open submenu first, and outside press dismisses the chain by default. Opening another submenu replaces only that level and its descendants. Menu motion uses the requested anchor side for its origin, including when placement falls back to another side.

## Modal surfaces

Command and declarative Dialogs share the modal layer, focus, scrim, and exit path. Declarative visibility is controlled. Outside press and Back request a source-state change through `on_dismiss_request`. When that state becomes visible during exit, the layer reverses from its current motion value.

Standard Dialogs use Theme to arrange the title, message, and positive and optional negative actions. Actions request dismissal before invoking callbacks. Empty labels resolve through the built-in `dialog_ok` and `dialog_cancel` resources. Custom factories keep ownership of their content layout, while the presentation style still supplies the outer placement, scrim, and motion.

BottomSheet uses the same modal policies with its own edge-attached surface. A style-provided handle captures downward drag and shares the offset with the surface transition. Cancellation or a short release settles to the edge. A release over the dismissal threshold requests dismissal and settles if the layer stays visible.

## Transient feedback

Toast entries are message-only and time out through their window service. SnackBar keeps one active request per window. Replacement atomically swaps the layer and binds timers and action callbacks to the new ID. An action dismisses its own request before running application code, so the callback can show another SnackBar.

SnackBar dismissal uses frame timing and pauses during the relevant hover, focus, and press states or while the application is inactive. It passes Back through. Tooltip uses a retained target modifier and the same layer controller.

The built-in diagnostics overlay attaches after window hooks as a System entry. Sampling is active only while the metrics panel is expanded. Layout guides reuse the application tree's resolved presentation geometry and effective clips. The overlay has an independent scope so diagnostic updates need not recompose the application root. See [Runtime profiling](../development/building.md#runtime-profiling) for capture controls.
