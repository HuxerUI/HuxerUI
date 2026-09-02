# ComboBox

`ComboBox` is the editable suggestion component.
It composes the existing TextField and Popup boundaries rather than introducing another text editor, input client, presentation service, or selection model.

## Public contract

The component receives one controlled `TextEditingValue` and one finite suggestion range.
The simple constructor accepts string-compatible suggestions and uses the same text for accepted values and default Text content.
The projected constructor accepts a text projection and a View factory, so application data remains strongly typed without a public `ComboBoxItem` wrapper.
Construction copies the range into one declaration snapshot; filtering, ordering, asynchronous loading, and caching remain application state.
Each factory result retains its ordinary View key, so keyed suggestion identity follows application data while unkeyed identity follows position.

Direct editing emits `ComboBoxEvents::Changed` with the complete proposed editing value.
Accepting an enabled suggestion emits `ComboBoxEvents::Selected` with its current zero-based index and a complete `TextEditingValue::FromText()` replacement proposal.
Selection does not also emit Changed, including when the suggestion text equals the current text.
Submission without an active suggestion emits `ComboBoxEvents::Submitted`.
`ComboBoxEvents::ExpandedChanged` reports a Boolean only when the popup gains or releases its operational layer.
An accepted suggestion or submission reports collapse before its corresponding Selected or Submitted event.

The application applies any proposal to its controlled value.
There is no controlled selected index because arbitrary text may not correspond to a suggestion and an active suggestion is transient interaction state rather than committed application data.

## Composition and ownership

The ComboBox declaration is one Scope whose visible child is an ordinary TextField.
TextField remains the only owner of text reduction, working-value acknowledgement, selection, composition, caret, history, platform input configuration, validation presentation, and native input session synchronization.
ComboBox forwards its field configuration and translates the TextField Changed and Submitted events into its own typed event keys.

One retained field extension owns a small session shared with the active popup.
The session's optional Popup layer id is the only expanded-state authority.
It also retains anchor width, focus and explicit-dismiss state, the current suggestion declaration, and popup interaction state.
Unmount, focus loss, and disabled input dismiss the layer without emitting selection.
Compatible recomposition updates the existing popup through `PopupHandle::Update`; it does not create a second layer or registry.
All open and close paths change the optional layer through one transition operation, so overlapping dismissal paths and equal state cannot emit duplicate ExpandedChanged events.

The default dropdown indicator and a `TrailingIcon(...)` override use the TextField trailing-icon paint path and are decorative.
It does not become another focus target or parallel open callback.

## Popup interaction

The popup does not trap or move Runtime focus.
Real keyboard focus and the native input session stay on TextField while one optional active suggestion is retained in the popup state.
Up and Down choose the next enabled suggestion without wrapping and reveal it through the existing ScrollController.
Enter accepts an active suggestion; otherwise TextField performs ordinary submission.
Escape, outside press, focus loss, disabled input, and unmount dismiss without selection.
The collapse event follows operational ownership rather than popup exit animation: the ComboBox requests dismissal, releases the layer id, and then emits it.

Composition takes priority over suggestion navigation.
When the current working value has an IME composition, ComboBox leaves Arrow, Enter, and Escape to the TextField and platform input path.
Shift-, Control-, and Meta-modified keys likewise remain editor input instead of changing the active suggestion.
Alt modifies only Up and Down for popup opening and closing; other Alt-modified keys remain editor or platform input.
It never commits, finishes, or cancels composition itself.

Explicit dismissal records the current query revision so a delayed result update cannot immediately reopen the popup.
A later direct edit or a new focus cycle clears that suppression.
Empty suggestions leave the popup closed unless the application supplies `EmptyContent`, which is rendered in the same popup surface rather than a second loading presentation path.

Each suggestion root is one interaction target.
It may use the shared Enabled modifier, but suggestion and empty-state content cannot own independent pointer activation or focusable descendants.
Pointer release, keyboard acceptance, and semantic activation share one commit path and validate the current popup generation so a pointer sequence cannot accept a replaced declaration.

## Semantics

The TextField semantic owner is enriched to `SemanticRole::ComboBox` with editable state, current expanded state, and Expand or Collapse.
It retains TextField label, value, placeholder, validation, UTF-16 selection, SetText, and SetSelection output.
The popup publishes a List collection and each suggestion publishes one labeled ListItem with enabled state, active selected state, collection index, and Activate.
Suggestion descendants are excluded from duplicate accessibility output.

The platform mapping distinguishes this editable ComboBox from Select through `read_only` and available SetText actions.
Windows retains UI Automation ComboBox with Value and ExpandCollapse patterns, Android uses an editable auto-complete class rather than Spinner, AppKit uses its ComboBox role rather than PopUpButton, and UIKit attaches its shared text-input responder instead of advertising a button-only trait.

## Theme and validation

TextFieldStyle owns all visible field states and metrics.
ComboBoxStyle owns only popup background and foreground, active item background, item and surface padding, indication, shadow, corner radius, minimum item height, and maximum popup height.
Flat themes derive this style from ThemeSpec and Material themes install an explicit style override.
Geometry and shadow inputs are validated before the composed field mounts.

Validation is still application-owned TextField presentation.
It does not filter suggestions, reject direct edits, or select an item.
