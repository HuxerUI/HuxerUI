# Text Input and TextField Design

Status: implemented foundation with Android, iOS, macOS, and Windows platform adapters

This document defines the target text editing model, input session lifecycle, platform IME boundary, and built-in `TextField` behavior for HuxerUI. The design builds on the existing controlled control model, `Runtime` focus ownership, `PlatformAdapter`, retained `NodeExtension` state, typed events, and retained-scene rendering.

The design also defines the extension boundary required by complex editable components. SweetEditor is the reference integration: it should reuse the HuxerUI focus and platform input path without replacing its document model with the built-in TextField state.

This document records the implemented architecture and the remaining extension direction.

## Goals

- Provide a controlled built-in TextField with selection and composition state.
- Support native IME composition without inferring edits from full text snapshots.
- Keep focus and input session ownership in `Runtime`.
- Keep native input connection behavior in `PlatformAdapter`.
- Let editable components own their text, selection, composition, and editing semantics.
- Isolate delayed callbacks from old native input sessions.
- Use one platform input path for TextField, SweetEditor, and future custom editable components.
- Preserve UTF-8 strings in the public C++ API while using one explicit offset convention across platforms.
- Grow the validated single-line foundation into multiline editing without changing the platform input protocol.

The initial design deliberately does not include:

- A second application runtime for text input.
- A full-document mirror owned by `Runtime` or `PlatformAdapter`.
- Mutation inference by comparing two complete text snapshots.
- A Flutter-style delta buffer and revision protocol.
- Editor history, transactions, or linked-editing behavior in the common input protocol.
- Platform-specific IME types in common public headers.
- A temporary TextField that accepts characters but cannot represent native composition.

## Architecture

The text input path has two related but separate models:

```text
TextEditingValue
    declarative value of the built-in TextField

TextInputCommandBatch
    ordered editing intent delivered by a native input system
```

`TextEditingValue` is not the platform mutation protocol. A normal TextField reduces input commands into a new `TextEditingValue`. A complex component can apply the same commands to its own document core without creating a full value snapshot.

The complete path is:

```text
native IME
    |
platform input adapter
    |
TextInputCommandBatch
    |
Runtime text input session
    |
focused TextInputClient
    |                       |
TextField reducer           SweetEditor bridge
    |                       |
TextEditingValue            EditorCore
```

The ownership boundaries are:

| Layer | Responsibility |
| --- | --- |
| Platform input adapter | Normalize native callbacks and operate the native input connection |
| Runtime | Focus ownership, session identity, routing, and stale callback rejection |
| TextInputClient | Text state, edit semantics, context queries, and text geometry |
| TextField | Controlled value, simple editing reducer, selection, caret, and painting |
| SweetEditor | Document state, transactions, history, and editor-specific behavior |

`Runtime` does not own text content. `PlatformAdapter` does not decide how an edit changes a value. An editable component does not call native IME APIs directly.

## Text model

The common types belong in:

```cpp
#include <huxerui/text_input.h>
```

A representative public model is:

```cpp
using TextOffset = std::int64_t;

enum class TextAffinity {
  Upstream,
  Downstream,
};

struct TextRange {
  TextOffset start = 0;
  TextOffset end = 0;

  [[nodiscard]] bool IsCollapsed() const noexcept;
  [[nodiscard]] TextOffset Length() const noexcept;
};

struct TextSelection {
  TextOffset anchor = 0;
  TextOffset active = 0;
  TextAffinity affinity = TextAffinity::Downstream;

  [[nodiscard]] bool IsCollapsed() const noexcept;
  [[nodiscard]] TextRange Range() const noexcept;
};

struct TextEditingValue {
  std::string text;
  TextSelection selection;
  std::optional<TextRange> composition;

  static TextEditingValue FromText(std::string text);
};
```

The model follows these rules:

- Text is stored as UTF-8.
- Every `TextOffset` is measured in UTF-16 code units.
- `TextRange` is ordered and does not preserve direction.
- `TextSelection` preserves direction through `anchor` and `active`.
- Selection and composition ranges are measured in the same current text.
- A missing composition and a collapsed composition are distinct states.
- Session identity and platform synchronization revisions are not part of `TextEditingValue`.
- Invalid offsets, reversed ranges, and offsets inside a UTF-16 surrogate pair are rejected at protocol boundaries.

UTF-16 offsets are an explicit interoperability choice. Android, Apple text input APIs, Windows input APIs, and SweetEditor already operate in this coordinate system. Keeping one offset convention prevents every adapter from inventing a different conversion policy.

Application code should not normally manipulate UTF-16 offsets directly. TextField and reusable text utilities provide validated range and movement operations.

## Input commands

Native input is normalized into typed commands:

```cpp
enum class TextInputCommandKind {
  SetSelection,
  BeginComposition,
  UpdateComposition,
  CommitText,
  FinishComposition,
  CancelComposition,
  DeleteSurrounding,
};
```

The command representation carries only fields relevant to its kind:

```cpp
enum class TextInputCoordinateSpace {
  Text,
  Composition,
};

enum class TextInputUnit {
  Utf16CodeUnit,
  UnicodeCodePoint,
};

struct TextInputCommand {
  TextInputCommandKind kind;
  TextInputCoordinateSpace coordinate_space =
      TextInputCoordinateSpace::Text;
  std::optional<TextRange> target;
  std::optional<TextSelection> selection_after;
  std::string text;
  TextOffset delete_before = 0;
  TextOffset delete_after = 0;
  TextInputUnit delete_unit = TextInputUnit::Utf16CodeUnit;
};

struct TextInputCommandBatch {
  TextInputSessionId session_id;
  std::vector<TextInputCommand> commands;
};
```

`coordinate_space` applies to `target`. `selection_after` is always expressed in absolute UTF-16 offsets in the resulting text. A platform adapter converts native relative cursor placement before constructing the command.

The protocol has these semantics:

- One native input callback produces one ordered batch.
- A batch is validated before any visible mutation occurs.
- Commands execute in order against a staged state.
- A later command sees the state produced by earlier commands in the batch.
- A rejected command rejects the complete batch.
- The client publishes at most one resulting state change for a successful batch.
- The Runtime rejects a batch whose session does not match the active client.
- The platform adapter never reconstructs a command by diffing two full text snapshots.

`SetSelection` changes selection without changing text.

`BeginComposition` records the replacement baseline and establishes a composition range.

`UpdateComposition` replaces the active composition or its explicit target, updates the composition range, and applies the requested selection.

`CommitText` replaces the active composition or selection and clears the composition.

`FinishComposition` keeps the provisional text and clears the composition marker.

`CancelComposition` restores the client-owned composition baseline.

`DeleteSurrounding` removes text around the current selection using the explicit unit supplied by the native platform.

The protocol does not include undo grouping, editor transactions, document revision IDs, linked editing, or full text buffers. Those are client concerns.

## Text input client

`TextInputClient` is the common capability implemented by the built-in TextField and custom editable components:

```cpp
class TextInputClient {
public:
  virtual ~TextInputClient() = default;

  [[nodiscard]] virtual TextInputConfiguration Configuration() const = 0;
  [[nodiscard]] virtual TextInputState State() const = 0;

  virtual TextInputState BeginTextInput(
      TextInputSessionId session_id
  ) = 0;

  virtual TextInputApplyResult ApplyTextInput(
      const TextInputCommandBatch& batch
  ) = 0;

  [[nodiscard]] virtual TextInputContext QueryTextInputContext(
      TextInputSessionId session_id,
      TextOffset start,
      TextOffset length
  ) const = 0;

  [[nodiscard]] virtual TextInputGeometry QueryTextInputGeometry(
      TextInputSessionId session_id,
      TextRange range
  ) const = 0;

  [[nodiscard]] virtual TextInputPositionResult QueryTextInputPosition(
      TextInputSessionId session_id,
      Point point
  ) const = 0;

  virtual TextInputKeyResult HandleTextKey(const KeyEvent& event) = 0;

  virtual void EndTextInput(
      TextInputSessionId session_id,
      TextInputEndReason reason
  ) = 0;
};
```

The result and state types are public because `TextInputClient` is a public extension point. Their required semantics are:

- A current selection and optional composition.
- A bounded text context query.
- Accepted, rejected, read-only, and session-mismatch outcomes.
- A request to update or restart the native input connection.
- Caret, range, and point hit-test geometry.

`TextInputApplyResult` reports only the result code and any required native synchronization action.
Runtime derives whether state changed from `TextInputState` revisions instead of accepting a second, potentially inconsistent changed flag.

A `NodeExtension` exposes the capability through one optional hook:

```cpp
virtual std::shared_ptr<TextInputClient> GetTextInputClient() noexcept {
  return {};
}
```

A focusable node can expose at most one client. Multiple text input clients on the same node are rejected instead of receiving an implicit modifier priority.

The extension returns a stable shared client. Runtime retains that client only for the active input session, allowing it to call `EndTextInput()` after the owning extension is reconciled away without retaining a raw extension pointer.

The built-in TextField installs one retained extension. A future SweetEditor component installs its own retained extension and returns its bridge from the same hook.

## Text selection client

Editable and read-only selectable content share selection gestures and overlay presentation without sharing IME ownership.
`TextSelectionClient` is therefore a separate capability from `TextInputClient`:

```cpp
class TextSelectionClient {
public:
  virtual ~TextSelectionClient() = default;

  virtual bool SelectWord(Point position) = 0;
  virtual bool ExtendSelection(Point position, bool start_handle) = 0;
  virtual bool QuerySelectionGeometry(Rect& start, Rect& end) const = 0;
  virtual Color SelectionHandleColor() const noexcept = 0;
};
```

Selection points and geometry use the owning node's local logical coordinates.
Runtime maps them through the node's resolved presentation transform exactly once at the host boundary.
The optional editing-action methods allow read-only clients to expose Copy and Select All through the shared clipboard menu without pretending to be IME clients.

A `NodeExtension` exposes at most one selection client through `GetTextSelectionClient()`.
Runtime borrows this pointer only during dispatch and never retains it beyond the owning extension's lifetime.
TextField exposes both input and selection clients; `SelectionArea` exposes only a selection client and never starts an IME session.

`SelectionArea` prepares descendant text-layout and relative-transform value snapshots after presentation resolution.
It does not retain descendant node pointers, and unchanged descendant geometry keeps its foreground PaintSequence clean.

## Input configuration

The client describes native keyboard and submission behavior with typed configuration:

```cpp
enum class TextInputType {
  Text,
  Email,
  Number,
  Decimal,
  Phone,
  Url,
};

enum class TextCapitalization {
  None,
  Characters,
  Words,
  Sentences,
};

enum class TextInputAction {
  Default,
  Done,
  Go,
  Next,
  Search,
  Send,
  Newline,
};

struct TextInputConfiguration {
  TextInputType type = TextInputType::Text;
  TextCapitalization capitalization = TextCapitalization::None;
  TextInputAction action = TextInputAction::Default;
  bool multiline = false;
  bool secure = false;
  bool autocorrect = true;
  bool read_only = false;
};
```

Platform-specific behavior is not represented by untyped string properties. New common values can be added when at least one component and platform integration need them.

Submission actions have common Runtime semantics:

| Action | Behavior |
| --- | --- |
| `Default` | Resolve to `Done` for single-line input or `Newline` for multiline input |
| `Done`, `Go`, `Search`, `Send` | Trigger `OnSubmitted` |
| `Next` | Trigger `OnSubmitted`, then move to the next focusable node without wrapping |
| `Newline` | Insert a newline in a multiline TextField |

Terminal soft-keyboard dismissal remains a platform responsibility. It does not clear HuxerUI focus merely to hide the keyboard. Native IME actions and hardware Enter both pass through the same focused TextInputClient key path.

Secure entry uses the same state and command protocol. The retained `TextEditingValue` contains the real text, while TextField builds a separate single-line mask layout and draws one bullet per grapheme. Copy and Cut are disabled. Platform adapters prevent native surrounding-text and extracted-text queries from returning the value while preserving the internal context needed for command routing and composition. Secure and multiline configurations are mutually exclusive.

The shared `SemanticFrame` follows the same privacy boundary.
An ordinary TextField publishes its committed value, normalized UTF-16 selection, label, placeholder, validation state, focus, and supported SetText and SetSelection actions so native accessibility can edit and announce it.
These actions enter the retained TextField reducer and the same Runtime invalidation and active-session synchronization path as platform text input; they do not create a second editor protocol.
A read-only field omits SetText but retains SetSelection, while a secure field may expose SetText but omits SetSelection.
A secure TextField semantic frame never contains TextField-owned plaintext, selected text, surrounding text, composition text, clipboard content, or a plaintext-derived state description; Copy and Cut remain unavailable through the existing editing policy.
Application-authored labels, hints, errors, and identifiers remain trusted metadata and must not copy the protected value.
The semantic frame carries protected state without a value or protected grapheme count.
Accessibility focus remains platform-owned and does not start a text-input session; an explicit semantic input-focus action follows the existing Runtime focus and TextInputClient path.
See [Semantics and Accessibility Design](semantics.md) for the complete frame and action contract.

## Runtime session ownership

`Runtime` owns one active HuxerUI text input session per host view:

```text
focused node identity
focused TextInputClient
monotonic TextInputSessionId
last synchronized client revision
native input connection state
```

Session IDs are non-zero, monotonically increasing, and not reused during the Runtime lifetime.

The lifecycle is:

```text
focus editable node
    begin client session
    start native input

recompose same node and client
    preserve session
    synchronize changed state when required

move focus
    finish old client session
    stop old native input
    begin new session when the new node is editable

unmount, disable, or make client read-only
    finish current composition
    stop native input

native control takes focus
    stop HuxerUI text input
    let the native control own the IME
```

An asynchronous platform callback captures the session ID at entry. If focus or ownership changes before the callback reaches Runtime, it is rejected without accessing the former client.

Normal focus loss finishes composition by retaining provisional text and clearing the composition marker. Escape first cancels an active composition. When there is no active composition, Escape follows the existing Runtime focus behavior.

Mouse and pen presses focus a node before native input synchronization. Touch focus remains pending until the gesture resolves as a tap. TextField also defers its touch caret placement until release. If movement wins a scroll gesture, both pending operations are cancelled, so scrolling across an editor does not focus it or display the software keyboard. A recognized long press commits focus before opening text selection.

Native input starts or updates after the current pointer dispatch completes. The platform therefore sees the resulting selection and geometry rather than the caret position from the previous frame.

Modal focus capture and restoration use the same lifecycle. Restoring focus to an editable node creates a new native input session rather than reviving an old session ID.

## Platform input capability

Text input is a cohesive optional capability of `PlatformAdapter`. It should not expand `PlatformAdapter` into a collection of unrelated per-command methods:

```cpp
class PlatformTextInput {
public:
  virtual ~PlatformTextInput() = default;

  virtual void Start(
      TextInputSessionId session_id,
      const TextInputConfiguration& configuration,
      const TextInputState& state,
      const TextInputGeometry& geometry
  ) = 0;

  virtual void Update(
      TextInputSessionId session_id,
      const TextInputState& state,
      const TextInputGeometry& geometry
  ) = 0;

  virtual void Restart(
      TextInputSessionId session_id,
      const TextInputConfiguration& configuration,
      const TextInputState& state,
      const TextInputGeometry& geometry
  ) = 0;

  virtual void Stop(TextInputSessionId session_id) = 0;

  virtual void RequestShow(TextInputSessionId session_id) {
    static_cast<void>(session_id);
  }
};
```

`PlatformAdapter` provides a nullable capability:

```cpp
virtual PlatformTextInput* TextInput() noexcept {
  return nullptr;
}
```

A test host, headless host, or incomplete platform does not need an empty input implementation. Hardware navigation can continue to use ordinary key events when no platform text input capability exists.

Focus, input-session ownership, and software-keyboard visibility remain separate. Hiding a software keyboard does not clear HuxerUI focus or end the active session. A confirmed tap on the same focused text client calls `RequestShow()` with the active session ID. Platforms without a software keyboard keep the default no-op implementation.

Start, Update, and Restart receive state and geometry from the same Runtime snapshot.
Platform adapters do not call back into Runtime merely to recover the current caret during Start or Restart.

The platform adapter calls Runtime through session-aware entry points:

```text
HandleTextInputCommands
PerformTextInputAction
QueryTextInputContext
QueryTextInputGeometry
QueryTextInputPosition
```

It does not retain a raw TextField or `TextInputClient` pointer across native callbacks.

## State synchronization

The native input connection needs selection, composition, and candidate geometry. It does not need an authoritative copy of the complete client text.

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
The text-content revision increases only when text changes, allowing Runtime to distinguish layout-affecting edits from selection and composition-marker updates.
Both revisions are Runtime synchronization details.
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
Runtime synchronizes the new state after reconciliation.
A configuration or ownership change requests a native restart; an ordinary selection change requests only an update and paint invalidation.
Client-owned geometry changes update candidate geometry without restarting an active native composition.

## TextField API

The built-in component is declared with the other fundamental Views in:

```cpp
#include <huxerui/view.h>
```

Normal application code continues to use the umbrella header:

```cpp
#include <huxerui/huxerui.h>
```

Representative use is:

```cpp
[[huxerui::scope]]
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

The component uses typed events:

```cpp
struct TextFieldEvents {
  struct Changed
      : Event<TextFieldEvents, void(const TextEditingValue&)> {};

  struct Submitted
      : Event<TextFieldEvents, void()> {};
};
```

`TextFieldEvents` belongs in `event.h`, following the existing `ToggleEvents` convention.

`OnChanged()` and `OnSubmitted()` are convenience wrappers over the matching typed events.

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
They are decorative content within the editor geometry rather than independent controls.
Their occupied width participates in text layout, selection, caret geometry, scrolling, and native text-input geometry.
Vector assets resolve the TextField state color, while raster assets retain their encoded colors.

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

`ValidationResult` distinguishes `None`, `Valid`, `Invalid`, and `Pending` and retains its message as a StringVariant until TextField composition. TextField consumes the resolved result only for presentation: an invalid result uses the Theme validation color, border width, and supporting message layout. It does not reject an edit, mutate the controlled value, or decide whether validation runs on change, focus loss, or submission. Those trigger policies remain application state and can later be coordinated by a Form layer without changing TextField.

Synchronous rules return a `ValidationResult` and `Validate()` stops at the first result that is not valid. `Required` and `EmailAddress` use `validation_required` and `validation_email` from the built-in `huxerui` resource domain unless the application supplies another StringVariant. Custom callables remain equally supported. `Pending` is supplied by asynchronous application state and may display a neutral supporting message without the invalid border; TextField does not own asynchronous work.

Visual properties remain Theme styles or modifiers rather than growing one-off TextField styling methods.

Password entry stays on the same component:

```cpp
return TextField(password)
    .Secure()
    .Placeholder("Password")
    .OnChanged([password](const TextEditingValue& value) mutable {
      password = value;
    });
```

`Secure()` is a convenience for `TextInputConfiguration::secure`. It does not create a second value model or a separate PasswordField component.

The public API should expose one value model. It should not provide a second string-only TextField with different change events. Simple applications use `TextEditingValue::FromText()`.

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

`SelectionArea` owns retained selection state and text layout geometry, but it does not expose a `TextInputClient` and never starts an IME session. Runtime routes Copy and Select All to the focused selection area through the same `TextEditingAction` entry points used by editable clients. Cut and Paste remain unavailable for static content.

Desktop hosts use pointer drag selection and the standard Ctrl or Command shortcuts. Touch input uses runtime-owned long-press word selection and a shared HuxerUI selection overlay with draggable handles. A newline separates adjacent descendant `Text` nodes in copied plain text.

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
- Runtime synchronizes the resulting selection and composition to the host.

When composition produces provisional text, `Changed` is emitted with the current composition range. The application echoes the complete value, including selection and composition, on the next composition.

During reconciliation:

- An incoming value equal to the last emitted value acknowledges the working value and preserves the session.
- An incoming value different from the last emitted value is authoritative.
- An authoritative replacement clears the old composition baseline, replaces the working value, and requests native synchronization.
- An authoritative text replacement clears local undo and redo history.
- An authoritative selection-only replacement preserves history but ends the current merge group.
- An authoritative replacement during active composition restarts the native input connection.
- TextField never converts an authoritative replacement into inferred insert or delete commands.

If application code does not preserve the emitted value, a later reconciliation can restore the declarative value. This is consistent with other controlled controls and avoids an undocumented internal source of truth.

## TextField reducer

The built-in reducer is independent of Runtime and platform code. It accepts a validated value, an optional composition baseline, and an ordered command batch.

Composition cancellation requires retained state that is intentionally absent from `TextEditingValue`:

```text
original replacement range
original replaced text
original selection
```

`BeginComposition` captures this baseline once. Composition updates retain it. `CommitText` and `FinishComposition` discard it. `CancelComposition` restores it.

Hardware keyboard movement and deletion use Unicode grapheme clusters. Native `DeleteSurrounding` follows the explicit unit in the command. These paths must not be implemented with UTF-8 byte movement or average character width assumptions.

The reducer does not own undo history. TextField keeps bounded undo and redo stacks beside its retained working value without changing the platform command protocol. Each entry stores the complete value before and after one edit so selection is restored with text.

Consecutive single-code-point insertion, backward deletion, and forward deletion merge while their selection continuity, edit direction, and time window remain compatible. Navigation, focus changes, atomic edits, undo, and redo end the current merge group. Paste, cut, word deletion, and newline insertion remain individual history entries.

Composition updates do not create intermediate entries. TextField captures one value before composition starts and commits one entry when composition finishes. Undo during active composition cancels it back to that baseline before consuming committed history.

## Text layout and geometry

The existing whole-string text measurement is insufficient for an editable control. Correct selection, caret placement, pointer hit testing, and native candidate windows require:

- Point-to-text hit testing.
- Text offset-to-caret geometry.
- Range-to-selection rectangles.
- Line metrics.
- Affinity at bidirectional boundaries.
- Glyph cluster and grapheme boundaries.

A minimal text layout capability uses the public `TextPosition` value:

```cpp
class TextLayout {
public:
  virtual ~TextLayout() = default;

  [[nodiscard]] virtual Size Measure() const = 0;
  [[nodiscard]] virtual TextPosition HitTest(Point point) const = 0;
  [[nodiscard]] virtual Rect CaretRect(
      TextOffset offset,
      TextAffinity affinity
  ) const = 0;
  [[nodiscard]] virtual std::vector<Rect> RangeRects(
      TextRange range
  ) const = 0;
};
```

Secure TextField creates grapheme boundaries from the platform text layout, then lays out only its mask string. A small mapped layout translates between real UTF-16 offsets and visual bullet offsets for hit testing, caret geometry, selection rectangles, and deletion. The PaintSequence never receives the real text.

Editable `detail::TextLayout` remains internal to HuxerUI. Public Canvas and TextMeasurer expose immutable paragraph and run metrics plus paint commands, while hit testing, caret geometry, and range geometry remain owned by TextField and native editable layouts.

The TextField caches layout by text, font, available width, multiline configuration, and relevant style values.

Editor components can use TextMeasurer for exact-run metrics while retaining their own line, selection, and document models. They do not reuse TextField's internal editable layout.

Candidate geometry is reported in node-local logical coordinates. Runtime applies layout and presentation transforms to obtain host-view coordinates. The platform adapter converts those coordinates to the native coordinate space required by its input API.

`TextInputClient::QueryTextInputPosition` receives a point in the owning node's local logical coordinates.
`Runtime::QueryTextInputPosition` accepts host-view logical coordinates and applies the inverse node transform before calling the client.

An active session retains the last platform-published host-view geometry and, when needed, one prepared snapshot that has not yet been published.
Both snapshots carry the client synchronization revision, node layout revision, and node-to-host transform that produced them.
Runtime queries geometry again only when none of those keys match, and notifies the platform only when state or the resulting geometry changed.
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

A single-line field registers its editor rectangle as a horizontal scroll viewport, maintains a retained offset through the shared scroll state, and keeps the active caret visible. Fixed decorations such as leading and trailing icons remain outside that viewport. The field does not create an internal ScrollView node.

A multiline field uses the same retained scroll state on the vertical axis when its content is taller than its viewport. Pointer hit testing, selection, composition, candidate geometry, and caret painting all resolve through the same translated text origin. Up and Down preserve a preferred horizontal caret position, while Home and End move to visual line boundaries.

The field participates in the same retained scroll chain as other scrollable nodes. Wheel and touch movement scroll the field first and pass unconsumed movement to an enclosing scroll container. Mouse and pen dragging retain text selection semantics, and dragging a selection beyond the viewport advances the internal text offset. Manual scrolling temporarily suppresses automatic caret reveal until editing or navigation resumes.

The caret blink timer and label transition are retained by the TextField extension. They request frames through the existing mounted extension scheduling path and respect resolved reduced-motion durations. Pointer or keyboard edits reset the visible caret phase. The resolved `caret_width` controls only painted caret thickness; text hit testing, scrolling, and native input geometry continue to use the logical caret boundary from TextLayout.

The first pointer behavior includes:

- Focus mouse and pen input on press.
- Focus touch input and place its caret only after a completed tap.
- Drag to extend selection.
- Preserve pointer cancellation behavior when a parent scroll gesture wins.

Mouse or pen double-click selects a word immediately. Touch double-tap selects on the second release so a drag beginning with the second press can still yield to scrolling. The runtime also owns long-press word selection and paints the shared selection menu and handles in a framework-owned `FrameworkOverlay` above the shared LayerStack. The overlay state owns its stable RenderNode and is appended to the synthetic RuntimeRoot scene without becoming a mounted application or LayerStack node. It is not a public Layer entry and does not participate in application-layer ordering or focus containment, but Runtime Back routing hides it before consulting public layers. Magnifiers and more advanced gesture behavior remain incremental.

## Theme

TextField uses a semantic style key:

```cpp
struct TextFieldVariantStyle {
  Color background;
  std::optional<Color> disabled_background;
  Color border;
  Color hovered_border;
  Color focused_border;
  Color disabled_border;
  float minimum_height = 0.0F;
};

struct TextFieldStyle {
  TextFieldVariant variant;
  TextFieldVariantStyle standard;
  TextFieldVariantStyle filled;
  TextFieldVariantStyle outlined;
  TextStyle text_style;
  TextStyle label_style;
  TextStyle floating_label_style;
  TextStyle placeholder_style;
  Color disabled_text;
  Color focused_label;
  Color disabled_label;
  Color error_label;
  Color disabled_placeholder;
  Color disabled_supporting_text;
  Color leading_icon;
  Color focused_leading_icon;
  Color disabled_leading_icon;
  Color error_leading_icon;
  Color trailing_icon;
  Color focused_trailing_icon;
  Color disabled_trailing_icon;
  Color error_trailing_icon;
  Color selection;
  Color caret;
  Color error_caret;
  Color composition;
  float caret_width = 1.0F;
  float border_width = 1.0F;
  float focused_border_width = 2.0F;
  float corner_radius = 0.0F;
  EdgeInsets padding;
  float leading_icon_size = 0.0F;
  float trailing_icon_size = 0.0F;
  float icon_spacing = 0.0F;
  float label_cutout_padding = 0.0F;
  double label_animation_duration = 0.0;
  double caret_blink_interval = 0.5;
  Color validation_error;
  float validation_border_width = 1.0F;
  float focused_validation_border_width = 2.0F;
  TextStyle validation_text_style;
  float validation_spacing = 4.0F;

  static TextFieldStyle Default();
};
```

Flat and Material Theme definitions provide their own TextField styles. Material defaults to a 56-unit Filled field and provides 56-unit Standard and Outlined alternatives. Flat defaults to a 36-unit Standard field, retains a 36-unit Outlined alternative, and provides a 44-unit compact Filled alternative. Material paints a 2-unit caret and uses 24-unit leading and trailing icons, while Flat retains a 1-unit caret and 18-unit icons.

Standard uses a transparent container and bottom state indicator. Filled adds a top-rounded container fill to the same indicator geometry. Outlined keeps the same radius on all four corners and uses a segmented top outline so a floating label does not depend on painting an opaque background over the border. Material Filled and Standard indicators use `on_surface_variant`, while Outlined uses `outline`; hover, focus, and disabled colors also resolve from the selected variant bundle. Container backgrounds are painted only within the editor frame and never extend through supporting text.

Each `TextFieldVariantStyle` owns one variant's background, optional disabled background, stateful border colors, and minimum height. An absent disabled background falls back to the ordinary background. `TextFieldStyle::variant` selects the Theme default, while an explicit component variant selects the corresponding bundle without duplicating active aliases. TextField draws its hover, focus, validation, and disabled states from these style values, so the common node indication and focus ring do not surround supporting text. Hover changes only the variant's indicator or outline, and disabled colors are resolved per element instead of reducing the opacity of the complete field subtree.

`TextFieldStyle` belongs in `theme.h` with the existing built-in component styles. Its type is also its Theme override identity.

Text input configuration, selection behavior, an explicit variant, label and placeholder content, and icon assets are not Theme values.

The selection overlay resolves handle colors from the focused control: `TextFieldStyle::caret` for editable text and the current Theme primary color for `SelectionArea`. Its horizontal toolbar reuses the active `MenuStyle` surface, foreground, shape, shadow, item geometry, separators, and indication without adopting the public Menu service lifecycle. Cut, Copy, Paste, and Select All resolve from the built-in `huxerui` resource domain and use the inherited Locale for shaping. An explicitly provided `TextSelectionMenuLabels` Environment value overrides each non-empty field for that subtree, while empty fields continue to resolve their localized framework defaults. Editing actions execute on release; Cut, Copy, and Paste make the menu non-interactive until the indication exit animation finishes, while Select All retains the overlay, exposes range handles, and recomputes the remaining actions. The public Menu service keeps separate LayerStack lifecycle, anchoring, focus, and dismissal.

A collapsed TextField selection uses a caret-anchored menu without selection handles. This allows an empty field to expose Paste when the clipboard contains text. A range selection uses the same menu together with themed start and end handles.

## Keyboard path

Text-producing input and control keys remain separate:

- Native committed and composing text enters through input commands.
- `KeyEvent` handles navigation, shortcuts, deletion, submission, and focus traversal.
- The focused TextInputClient receives text-related keys before generic activation behavior.
- An unhandled key continues through the existing NodeExtension and typed View event path.
- A platform adapter suppresses native character events that duplicate an IME commit.

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
| Escape | Cancel composition, otherwise follow Runtime focus behavior |
| Enter and Space activation | Not applied to TextField |

Shift extends the selection for movement commands. macOS `NSTextInputClient` selectors retain their word, line, page, and document semantics when converted to common key events.

Clipboard uses the optional `PlatformClipboard` capability. Runtime maps Ctrl/Command+A, C, V, and X to typed `TextEditingAction` values. TextField supports Select All, Copy, Paste, and Cut subject to its read-only and secure configuration; SelectionArea supports Select All and Copy. Undo and redo stay inside TextField rather than expanding `TextEditingAction` or the common input protocol. Secure fields allow Select All and Paste but reject Copy and Cut. Complex text clients retain ownership of their own history.

## Android adapter

`HuxerUIView` remains the native input target. `HuxerUIInputConnection` implements the input connection for the currently focused HuxerUI text input client.

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
- Revalidates the active session before honoring `RequestShow()` and asking `InputMethodManager` to display a keyboard hidden without focus loss.
- Resizes the logical viewport for visible IME insets and asks ancestor scroll containers to reveal the active caret.
- Forwards raw touch input while Runtime owns long-press recognition, editing actions, and the HuxerUI-drawn selection overlay.
- Uses password input type without suggestions and withholds surrounding, selected, and extracted text from secure input connections.

The adapter can retain a bounded surrounding-text mirror for Android query behavior. It does not own an authoritative copy of a complete SweetEditor document.

`restartInput()` is used when client ownership or input configuration changes, or when an authoritative state replacement invalidates the current native composition. Normal recomposition and selection changes use ordinary state updates.

## Apple adapter

An AppKit-specific client owned by `MacTextInput` conforms to `NSTextInputClient` on macOS. The host view remains the first responder and exposes the client's explicit `NSTextInputContext`.

On iOS, the HuxerUI View conforms to `UITextInput` through a dedicated `UIKitTextInput` bridge. Native UTF-16 positions, selection, marked text, keyboard traits, action keys, and geometry queries translate directly to the same Runtime session and command protocol. The UIKit bridge keeps only session and native-service bookkeeping; the controlled `TextEditingValue` remains authoritative in the shared TextField client.

The macOS adapter maps:

- `insertText` to `CommitText`.
- `setMarkedText` to begin or update composition.
- `unmarkText` to `FinishComposition`.
- `selectedRange` and `markedRange` to Runtime state queries.
- `attributedSubstringForProposedRange` to bounded context queries.
- `firstRectForCharacterRange` to text geometry.
- `characterIndexForPoint` to client hit testing.
- Returns no attributed substring for secure input.
- Enables Secure Event Input only while a secure field is active and the application is active.

The host view remains first responder while one HuxerUI editable node transfers focus to another. Runtime still creates a new logical input session so delayed callbacks from the previous client are rejected.

## Windows adapter

The first Windows implementation uses IMM32:

- `WM_IME_STARTCOMPOSITION` begins composition.
- `WM_IME_COMPOSITION` publishes composing updates and committed results.
- `WM_IME_ENDCOMPOSITION` finishes composition when required.
- Candidate and composition window placement uses current caret geometry.
- `WM_IME_CHAR` and `WM_CHAR` paths are filtered to prevent duplicate commits.
- Selection, deletion, and control keys continue through the key path where IMM32 does not provide a direct operation.

TSF can replace or augment the adapter later without changing Runtime, TextInputClient, TextField, or SweetEditor integration.

## PlatformView focus

An embedded native control and a HuxerUI TextInputClient cannot own the same host input session.

When input enters a native view:

- Runtime ends the active HuxerUI client session.
- PlatformAdapter stops the HuxerUI input connection.
- The native control receives native focus and owns its IME directly.

When focus returns to a HuxerUI editable node, Runtime creates a new session.

Focus transfer resolves the PlatformView identity from the current committed `RenderComposition`; a delayed native focus notification for an obsolete identity is ignored.
The Runtime remains authoritative for HuxerUI hit-test and focus ordering, while PlatformAdapter remains authoritative for native focus transfer and native input dispatch.
These focus and IME lifecycle messages are internal adapter coordination rather than PlatformPayload module events.
A module may emit a typed application event describing a native focus change, but that event neither starts nor ends a HuxerUI text-input session.
The macOS adapter implements this transfer through AppKit first-responder synchronization. AppKit retains key-view traversal within one native subtree, while traversal across its boundary returns to Runtime focus order. The remaining PlatformView hosts add equivalent native focus bridges with their hosting phases.
This follows the PlatformView ownership model in [`sdk-cli.md`](sdk-cli.md).

## SweetEditor integration

SweetEditor is a complex text input client, not a specialized TextField.

Its HuxerUI component bridge implements `TextInputClient` and maps:

| HuxerUI operation | SweetEditor operation |
| --- | --- |
| Begin client session | Begin EditorCore IME session |
| Apply command batch | Apply ordered `ImeCommandBatch` |
| Query state | Return EditorCore selection and composition |
| Query context | Return bounded EditorCore text context |
| Query geometry | Use SweetEditor layout geometry |
| End client session | End EditorCore IME session with finish semantics |

The bridge can map the HuxerUI session to an EditorCore session internally. HuxerUI session IDs remain the authority for Runtime callback routing.

SweetEditor keeps:

- Document text.
- Multi-selection policy.
- Editor transactions and history.
- Composition snapshot and recovery.
- Linked editing.
- Large-document context behavior.
- Editor-specific command validation.

The bridge does not build a complete `TextEditingValue`, copy the document into Runtime, or use the built-in TextField reducer.

SweetEditor's command path is the reference for ordered atomic native input, session mismatch handling, bounded context queries, and finish or cancel semantics. Editor-specific buffer protocols and recovery policies do not become mandatory HuxerUI APIs.

## Threading and reentrancy

Text input mutation occurs on the Runtime owning thread.

The platform adapter must marshal native callbacks to that thread before calling Runtime. Runtime validates session identity again after marshaling.

Applying one batch must not synchronously recompose the application in the middle of the reducer. The client publishes its resulting event and requests a frame after the atomic mutation completes.

Native update, restart, and close actions are executed after the client returns from mutation. A platform callback must not recursively reopen the same input connection while a command batch is being applied.

Destroying a host view invalidates every active session before destroying clients or platform input objects.

## Validation

Protocol validation includes:

- Current session identity.
- Valid command kind and required fields.
- Non-negative deletion lengths.
- Ordered ranges.
- Ranges inside the declared coordinate space.
- UTF-16 boundaries that do not split surrogate pairs.
- A composition-relative command only when composition exists.
- Resulting selection and composition inside the staged text.
- No mutation for a read-only client.

Validation failure returns a structured result and leaves client state unchanged. Platform adapters must not clamp arbitrary invalid commands into apparently valid mutations. Application-provided TextField values can use documented normalization helpers at the declarative boundary.

## Testing

The pure text input test suite covers:

- Empty values and collapsed selections.
- Forward and backward selections.
- UTF-8 and UTF-16 offset conversion.
- Emoji and surrogate-pair boundaries.
- Combining marks and grapheme deletion.
- Begin, update, commit, finish, and cancel composition.
- Collapsed composition distinct from no composition.
- Selection changes during composition.
- Surrounding deletion in both supported units.
- Ordered commands within one batch.
- Atomic rejection without partial mutation.
- Read-only rejection.
- Controlled value acknowledgement.
- Authoritative controlled replacement.
- Authoritative replacement during composition.

Runtime tests use a fake `PlatformTextInput` and cover:

- Focus begins one session.
- Recomposition preserves the current session.
- Focus transfer closes the old session and starts a new session.
- Restored modal focus receives a new session.
- Stale commands and context queries are rejected.
- Unmount, disable, and read-only transitions stop input.
- PlatformView focus closes the HuxerUI session.
- Pointer caret placement occurs before native state synchronization.
- External value changes request update or restart as appropriate.
- Key events do not duplicate committed text.
- Candidate geometry includes node and presentation transforms.
- Stable frames reuse the active-session geometry snapshot.
- Internal editor scrolling updates candidate geometry without forcing layout.
- Text position queries reject stale sessions and preserve UTF-16 affinity.

TextField tests cover:

- Caret painting and blink scheduling.
- Placeholder visibility.
- Selection painting.
- Composition underline painting.
- Horizontal caret visibility.
- Pointer hit testing and drag selection.
- Pointer cancellation when scroll gesture ownership changes.
- Theme style resolution.
- Disabled and focus interaction state.
- Secure grapheme masking, geometry, clipboard restrictions, and editing history.

Platform adapter tests and manual regression checks reuse the command-oriented portion of the SweetEditor IME regression matrix. Buffer-specific and linked-editing cases remain SweetEditor tests.

## Public header ownership

The new public headers are the common text input protocol and clipboard capability:

```text
include/huxerui/text_input.h
include/huxerui/clipboard.h
```

Existing public headers retain their current ownership:

```text
include/huxerui/view.h        TextField
include/huxerui/event.h       TextFieldEvents
include/huxerui/theme.h       TextFieldStyle
include/huxerui/huxerui.h     umbrella export
```

The design does not create one public header per range, selection, command, session type, or built-in control.

Implementation files can be:

```text
src/text_input.cpp
src/runtime_text_input.cpp
src/runtime_text_selection.cpp
src/text_field.cpp
src/selection_area.cpp
```

Platform adapters remain in their existing platform directories. Generic input behavior must not move into Android, Apple, or Windows helper libraries.

## Implemented delivery

The foundation contains:

- Common ranges, selections, editing values, and validation utilities.
- Input commands and atomic TextField reducer.
- Runtime session ownership and stale callback rejection.
- Fake platform input capability and unit tests.

The usable control contains:

- Single-line and multiline controlled TextField.
- Selection, caret, pointer placement, and drag selection.
- Composition painting and cancellation.
- Hardware navigation, deletion, and submission.
- Internal text geometry queries.
- Flat and Material TextField styles.
- Clipboard editing actions and native Android selection interaction.
- Automatic caret reveal when the Android IME reduces the viewport.
- Nested wheel and touch scrolling for fixed-height multiline fields.
- Bounded TextField-local undo and redo with composition grouping.
- Static selection through `SelectionArea`.

Android, iOS, macOS, and Windows now provide end-to-end native IME adapters.

The extension milestone validates one non-TextField client through a SweetEditor bridge or equivalent fake document client.

Accessibility semantics, richer iOS selection integration, OHOS, and TSF are incremental features built on the same protocol.

## Final design constraints

The implementation should preserve these constraints:

- `TextEditingValue` is the declarative value of TextField, not the universal platform mutation protocol.
- Native text mutation uses typed ordered command batches.
- One native callback produces one atomic client mutation.
- Runtime owns focus and logical input session identity.
- PlatformAdapter owns native input connections and coordinate conversion.
- Editable clients own text, selection, composition, and editing semantics.
- Runtime and PlatformAdapter do not mirror complete editor documents.
- Session IDs isolate delayed callbacks from previous clients.
- UTF-8 text and UTF-16 offsets use one validated conversion policy.
- TextField retains transient editing and animation state in a mounted extension.
- Recomposition of the same TextField does not restart native input.
- Authoritative controlled updates are not converted into inferred edits.
- Text geometry is based on real layout data, not average character width.
- TextField and SweetEditor share the input protocol without sharing their state models.
- PlatformView focus transfers IME ownership instead of creating two active clients.
- Text input protocol types remain concentrated in `text_input.h`; TextField, its events, and its style follow the ownership of existing built-in controls.
- TextField supports reliable single-line and multiline editing without becoming a document-editor abstraction.
