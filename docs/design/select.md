# Select

`Select` is the non-editable finite-choice component.
It is separate from a future editable ComboBox, which must own TextField-compatible editing, selection, composition, and submission behavior.

## Public contract

The public constructor takes a data range, a required controlled `std::size_t` selected index, and a View factory.
There is no `SelectItem` model, optional selection, placeholder, value binding, or type-erased application data source.
The rvalue-qualified `Label` fluent method supplies the control's accessible name independently from the selected choice value.
Construction copies the input range into the same internal item-source representation used by virtual layouts, so a temporary range is safe and the factory receives stable stored values during one declaration.

The range must contain at least one item and the selected index must be in range.
The component emits `SelectEvents::Changed` only when an enabled choice differs from the current controlled index.
The application supplies the authoritative index on the next composition.

The factory result is the choice root.
Its existing View key is the semantic item identity used by ordinary reconciliation; an absent key deliberately keeps positional identity.
The root must expose a non-empty semantic label and may use the shared `Enabled` modifier.
The root represents one choice and cannot provide independent pointer activation or contain focusable descendants.
Composition and mounted validation reject empty or unlabeled factory results and this conflicting interaction surface.

## Composition and retained ownership

The Select declaration is a Scope containing a themed trigger.
The scope owns a stable Popup anchor and a small mounted session shared only by the trigger and its active popup.
The session retains the active layer id, trigger width, selected index, and popup interaction state; none of these values enter `ViewSpec`.

Opening attaches one anchored Popup.
Compatible Select recomposition calls `PopupHandle::Update`, retaining the Layer id, focus trap, dismissal policy, placement, and popup composition scope while replacing the data snapshot, factory, selected index, style, events, and captured Environment.
Unmounting the trigger dismisses its popup.

The selected content is composed once in the trigger and each choice is composed once in the popup.
Factories are declarative and must not rely on invocation count or side effects.

## Identity and active choice

The active choice is the popup's only focusable item, so opening gives real Runtime and semantic focus to the selected choice when enabled or the first enabled choice otherwise.
Other choices are not Tab stops, and a popup with no enabled choices has no active focus target.
Its retained state tracks the active mounted item identity in addition to the current index.
When keyed data reorders, reconciliation preserves the mounted identity and the active index follows that identity.
When no key is present, reconciliation preserves positions and the active choice therefore follows its index.

If the active item disappears or becomes disabled, the selected item is used when enabled, otherwise the first enabled item becomes active; the active identity is cleared when every item is disabled.
Up and Down find the next enabled choice without wrapping, Home and End find an enabled edge, and Enter or Space requests the active index.
The scroll controller reveals the active item when keyboard movement leaves the viewport.

## Dismissal and controlled events

Pointer activation, semantic activation, and keyboard commit share one request path.
That path clears and dismisses the popup before emitting a changed index, preventing recomposition from updating a layer that is already closing.
Selecting the controlled index closes without an event.
Escape, Back, outside press, disabled input, cancellation, anchor removal, and unmount never emit a change.
Disabling the Select while open dismisses the popup before its separate Layer can accept more input.
Layer focus trapping keeps focus on the active choice and Runtime restores the displaced trigger focus after dismissal.

## Semantics

The trigger publishes `SemanticRole::ComboBox`, its independent accessible label, the selected root label as a read-only current value, expanded state, validation state, and Activate plus Expand or Collapse actions while it owns the active semantic frame.
Its selected content and indicator are excluded as duplicate descendants.
The popup publishes one single-selection `List` collection and each choice root publishes one `ListItem` with label, selected state, enabled state, index, focus for the active choice, and Activate when enabled.
While the modal popup owns semantic focus, the active frame exposes the choice list instead of the underlying trigger; dismissal restores the trigger and its focus.

The platform role mapping uses UI Automation ComboBox, Android Spinner, AppKit PopUpButton, and a button-compatible UIKit trait.
The active choice remains mounted interaction state rather than a second controlled selection value.

## Theme and validation

`SelectStyle` owns trigger and popup foreground, surface metrics, active and selected item backgrounds, indication, and validation presentation.
Flat themes resolve the shared default from their ThemeSpec; Material themes provide an explicit override.
Light and dark variants therefore derive complete values from their corresponding ColorScheme, TypographyScheme, ShapeScheme, spacing, elevation, motion, and interaction schemes.
Insets, spacing, surface metrics, and shadow geometry are validated before the component is mounted.

`Validation` is application-owned presentation state.
It can mark the trigger invalid and show supporting text, but it neither filters choices nor changes the controlled index.
