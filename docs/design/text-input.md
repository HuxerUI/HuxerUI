# Text Input and TextField Design

Editable components own their text, selection, composition, and editing semantics. UiWindow owns focus and the active logical input session. Platform adapters translate IME callbacks into commands for the current client. The public extension contract is declared in [text_input.h](../../include/huxerui/text_input.h).

## Architecture

The text input path has two related but separate models:

```text
TextEditingValue
    declarative value of the built-in TextField

TextInputCommandBatch
    ordered editing intent delivered by a platform input system
```

`TextEditingValue` is not the platform mutation protocol. A normal TextField reduces input commands into a new `TextEditingValue`. A complex component can apply the same commands to its own document core without creating a full value snapshot.

The complete path is:

```text
platform IME
    |
platform input adapter
    |
TextInputCommandBatch
    |
UiWindow text input session
    |
focused TextInputClient
    |                       |
TextField reducer           Custom editor client
    |                       |
TextEditingValue            EditorCore
```

The ownership boundaries are:

| Layer | Responsibility |
| --- | --- |
| Platform input adapter | Normalize platform callbacks and operate the platform input connection |
| UiWindow | Focus ownership, session identity, routing, and stale callback rejection |
| TextInputClient | Text state, edit semantics, context queries, and text geometry |
| TextField | Controlled value, simple editing reducer, selection, caret, and painting |
| Custom editor | Document state, transactions, history, and editor-specific behavior |

`UiWindow` does not own text content. `PlatformTextInput` does not decide how an edit changes a value. An editable component does not call platform IME APIs directly.

## Text model

`TextEditingValue` is the controlled value of the built-in TextField. It contains the UTF-8 string, a directed selection with affinity, and optional composition.

The model follows these rules:

- Text is stored as UTF-8.
- Every `TextOffset` is measured in UTF-16 code units.
- `TextRange` is ordered and does not preserve direction.
- `TextSelection` preserves direction through `anchor` and `active`.
- Selection and composition ranges are measured in the same current text.
- A missing composition and a collapsed composition are distinct states.
- Session identity and platform synchronization revisions are not part of `TextEditingValue`.
- Invalid offsets, reversed ranges, and offsets inside a UTF-16 surrogate pair are rejected at protocol boundaries.

UTF-16 offsets are an explicit interoperability choice. Windows, Apple, and Android input APIs already operate in this coordinate system. Keeping one offset convention prevents every adapter from inventing a different conversion policy.

Application code should not normally manipulate UTF-16 offsets directly. TextField and reusable text utilities provide validated range and movement operations.

## Input commands

A `TextInputCommandBatch` combines the current session ID with an ordered list of text input commands. Each command carries the target, text, selection, or deletion lengths needed by its operation.

`coordinate_space` applies to `target`. `selection_after` is always expressed in absolute UTF-16 offsets in the resulting text. A platform adapter converts platform-specific relative cursor placement before constructing the command.

The protocol has these semantics:

- One platform input callback produces one ordered batch.
- A batch is validated before any visible mutation occurs.
- Commands execute in order against a staged state.
- A later command sees the state produced by earlier commands in the batch.
- A rejected command rejects the complete batch. Validation covers command fields, non-negative deletion lengths, the active coordinate space, valid UTF-16 boundaries, resulting selection and composition, and editability.
- The client publishes at most one resulting state change for a successful batch.
- UiWindow rejects a batch whose session does not match the active client.
- The platform adapter never reconstructs a command by diffing two full text snapshots.

`SetSelection` changes selection without changing text.

`BeginComposition` records the replacement baseline and establishes a composition range.

`UpdateComposition` replaces the active composition or its explicit target, updates the composition range, and applies the requested selection.

`CommitText` replaces the active composition or selection and clears the composition.

`FinishComposition` keeps the provisional text and clears the composition marker.

`CancelComposition` restores the client-owned composition baseline.

`DeleteSurrounding` removes text around the current selection using the explicit unit supplied by the platform.

The client owns undo grouping, editor transactions, document history, and any linked editing.

## Text input client

`TextInputClient` begins and ends a session, applies command batches, handles editing keys, and provides bounded context, state, and geometry queries.

The result and state types are public because `TextInputClient` is a public extension point. Their required semantics are:

- A current selection and optional composition.
- A bounded text context query.
- Accepted, rejected, read-only, and session-mismatch outcomes.
- A request to update or restart the platform input connection.
- Caret, range, and point hit-test geometry.

`TextInputApplyResult` reports only the result code and any required platform input synchronization action.
UiWindow derives whether state changed from `TextInputState` revisions instead of accepting a second, potentially inconsistent changed flag.

A `NodeExtension` exposes the capability through `GetTextInputClient()`.

A focusable node can expose at most one client. Multiple text input clients on the same node are rejected instead of receiving an implicit modifier priority.

The extension returns a stable shared client. UiWindow retains that client only for the active input session, allowing it to call `EndTextInput()` after the owning extension is reconciled away without retaining a raw extension pointer.

## Text selection client

Editable and read-only selectable content share `TextSelectionClient`. The capability exposes the selection geometry and editing actions needed by the selection overlay.

Selection points and geometry use the owning node's local logical coordinates.
`TextSelectionGeometry` contains independent optional `start`, `end`, and `toolbar_anchor` rectangles; an offscreen endpoint does not hide a visible handle or clear the logical selection.
`QuerySelectionGeometry()` returns `std::nullopt` when no logical selection or editor caret exists, ending the old menu intent; an existing selection with no visible geometry returns `TextSelectionGeometry{}` and can restore its menu when visible again.
UiWindow maps them through the node's resolved presentation transform exactly once at the host boundary.
The editing-action methods are optional. Read-only clients can expose Copy and Select All through the shared selection menu without opening an IME session.

`ClearSelection()` cancels a range without needing visible geometry: a read-only client removes its logical selection, while an editor collapses to its active endpoint through its normal controlled update path. UiWindow invokes it after a confirmed touch tap elsewhere. A tap inside the active editor retains its ordinary caret-placement path instead of emitting a second selection update.

A `NodeExtension` exposes at most one selection client through `GetTextSelectionClient()`.
UiWindow borrows this pointer only during dispatch and never retains it beyond the owning extension's lifetime.
TextField exposes both input and selection clients; `SelectionArea` exposes only a selection client and never starts an IME session.

`SelectionArea` prepares descendant text-layout and relative-transform value snapshots after presentation resolution.
It does not retain descendant node pointers, and unchanged descendant geometry keeps its foreground PaintSequence clean.

TextInteraction creates text-selection recognizers using the existing GestureRecognizer decision and deadline protocol. PointerInteraction retains their physical sequence and applies selection only through successful ownership acceptance, after revalidating the original target and any focus, IME, or competing-recognizer callbacks. Double-tap history belongs to TextInteraction, while the pointer owner and candidate binding belong to PointerInteraction. TextInteraction never reads PointerSession state. Touch focus and text recognition keep their explicit order before overlay geometry is published.

## Input configuration

`TextInputConfiguration` supplies the common input type, capitalization, submission action, multiline, secure and read-only modes, autocorrection, and software keyboard policy.

Submission actions have common UiWindow semantics:

| Action | Behavior |
| --- | --- |
| `Default` | Resolve to `Done` for single-line input or `Newline` for multiline input |
| `Done`, `Go`, `Search`, `Send` | Trigger `OnSubmitted` |
| `Next` | Trigger `OnSubmitted`, then move to the next focusable node without wrapping |
| `Newline` | Insert a newline in a multiline TextField |

Terminal soft-keyboard dismissal remains a platform responsibility. It does not clear HuxerUI focus merely to hide the keyboard. Platform IME actions and hardware Enter both pass through the same focused TextInputClient key path.

Secure entry uses the same state and command protocol. The retained `TextEditingValue` contains the real text, while TextField builds a separate single-line mask layout and draws one bullet per grapheme. Copy and Cut are disabled. Platform adapters prevent platform surrounding-text and extracted-text queries from returning the value while preserving the internal context needed for command routing and composition. Secure and multiline configurations are mutually exclusive.

The shared `SemanticFrame` follows the same privacy boundary.
An ordinary TextField publishes its committed value, normalized UTF-16 selection, label, placeholder, validation state, focus, and supported SetText and SetSelection actions so platform accessibility can edit and announce it.
These actions enter the retained TextField reducer and the same UiWindow invalidation and active-session synchronization path as platform text input; they do not create a second editor protocol.
A read-only field omits SetText but retains SetSelection, while a secure field may expose SetText but omits SetSelection.
A secure TextField semantic frame never contains TextField-owned plaintext, selected text, surrounding text, composition text, clipboard content, or a plaintext-derived state description; Copy and Cut remain unavailable through the existing editing policy.
Application-authored labels, hints, errors, and identifiers remain trusted metadata and must not copy the protected value.
The semantic frame carries protected state without a value or protected grapheme count.
Accessibility focus remains platform-owned and does not start a text-input session; an explicit semantic input-focus action follows the existing UiWindow focus and TextInputClient path.
See [Semantics and Accessibility Design](semantics.md) for the complete frame and action contract.

## UiWindow session ownership

`UiWindow` owns one active HuxerUI text input session per host view:

```text
focused node identity
focused TextInputClient
monotonic TextInputSessionId
last synchronized client revision
platform input connection state
```

Session IDs are non-zero, monotonically increasing, and not reused during the UiWindow lifetime.

The lifecycle is:

```text
focus editable node
    begin client session
    start platform input

recompose same node and client
    preserve session
    synchronize changed state when required

move focus
    finish old client session
    stop old platform input
    begin new session when the new node is editable

unmount, disable, or make client read-only
    finish current composition
    stop platform input

PlatformView takes focus
    stop HuxerUI text input
    let the PlatformView own the IME
```

An asynchronous platform callback captures the session ID at entry. If focus or ownership changes before the callback reaches UiWindow, it is rejected without accessing the former client.

Normal focus loss finishes composition by retaining provisional text and clearing the composition marker. Escape first cancels an active composition. When there is no active composition, Escape follows the existing UiWindow focus behavior.

Mouse and pen presses focus a node before platform input synchronization. Touch focus remains pending until the gesture resolves as a tap. TextField also defers its touch caret placement until release. If movement wins a scroll gesture, both pending operations are cancelled, so scrolling across an editor does not focus it or display the software keyboard. A recognized long press commits focus before opening text selection.

Platform input starts or updates after the current pointer dispatch completes. The platform therefore sees the resulting selection and geometry rather than the caret position from the previous frame.

Modal focus capture and restoration use the same lifecycle. Restoring focus to an editable node creates a new HuxerUI text-input session rather than reviving an old session ID.

PointerInteraction requests keyboard presentation by node identity without obtaining text-input session IDs. SemanticTree routes text actions to TextInteraction, which invokes the original extension action, validates the client's resulting state, and synchronizes any active session. Semantic editing does not require the target to own focus or an active text-input session.

## Platform input capability

UiWindow exposes an optional `PlatformTextInput` endpoint. `Start` opens native input for the selected session, `Update` refreshes it, `Restart` reconfigures it, and `Stop` retires it. Software keyboard presentation uses separate best-effort show and hide requests.

A host without the capability still dispatches ordinary hardware key events.

Focus, input-session ownership, and software-keyboard visibility remain separate. `Start` prepares native input. With `Automatic`, UiWindow requests the software keyboard after starting a session and after a confirmed tap or click on the focused editor. `Manual` suppresses these automatic requests. Changing only the policy neither restarts native input nor changes keyboard visibility.

`WindowHandle::ShowKeyboard()` and `HideKeyboard()` address the editor focused in that window at the time of the call. The protected `NodeExtension` methods address the extension's own node, including during the event that first focuses it. UiWindow resolves requests after dispatch and input-session synchronization. The last valid explicit request takes precedence over an automatic request. The mounted node, client, and session must still match. An explicit request made before session start can resolve only for that same editor during the current dispatch.

`ViewHandle::RequestFocus()` uses the same dispatch boundary to synchronize the accepted target's text-input session. Outside input dispatch, this synchronization completes before the call returns, including from a resumed Task. Inside an event handler it completes at the outer dispatch boundary; a subsequent `WindowHandle::ShowKeyboard()` in that handler addresses the newly focused editor. Requests captured for a previous editor cannot transfer to the new one. With `SoftwareKeyboardPolicy::Manual`, programmatic focus alone starts the input session without requesting keyboard presentation.

`RequestShowKeyboard` and `RequestHideKeyboard` are best-effort platform operations for the active session. Hiding preserves HuxerUI focus, the session, and its selection. Native IME events, including a final composition update during dismissal, still use the ordinary command path. Platforms without a software keyboard can keep both hooks as no-ops. On Web, presentation depends on whether the browser allows the input proxy to focus and open the keyboard during the current user gesture.

Start, Update, and Restart receive state and geometry from the same UiWindow snapshot.
Platform-derived UiWindow implementations do not call back into shared UiWindow logic merely to recover the current caret during Start or Restart.

The platform-derived UiWindow calls shared UiWindow logic through session-aware entry points:

```text
HandleTextInputCommands
PerformTextInputAction
QueryTextInputContext
QueryTextInputGeometry
QueryTextInputPosition
```

It does not retain a raw TextField or `TextInputClient` pointer across platform callbacks.

## State synchronization

The platform input connection needs selection, composition, and candidate geometry. It does not need an authoritative copy of the complete client text.

`TextInputState` contains:

```text
active session ID
selection
optional composition
client synchronization revision
text-content revision
```

The client synchronization revision increases for every observable selection, composition, text-content, or client-owned geometry change.
Client-owned geometry includes an editor's internal text scrolling but not ancestor layout, scrolling, or presentation transforms.
The text-content revision increases only when text changes, allowing UiWindow to distinguish layout-affecting edits from selection and composition-marker updates.
Both revisions are UiWindow synchronization details.
They are not mutation preconditions and do not appear in `TextEditingValue`.

The platform can request a bounded `TextInputContext`:

```text
slice start in UTF-16 units
total text length in UTF-16 units
UTF-8 slice text
selection
optional composition
```

Large document clients return only the requested surrounding context. TextField can return the complete short value when appropriate. Platform adapters must tolerate partial context and request another range when needed.

Client state changes outside an IME callback, including an authoritative controlled TextField update, increment the appropriate revisions.
UiWindow synchronizes the new state after reconciliation.
A configuration or ownership change requests a platform input restart; an ordinary selection change requests only an update and paint invalidation.
Client-owned geometry changes update candidate geometry without restarting an active IME composition.

## TextField API

The built-in TextField is declared in [view.h](../../include/huxerui/view.h). Applications normally include the umbrella header.

Representative use is:

```cpp
[[huxerui::composable]]
View LoginForm() {
  auto name = UseState(TextEditingValue::FromText(""));

  return TextField(name)
      .Label("Name")
      .Placeholder("Enter your name")
      .OnChanged([name](const TextEditingValue& value) mutable {
        name = value;
      })
      .OnSubmitted([] {
        Submit();
      });
}
```

`OnChanged()`, `OnSubmitted()`, and `OnTrailingIconClick()` bind the matching typed `TextFieldEvents`.

`Label()` and `Placeholder()` have separate presentation roles.
When an empty field is unfocused, its label occupies the input line and its placeholder is hidden.
Focus or non-empty text moves the label to the selected variant's floating position; an empty focused field then reveals its placeholder.
A TextField without a label retains placeholder-only behavior.
The label transition is retained visual state and uses the resolved Theme motion duration without recomposing application state.

`Variant(TextFieldVariant::Standard)` uses a transparent container and bottom state indicator.
`Variant(TextFieldVariant::Filled)` adds a top-rounded container fill to the same indicator and label geometry.
`Variant(TextFieldVariant::Outlined)` uses a full outline interrupted by the floating label.
Material defaults to Filled while Flat defaults to Standard; an explicit variant is stable across themes.

`LeadingIcon()` and `TrailingIcon()` accept image resources, raster assets, and vector assets.
The one-argument forms are decorative content within the editor geometry.
`TrailingIcon(icon, semantic_label)` defines a self-drawn action subtarget when `TextFieldEvents::TrailingIconClick` is bound.
Its handler remains in the View event bindings rather than the TextField modifier, and its virtual Button semantics use the same typed event as pointer activation.
Pointer activation uses the resolved `IconButtonStyle` interaction geometry without moving the editor selection; TextField still owns the icon size and state tint.
Their occupied width participates in text layout, selection, caret geometry, scrolling, and platform text-input geometry.
Vector assets resolve the TextField state color, while raster assets retain their encoded colors.

`Align(TextAlign::...)` and `VerticalAlign(TextVerticalAlign::...)` configure the editable text region rather than the field container.
The retained platform layout applies horizontal alignment to the value, placeholder, selection, caret, hit testing, scrolling, and platform input geometry.
Vertical alignment places the same editing geometry within the available content height without changing intrinsic measurement.
Single-line fields default to `Center`; multiline fields default to `Top`; an explicit value remains stable when line limits change.
Floating labels, icons, and supporting messages keep their component-owned placement.

Text semantics such as placeholder, multiline behavior, keyboard type, and submission action are component configuration. Intrinsic line limits and input length limits follow the same rule:

```cpp
return TextField(message)
    .LineLimits(TextFieldLineLimits::MultiLine(3, 8))
    .MaxLength(200);
```

`TextFieldLineLimits::MultiLine()` uses the resolved text line height and can set an intrinsic minimum and maximum. It controls intrinsic sizing rather than replacing `Frame`; explicit parent constraints remain authoritative. Once content exceeds the maximum, TextField keeps its internal viewport scrollable. `TextFieldLineLimits::SingleLine()` represents the default single-line mode.

`MaxLength()` counts grapheme clusters through the platform text layout boundaries. Commit and paste operations truncate only inserted text at a grapheme boundary. Deletion remains available when a controlled value already exceeds the limit, and such an external value is never silently rewritten. Active IME composition may temporarily exceed the limit and is constrained as one edit when committed or finished.

Application validation is a separate pure operation:

```cpp
const ValidationResult validation = Validate(
    email.text,
    Required(),
    EmailAddress()
);

return TextField(email).Validation(validation);
```

`ValidationResult` distinguishes `None`, `Valid`, `Invalid`, and `Pending` and retains its message as a StringVariant until mounted reconciliation. TextField consumes the resolved result only for presentation: an invalid result uses the Theme validation color, border width, and supporting message layout. The application owns the value and chooses when to validate it. TextField presents the supplied outcome through Theme.

Synchronous rules return a `ValidationResult` and `Validate()` stops at the first result that is not valid. `Required` and `EmailAddress` use `validation_required` and `validation_email` from the built-in `huxerui` resource domain unless the application supplies another StringVariant. Custom callables remain equally supported. `Pending` is supplied by asynchronous application state and may display a neutral supporting message without the invalid border; TextField does not own asynchronous work.

Visual properties remain Theme styles or modifiers rather than growing one-off TextField styling methods.

Password visibility stays controlled application state on the same component:

```cpp
auto visible = UseState(false);

return TextField(password)
    .Secure(!visible)
    .TrailingIcon(visibility_icon, visible ? "Hide password" : "Show password")
    .OnTrailingIconClick([visible] { visible = !visible; })
    .OnChanged([password](const TextEditingValue& value) mutable { password = value; });
```

`Secure(bool)` configures `TextInputConfiguration::secure`, and its default argument enables secure entry.
It does not create a second value model or a separate PasswordField component.

Use `TextEditingValue::FromText()` to construct a controlled value from plain text.

## Static text selection

Static text selection is deliberately separate from editable text input. `Text` remains non-selectable by default, while `SelectionArea` enables selection across descendant `Text` nodes:

```cpp
SelectionArea {
  Column {
    Text("Heading", TextRole::Title),
    Text("Selectable body text."),
  },
};
```

`SelectionArea` owns retained selection state and text layout geometry, but it does not expose a `TextInputClient` and never starts an IME session. UiWindow routes Copy and Select All to the nearest selection owner of the focused node through the same `TextEditingAction` entry points used by editable clients. An active editable client retains priority. Cut and Paste remain unavailable for static content.

Desktop hosts use pointer drag selection and the standard Ctrl or Command shortcuts. Touch input uses runtime-owned long-press word selection and a shared HuxerUI selection overlay with draggable handles. A newline separates adjacent descendant `Text` nodes in copied plain text.

For virtual content, `SelectionArea::Source()` and `Text::SelectionBlock()` bind visible paragraphs to an immutable logical snapshot. Select All and Copy include unmounted blocks without realizing their Views. Snapshot replacement, optional endpoint geometry, links, and edge scrolling follow [Text and Fonts](text.md#logical-selection-source).

## Controlled value behavior

TextField follows the controlled control model while retaining a responsive mounted working value.

The retained extension stores:

```text
latest declarative value
working TextEditingValue
last emitted value
composition cancellation baseline
composition history baseline
bounded undo and redo history
text layout cache
horizontal scroll offset
caret blink state
pointer selection state
client synchronization revision
text-content revision
```

When a command batch succeeds:

- The reducer updates the working value immediately.
- TextField repaints from the working value.
- One `TextFieldEvents::Changed` event is emitted.
- UiWindow synchronizes the resulting selection and composition to the host.

When composition produces provisional text, `Changed` is emitted with the current composition range. The application echoes the complete value, including selection and composition, on the next composition.

During reconciliation:

- An incoming value equal to the last emitted value acknowledges the working value and preserves the session.
- An incoming value different from the last emitted value is authoritative.
- An authoritative replacement clears the old composition baseline, replaces the working value, and requests platform input synchronization.
- An authoritative text replacement clears local undo and redo history.
- An authoritative selection-only replacement preserves history but ends the current merge group.
- An authoritative replacement during active composition restarts the platform input connection.
- TextField never converts an authoritative replacement into inferred insert or delete commands.

If application code does not preserve the emitted value, a later reconciliation can restore the declarative value. This is consistent with other controlled controls and avoids an undocumented internal source of truth.

## TextField reducer

The built-in reducer is independent of UiWindow and platform code. It accepts a validated value, an optional composition baseline, and an ordered command batch.

Composition cancellation requires retained state that is intentionally absent from `TextEditingValue`:

```text
original replacement range
original replaced text
original selection
```

`BeginComposition` captures this baseline once. Composition updates retain it. `CommitText` and `FinishComposition` discard it. `CancelComposition` restores it.

Hardware keyboard movement and deletion use Unicode grapheme clusters. Platform-requested `DeleteSurrounding` follows the explicit unit in the command. These paths must not be implemented with UTF-8 byte movement or average character width assumptions.

The reducer does not own undo history. TextField keeps bounded undo and redo stacks beside its retained working value without changing the platform command protocol. Each entry stores the complete value before and after one edit so selection is restored with text.

Consecutive single-code-point insertion, backward deletion, and forward deletion merge while their selection continuity, edit direction, and time window remain compatible. Navigation, focus changes, atomic edits, undo, and redo end the current merge group. Paste, cut, word deletion, and newline insertion remain individual history entries.

Composition updates do not create intermediate entries. TextField captures one value before composition starts and commits one entry when composition finishes. Undo during active composition cancels it back to that baseline before consuming committed history.

## Text layout and geometry

The existing whole-string text measurement is insufficient for an editable control. Correct selection, caret placement, pointer hit testing, and platform input-method candidate windows require:

- Point-to-text hit testing.
- Text offset-to-caret geometry.
- Range-to-selection rectangles.
- Line metrics.
- Affinity at bidirectional boundaries.
- Glyph cluster and grapheme boundaries.

The private `TextLayoutBackend` supplies platform geometry to both TextField and the public layout snapshot.

Secure TextField creates grapheme boundaries from the platform text layout, then lays out only its mask string. A small mapped layout translates between real UTF-16 offsets and visual bullet offsets for hit testing, caret geometry, selection rectangles, and deletion. The PaintSequence never receives the real text.

The TextField caches layout by text, font, available width, multiline configuration, resolved shaping options, and relevant style values.
On Linux and Web, the internal per-window [text-metrics revision](text.md#platform-caches) also invalidates these layouts when platform text metrics change without a Font value change.
TextField retains its controlled value, selection, composition, focus, and input-session identity while refreshing caret and IME geometry.
An empty shaping locale resolves from the mounted TextField's effective inherited Locale, while `TextField::Shaping()` preserves a non-empty explicit locale.
The resolved shaping value is shared by the controlled value, label, placeholder, validation message, grapheme boundaries, selection and caret geometry, measurement, and every DrawText command so platform layout and renderer replay cannot diverge.

Editor components obtain paragraph geometry with `TextMeasurer::CreateTextLayout()` and draw it with `PaintContext::DrawText()`. They own the document, selection, input commands, and mapping between document offsets and each paragraph. The [custom editor example](../../examples/custom_editor/main.cpp) uses one input session across the document.

Candidate geometry is reported in node-local logical coordinates. UiWindow applies layout and presentation transforms to obtain host-view coordinates. The native input boundary converts those coordinates to the coordinate space required by its API.

`TextInputClient::QueryTextInputPosition` receives a point in the owning node's local logical coordinates.
`UiWindow::QueryTextInputPosition` accepts host-view logical coordinates and applies the inverse node transform before calling the client.

An active session retains the last platform-published host-view geometry and, when needed, one prepared snapshot that has not yet been published.
Both snapshots carry the client synchronization revision, node layout revision, and node-to-host transform that produced them.
UiWindow queries geometry again only when none of those keys match, and notifies the platform only when state or the resulting geometry changed.
Caret reveal may prepare geometry before scrolling; the next platform synchronization promotes the still-matching snapshot only after it is published.
The cache belongs to the active session and is discarded when that session ends.

Average character width must not be used for caret or hit-test behavior. It fails for emoji, ligatures, CJK text, combining marks, and bidirectional text.

## TextField layout and painting

TextField is a leaf node. A single-line field has the intrinsic height provided by its style. A multiline field wraps to the available width and grows to its content height unless `Frame` or its parent supplies a bounded height.

The content pipeline is:

```text
variant background
selection rectangles
text, placeholder, or expanded label
composition underline
caret
decorative icons and supporting text
bottom indicator or outlined border
floating label
```

The current `PaintContext` already provides the necessary primitives:

- `DrawRect` for selection, caret, a thin composition underline, and Standard or Filled state indicators.
- `DrawText` for text, labels, placeholders, and supporting text.
- `DrawBorder` for an ordinary field border and `StrokePath` for an outlined border interrupted by a floating label.
- `DrawImageRect` for decorative raster and vector icons.
- `PushClip` and `PopClip` for content clipping.

TextField does not require a component-specific drawing command.

A single-line field registers its editor rectangle as a horizontal scroll viewport, maintains a retained offset through the shared scroll state, and keeps the active caret visible. The retained layout and DrawText command use the same bounded paragraph width, while `paragraph_offset` translates its rendering inside the fixed editor clip. Fixed decorations such as leading and trailing icons remain outside that viewport. The field does not create an internal ScrollView node.

A multiline field uses the same retained scroll state on the vertical axis when its content is taller than its viewport. Pointer hit testing, selection, composition, candidate geometry, and caret painting all resolve through the same translated text origin. Up and Down preserve a preferred horizontal caret position, while Home and End move to visual line boundaries.

The field participates in the same retained scroll chain as other scrollable nodes. Wheel and touch movement scroll the field first and pass unconsumed movement to an enclosing scroll container. Mouse and pen dragging retain text selection semantics, and dragging a selection beyond the viewport advances the internal text offset. Manual scrolling temporarily suppresses automatic caret reveal until editing or navigation resumes.

The caret blink timer and label transition are retained by the TextField extension. They request frames through the existing mounted extension scheduling path and respect resolved reduced-motion durations. Pointer or keyboard edits reset the visible caret phase. TextField uses `caret_width` for the painted stroke and fits it inside the clip at the trailing edge. Text hit testing, scrolling, and platform input geometry use the logical caret boundary from the platform layout.

The first pointer behavior includes:

- Focus mouse and pen input on press.
- Focus touch input and place its caret only after a completed tap.
- Drag to extend selection.
- Preserve pointer cancellation behavior when a parent scroll gesture wins.

Shared UiWindow recognition selects a word immediately on a mouse or pen double-click. Touch double-tap selects on the second release so a drag beginning with the second press can still yield to scrolling. UiWindow also owns long-press word selection and paints the shared selection menu and handles in a framework-owned `FrameworkOverlay` above the shared layer stack. The overlay state owns its stable RenderNode and is appended to the synthetic RuntimeRoot scene without becoming a mounted application or layer-stack node. It is not a public Layer entry and does not participate in application-layer ordering or focus containment, but UiWindow Back routing hides it before consulting public layers. Magnifiers and more advanced gesture behavior remain incremental.

Selection overlay intent is independent of its current visibility. Related drag, momentum, and overscroll activities temporarily suppress the overlay until their existing Begin/End/Cancel lifecycles finish. Missing geometry likewise suppresses presentation without discarding the intent or logical range. Visible geometry restores only a previously requested overlay; Copy, Back, focus changes, and explicit dismissal do not leave a pending reopen request. Touch Down outside the overlay still enters ordinary pointer arbitration, so a canceled tap or a winning scroll cannot clear the range or consume a child button or link action.

## Theme

`TextFieldStyle` in [theme.h](../../include/huxerui/theme.h) is the typed Theme value for field appearance. Its variant bundles supply fills, borders, corner radii, and minimum heights.

Flat and Material Theme definitions provide their own TextField styles. Material defaults to a 56-unit Filled field and provides 56-unit Standard and Outlined alternatives. Flat defaults to a 40-unit Standard field, retains a 36-unit Outlined alternative, and provides a 44-unit Filled alternative. Both themes paint a 2-unit caret, while Material uses 24-unit leading and trailing icons and Flat uses 18-unit icons.

Standard uses a transparent container and bottom state indicator. Filled adds a top-rounded container fill to the same indicator geometry. Outlined keeps the same radius on all four corners and uses a segmented top outline so a floating label does not depend on painting an opaque background over the border. Material Filled and Standard indicators use `on_surface_variant`, while Outlined uses `outline`; hover, focus, and disabled colors also resolve from the selected variant bundle. Container backgrounds are painted only within the editor frame and never extend through supporting text.

Each `TextFieldVariantStyle` owns one variant's `VisualFill` background, optional disabled fill, stateful indicator or outline colors, `CornerRadii`, and minimum height. An absent disabled background falls back to the ordinary background. `TextFieldStyle::variant` selects the Theme default, while an explicit component variant selects the corresponding bundle without duplicating active aliases. TextField draws its hover, focus, validation, and disabled states from these style values, so the common node indication and focus ring do not surround supporting text. Hover changes only the variant's indicator or outline, and disabled colors are resolved per element instead of reducing the opacity of the complete field subtree.

`TextFieldStyle::show_label` controls only visual label layout and painting. When it is `false`, `TextField::Label()` still supplies the accessibility label, while the placeholder remains in the editing line and Outlined paints a continuous border without a cutout. `label_spacing` defines the gap between a floating label and editable text for Standard and Filled fields.

`TextFieldStyle` belongs in `theme.h` with the existing built-in component styles. Its type is also its Theme override identity.

Input configuration, an explicit variant, label and placeholder text, and icon assets belong to the component declaration.

The selection overlay resolves handle colors from the focused control: `TextFieldStyle::caret` for editable text and the current Theme primary color for `SelectionArea`. Its horizontal toolbar reuses the active `MenuStyle` surface fill, foreground, corner radii, shadow, item geometry, separators, and indication without adopting the public Menu service lifecycle. Resource-backed surface fills resolve against the focused control's resource context and Locale before overlay painting. Cut, Copy, Paste, and Select All resolve from the built-in `huxerui` resource domain and use the inherited Locale for shaping. An explicitly provided `TextSelectionMenuLabels` Environment value overrides each non-empty field for that subtree, while empty fields continue to resolve their localized framework defaults. Editing actions execute on release; Cut, Copy, and Paste make the menu non-interactive until the indication exit animation finishes, while Select All retains the overlay, exposes range handles, and recomputes the remaining actions. The public Menu service keeps separate LayerStack lifecycle, anchoring, focus, and dismissal.

A collapsed TextField selection uses a caret-anchored menu without selection handles. This allows an empty field to expose Paste when the clipboard contains text. A range selection uses the same menu together with themed start and end handles.

## Keyboard path

Text-producing input and control keys remain separate:

- Committed and composing text from the platform enters through input commands.
- `KeyEvent` handles navigation, shortcuts, deletion, submission, and focus traversal.
- `KeyIntercept` runs along the active focus route before text editing so an explicit parent policy can override it.
- The focused TextInputClient receives text-related keys before component and generic activation behavior.
- An unhandled key continues through `NodeExtension::OnKey`, then the focused View's `KeyDown` or `KeyUp` decision event.
- Unhandled events reach the window shortcut handler before framework and platform defaults.
- A platform adapter suppresses platform character events that duplicate an IME commit.

Default behavior is:

| Key | TextField behavior |
| --- | --- |
| Left and Right | Move by grapheme cluster, or extend with Shift |
| Ctrl+Left and Ctrl+Right | Move to the previous or next word start |
| Option+Left and Option+Right | Move to the previous word start or next word end |
| Command+Left and Command+Right | Move to the beginning or end of the visual line |
| Up and Down | Move between visual lines in multiline fields |
| Command+Up and Command+Down | Move to the beginning or end of the document |
| Home and End | Move to visual line boundaries in multiline fields and document boundaries in single-line fields |
| Ctrl/Command+Home and Ctrl/Command+End | Move to document boundaries |
| Page Up and Page Down | Move by one visible field viewport in multiline fields |
| Backspace and Delete | Delete selection or adjacent grapheme |
| Ctrl/Option+Backspace and Ctrl/Option+Delete | Delete by the corresponding word boundary convention |
| Command+Backspace and Command+Delete | Delete to the visual line boundary |
| Ctrl/Command+Z | Undo the last TextField-local edit group |
| Ctrl+Y or Ctrl/Command+Shift+Z | Redo the last undone edit group |
| Tab and Shift+Tab | Move focus |
| Enter | Follow the configured input action; insert a newline for multiline `Newline`, otherwise submit |
| Escape | Cancel composition, otherwise follow UiWindow focus behavior |
| Enter and Space activation | Not applied to TextField |

Shift extends the selection for movement commands. macOS `NSTextInputClient` selectors retain their word, line, page, and document semantics when converted to common key events.

## Editing actions and clipboard

UiWindow maps Ctrl/Command+A, C, V, and X to typed `TextEditingAction` values and dispatches them through the same entry point as the touch selection menu. The focused editor owns its editing actions even when its read-only configuration closes the input session. UiWindow checks the current configuration before asking the selection client. Read-only input blocks Cut and Paste. Secure input blocks Copy and Cut. SelectionArea supports Select All and Copy. Editors retain ownership of their own history.

The application service owns one public `Clipboard` per Runtime and delegates to its stable `PlatformClipboard` capability. Runtime retirement disconnects this service before releasing the platform capability. Clipboard tasks capture their inputs and platform resources before they can outlive the service.

Built-in editing actions use the synchronous plain-text operations on platforms that provide them. Paste availability checks use advertised formats where available and permit a paste attempt when the format is unknown. They do not request clipboard payloads.

`GetTextEditingActionStatus()` returns `UseDefault` to leave an action with the framework, `Disabled` to suppress it, or `Handle` to execute it through the selection client. The menu queries availability without reading payloads. Dispatch checks the status again and consumes a claimed action immediately, including when an asynchronous clipboard operation later fails. Editors revalidate the original editing target, selection, and policy before applying a delayed result.

When a platform lacks synchronous clipboard access, unhandled Copy, Cut, and Paste shortcuts reach the platform's default editing path. The Web input proxy blocks browser Copy and Cut for secure entries. Read-only clients do not open a text-input session.

`ClipboardItem` owns alternative representations of one selection. A write publishes the complete item or reports failure. Each item can contain plain UTF-8 text, an HTML fragment, encoded image content, and custom MIME bytes. Custom names are normalized before dispatch. Custom data has a shared 16 MiB budget and at most 16 formats per item. HTML and plain text are supplied independently. `FromAttributedText()` generates both, and `ToAttributedText()` imports supported inline styles, links, and breaks. The converter never loads external content. Raw HTML remains available to the application.

A read starts from one platform item or clipboard offer. The platform chooses the first item advertising a requested representation. All accepted representations come from that item, and any read that loses track of the original item fails as a whole. A failure confined to one representation leaves the other readable values available. A fully captured item owns its data independently of future clipboard changes. File operations capture a complete ordered list through the existing file reference factories and publish copy semantics.

Windows captures named formats while holding one clipboard open. Its custom format name identifies a versioned byte-length header so allocation padding never becomes application data. GDK publishes a provider union and keeps a single format list, change signal, cancellation token, and timeout for one capture. AppKit captures one pasteboard item and verifies the pasteboard change count. UIKit retains one item provider. Apple custom formats share a deterministic UTI derived from the MIME name. Android resolves one captured clip item on a worker. Its read-only content provider serves custom and image MIME streams from the same granted URI.

Web starts browser access during the triggering user action and delivers Promise results through the Task dispatcher. The browser handles text, HTML, supported images, and custom web formats where available. All payloads are read from one browser `ClipboardItem`. Canceling a Task detaches its completion. Platforms with synchronous publication perform the write when `WriteItemAsync()` is called and deliver the result through the Task.

## Android adapter

`HuxerUIView` remains the Android input target. `HuxerUIInputConnection` implements the input connection for the currently focused HuxerUI text input client.

The Android adapter:

- Implements `onCheckIsTextEditor()`.
- Returns a custom `HuxerUIInputConnection` derived from `BaseInputConnection`.
- Maps `setComposingText()` to composition commands.
- Maps `commitText()` to `CommitText`.
- Maps `finishComposingText()` to `FinishComposition`.
- Maps `setSelection()` to `SetSelection`.
- Maps both surrounding deletion variants with their explicit units.
- Maps `performEditorAction()` to the typed common action and rejects stale or mismatched session actions.
- Uses `updateSelection()` to synchronize selection and composition.
- Uses `CursorAnchorInfo` for candidate and insertion marker geometry.
- Uses the complete Android View-to-screen matrix, including ancestor transforms and scrolling, and re-publishes monitored cursor geometry when a pre-draw detects that matrix changed.
- Rejects callbacks carrying a stale HuxerUI session.
- Checks the active session and request order before honoring a posted software-keyboard show or hide request. Dismissal keeps the input connection active.
- Resizes the logical viewport for visible IME insets and asks ancestor scroll containers to reveal the active caret.
- Forwards raw touch input while UiWindow owns long-press recognition, editing actions, and the HuxerUI-drawn selection overlay.
- Uses password input type without suggestions and withholds surrounding, selected, and extracted text from secure input connections.

The adapter can retain a bounded surrounding-text mirror for Android query behavior. Document ownership remains with the client.

`restartInput()` is used when client ownership or input configuration changes, or when an authoritative state replacement invalidates the current IME composition. Normal recomposition and selection changes use ordinary state updates.

## Apple adapter

An AppKit-specific client owned by `MacTextInput` conforms to `NSTextInputClient` on macOS. The host view remains the first responder and exposes the client's explicit `NSTextInputContext`.

On iOS, a private non-accessible view owned by the `UIKitTextInput` bridge conforms to `UITextInput`, while the HuxerUI drawing host remains a separate accessibility container. UIKit's UTF-16 positions, selection, marked text, keyboard traits, action keys, and geometry queries translate directly to the same UiWindow session and command protocol. The UIKit bridge keeps only session and UIKit text-service bookkeeping; the controlled `TextEditingValue` remains authoritative in the shared TextField client.

The iOS proxy becomes first responder on a show request and resigns on a hide request. Both operations preserve the active HuxerUI session.

The macOS adapter maps:

- `insertText` to `CommitText`.
- `setMarkedText` to begin or update composition.
- `unmarkText` to `FinishComposition`.
- `selectedRange` and `markedRange` to UiWindow state queries.
- `attributedSubstringForProposedRange` to bounded context queries.
- `firstRectForCharacterRange` to text geometry.
- `characterIndexForPoint` to client hit testing.
- Returns no attributed substring for secure input.
- Enables Secure Event Input only while a secure field is active and the application is active.

Outside an active composition, hardware keys first enter UiWindow without inferred text so shortcuts, navigation, and explicit key decisions can consume them before AppKit interpretation. Unhandled keys continue through `NSTextInputContext`, which alone produces committed or marked text. While marked text is active, AppKit retains first ownership of the event so IME composition is not bypassed; command selectors that AppKit does not consume still map to common key events.

The host view remains first responder while one HuxerUI editable node transfers focus to another. UiWindow still creates a new logical input session so delayed callbacks from the previous client are rejected.

## Windows adapter

The Windows adapter uses IMM32:

- `WM_IME_STARTCOMPOSITION` begins composition.
- `WM_IME_COMPOSITION` publishes composing updates and committed results.
- `WM_IME_ENDCOMPOSITION` finishes composition when required.
- Candidate and composition window placement uses current caret geometry.
- `WM_IME_CHAR` and `WM_CHAR` paths are filtered to prevent duplicate commits.
- Selection, deletion, and control keys continue through the key path where IMM32 does not provide a direct operation.


## Linux adapter

Font sizes are application-owned logical baselines; the renderer resolves every font description to `Font::Size() * effective_text_DPI / 96` logical units, including attributed runs and custom font families.
The host reads effective DPI from the drawing widget's GTK PangoContext, after GTK resolves settings and CSS, and falls back to 96 for an invalid or unspecified value.
It borrows only DPI: HuxerUI retains its private Pango font map and context for custom font registration.
GTK applies device scaling separately; surface scale is neither multiplied into nor divided out of the text DPI.

Initial DPI is synchronized before shared window initialization.
The canvas chains GTK's `css_changed` handler before requesting a frame; the next frame commit refreshes DPI and invalidates shared text metrics before building geometry.
Snapshot replay never changes DPI while painting a previously committed frame.
A repeated effective DPI is a no-op; a changed value clears only cached shaped paragraphs, preserving image caches, font registration, and independently retained text layouts.

The GTK host owns one `GtkIMMulticontext` for its drawing area and lets GTK select the active desktop input-method module.
Key events enter the input context before the shared direct-key path; accepted events produce preedit or commit callbacks, while declined events continue through UiWindow key handling in order.

The bridge:

- Maps `preedit-start`, `preedit-changed`, and `preedit-end` to the shared composition command sequence.
- Converts GTK UTF-8 byte positions to the common UTF-16 offset space without splitting a code point or surrogate pair.
- Maps committed UTF-8 text to `CommitText` and clears local composition bookkeeping after UiWindow processes the command.
- Publishes bounded non-secure surrounding text with both cursor and anchor positions and maps `delete-surrounding` to Unicode-code-point deletion.
- Updates the input-method candidate location from the latest transformed caret geometry.
- Maps input purpose, autocorrection, capitalization, and secure-entry policy to GTK input purpose and hint values.
- Disables inline preedit and surrounding-text publication for secure sessions while retaining GTK password input purpose and private-input hints.

The implementation does not contain separate XIM, IBus, or Fcitx state machines. GTK's input-method module boundary owns those desktop-specific protocols.

## HarmonyOS adapter

The ArkTS host connects the focused HuxerUI editor to the system input method controller. The bridge translates UTF-16 selection, surrounding text, preview composition, and editing commands to the active UiWindow session. Secure sessions suppress surrounding text.

The system controller has one connection across all windows. The ArkTS host serializes connection changes and detaches only the current connection. Callbacks carry the originating window ID and session token so a retired session cannot edit the next focused client.

## Web adapter

The browser adapter keeps an input proxy for the current session and translates DOM composition and input events into the same command batches. See [Browser text input and IME](web.md#browser-text-input-and-ime) for surrounding-text mapping, restart identity, and geometry.

## PlatformView focus

An embedded PlatformView and a HuxerUI TextInputClient cannot own the same host input session.

When input enters a PlatformView:

- UiWindow ends the active HuxerUI client session.
- The native PlatformTextInput endpoint stops the HuxerUI input connection.
- The platform control receives platform focus and owns its IME directly.

When focus returns to a HuxerUI editable node, UiWindow creates a new session.

Focus transfer resolves the PlatformView identity from the current committed `RenderComposition`; a delayed platform focus notification for an obsolete identity is ignored.
UiWindow remains authoritative for HuxerUI hit-test and focus ordering, while the platform-derived UiWindow and its native input endpoint perform platform focus transfer and input dispatch.
These focus and IME lifecycle messages are internal adapter coordination rather than PlatformModule events.
A library may emit a typed application event describing a platform focus change, but that event neither starts nor ends a HuxerUI text-input session.
The macOS backend implements this transfer through AppKit first-responder synchronization. AppKit retains key-view traversal within one PlatformView subtree, while traversal across its boundary returns to UiWindow focus order. Android synchronizes global platform focus changes, UiWindow-directed focus, Tab traversal, and IME dismissal through its owning `HuxerUIView`. iOS synchronizes touch and UiWindow-directed focus; hardware-keyboard traversal across the PlatformView boundary is not supported.
See [PlatformView composition](platform-content.md#platformview) for the drawing and input boundary.

## Custom editors

A custom editor exposes a stable `TextInputClient` through its retained node extension. The client can map the UiWindow session to its own editing session and supply bounded context and local layout geometry from its document model. The editor keeps its history, selection policy, transactions, and composition baseline. See the [custom editor example](../../examples/custom_editor/main.cpp) for a client that handles document editing and painting.

## Threading and reentrancy

Text input mutation occurs on the application thread owning UiWindow.

The native input bridge must marshal platform callbacks to that thread before calling UiWindow. UiWindow validates session identity again after marshaling.

Applying one batch must not synchronously recompose the application in the middle of the reducer. The client publishes its resulting event and requests a frame after the atomic mutation completes.

Platform input update, restart, and close actions are executed after the client returns from mutation. A platform callback must not recursively reopen the same input connection while a command batch is being applied.

Destroying a host view invalidates every active session before destroying clients or platform input objects.

## Validation

The [text model tests](../../tests/unit/text_input.cpp) cover UTF-16 conversion, graphemes, atomic rejection, composition, and controlled replacements. The [session tests](../../tests/runtime/text_input.cpp) exercise focus and ownership, stale commands and queries, geometry synchronization, and software keyboard requests. The [TextField tests](../../tests/runtime/text_field.cpp) cover layout, editing behavior, masking, selection, and gestures.

Platform validation checks candidate geometry, keyboard presentation, IME composition and cancellation, and handoff to embedded native editors on available hosts. Use mixed-direction text, emoji, and combining marks to verify offsets and hit testing across the boundary.
