# HuxerUI Text Input and TextField Design

Status: implemented foundation with Windows and Android platform adapters

This document defines the target text editing model, input session lifecycle,
platform IME boundary, and built-in `TextField` behavior for HuxerUI. The
design builds on the existing controlled control model, `Runtime` focus
ownership, `PlatformHost`, retained `NodeExtension` state, typed events, and
DisplayList rendering.

The design also defines the extension boundary required by complex editable
components. SweetEditor is the reference integration: it should reuse the
HuxerUI focus and platform input path without replacing its document model
with the built-in TextField state.

This document records the implemented architecture and the remaining extension
direction.

## Goals

- Provide a controlled built-in TextField with selection and composition state.
- Support native IME composition without inferring edits from full text
  snapshots.
- Keep focus and input session ownership in `Runtime`.
- Keep native input connection behavior in `PlatformHost`.
- Let editable components own their text, selection, composition, and editing
  semantics.
- Isolate delayed callbacks from old native input sessions.
- Use one platform input path for TextField, SweetEditor, and future custom
  editable components.
- Preserve UTF-8 strings in the public C++ API while using one explicit offset
  convention across platforms.
- Keep the first implementation small enough to validate through a reliable
  single-line TextField.

The initial design deliberately does not include:

- A second application runtime for text input.
- A full-document mirror owned by `Runtime` or `PlatformHost`.
- Mutation inference by comparing two complete text snapshots.
- A Flutter-style delta buffer and revision protocol.
- Editor history, transactions, or linked-editing behavior in the common
  input protocol.
- Platform-specific IME types in common public headers.
- A temporary TextField that accepts characters but cannot represent native
  composition.

## Architecture

The text input path has two related but separate models:

```text
TextEditingValue
    declarative value of the built-in TextField

TextInputCommandBatch
    ordered editing intent delivered by a native input system
```

`TextEditingValue` is not the platform mutation protocol. A normal TextField
reduces input commands into a new `TextEditingValue`. A complex component can
apply the same commands to its own document core without creating a full value
snapshot.

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

`Runtime` does not own text content. `PlatformHost` does not decide how an edit
changes a value. An editable component does not call native IME APIs directly.

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
- Session identity and platform synchronization revisions are not part of
  `TextEditingValue`.
- Invalid offsets, reversed ranges, and offsets inside a UTF-16 surrogate pair
  are rejected at protocol boundaries.

UTF-16 offsets are an explicit interoperability choice. Android, Apple text
input APIs, Windows input APIs, and SweetEditor already operate in this
coordinate system. Keeping one offset convention prevents every adapter from
inventing a different conversion policy.

Application code should not normally manipulate UTF-16 offsets directly.
TextField and reusable text utilities provide validated range and movement
operations.

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

`coordinate_space` applies to `target`. `selection_after` is always expressed
in absolute UTF-16 offsets in the resulting text. A platform adapter converts
native relative cursor placement before constructing the command.

The protocol has these semantics:

- One native input callback produces one ordered batch.
- A batch is validated before any visible mutation occurs.
- Commands execute in order against a staged state.
- A later command sees the state produced by earlier commands in the batch.
- A rejected command rejects the complete batch.
- The client publishes at most one resulting state change for a successful
  batch.
- The Runtime rejects a batch whose session does not match the active client.
- The platform adapter never reconstructs a command by diffing two full text
  snapshots.

`SetSelection` changes selection without changing text.

`BeginComposition` records the replacement baseline and establishes a
composition range.

`UpdateComposition` replaces the active composition or its explicit target,
updates the composition range, and applies the requested selection.

`CommitText` replaces the active composition or selection and clears the
composition.

`FinishComposition` keeps the provisional text and clears the composition
marker.

`CancelComposition` restores the client-owned composition baseline.

`DeleteSurrounding` removes text around the current selection using the
explicit unit supplied by the native platform.

The protocol does not include undo grouping, editor transactions, document
revision IDs, linked editing, or full text buffers. Those are client concerns.

## Text input client

`TextInputClient` is the common capability implemented by the built-in
TextField and custom editable components:

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

  virtual TextInputKeyResult HandleTextKey(const KeyEvent& event) = 0;

  virtual void EndTextInput(
      TextInputSessionId session_id,
      TextInputEndReason reason
  ) = 0;
};
```

The result and state types are public because `TextInputClient` is a public
extension point. Their required semantics are:

- A current selection and optional composition.
- A bounded text context query.
- Accepted, rejected, read-only, and session-mismatch outcomes.
- A request to update or restart the native input connection.
- Caret, range, and point hit-test geometry.

A `NodeExtension` exposes the capability through one optional hook:

```cpp
virtual std::shared_ptr<TextInputClient> GetTextInputClient() noexcept {
  return {};
}
```

A focusable node can expose at most one client. Multiple text input clients on
the same node are rejected instead of receiving an implicit modifier priority.

The extension returns a stable shared client. Runtime retains that client only
for the active input session, allowing it to call `EndTextInput()` after the
owning extension is reconciled away without retaining a raw extension pointer.

The built-in TextField installs one retained extension. A future SweetEditor
component installs its own retained extension and returns its bridge from the
same hook.

## Input configuration

The client describes native keyboard and submission behavior with typed
configuration:

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

Platform-specific behavior is not represented by untyped string properties.
New common values can be added when at least one component and platform
integration need them.

Secure entry can use the same state and command protocol, but its context query
and platform exposure rules require a separate security review before it is
enabled.

## Runtime session ownership

`Runtime` owns one active HuxerUI text input session per host view:

```text
focused node identity
focused TextInputClient
monotonic TextInputSessionId
last synchronized client revision
native input connection state
```

Session IDs are non-zero, monotonically increasing, and not reused during the
Runtime lifetime.

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

An asynchronous platform callback captures the session ID at entry. If focus
or ownership changes before the callback reaches Runtime, it is rejected
without accessing the former client.

Normal focus loss finishes composition by retaining provisional text and
clearing the composition marker. Escape first cancels an active composition.
When there is no active composition, Escape follows the existing Runtime focus
behavior.

The current pointer path focuses a node before its pointer extension receives
the event. Starting native input immediately would expose the old caret
position. Runtime therefore defers native start or update until the current
input dispatch completes. The TextField places its caret first, and the
platform sees the resulting selection and geometry.

Modal focus capture and restoration use the same lifecycle. Restoring focus to
an editable node creates a new native input session rather than reviving an old
session ID.

## Platform input capability

Text input is a cohesive optional capability of `PlatformHost`. It should not
expand `PlatformHost` into a collection of unrelated per-command methods:

```cpp
class PlatformTextInput {
public:
  virtual ~PlatformTextInput() = default;

  virtual void Start(
      TextInputSessionId session_id,
      const TextInputConfiguration& configuration,
      const TextInputState& state
  ) = 0;

  virtual void Update(
      TextInputSessionId session_id,
      const TextInputState& state,
      const TextInputGeometry& geometry
  ) = 0;

  virtual void Restart(
      TextInputSessionId session_id,
      const TextInputConfiguration& configuration,
      const TextInputState& state
  ) = 0;

  virtual void Stop(TextInputSessionId session_id) = 0;
};
```

`PlatformHost` provides a nullable capability:

```cpp
virtual PlatformTextInput* TextInput() noexcept {
  return nullptr;
}
```

A test host, headless host, or incomplete platform does not need an empty input
implementation. Hardware navigation can continue to use ordinary key events
when no platform text input capability exists.

The platform adapter calls Runtime through session-aware entry points:

```text
HandleTextInputCommands
QueryTextInputContext
QueryTextInputGeometry
```

It does not retain a raw TextField or `TextInputClient` pointer across native
callbacks.

## State synchronization

The native input connection needs selection, composition, and candidate
geometry. It does not need an authoritative copy of the complete client text.

`TextInputState` contains:

```text
active session ID
selection
optional composition
client synchronization revision
```

The client revision is a Runtime synchronization detail. It is not a mutation
precondition and does not appear in `TextEditingValue`.

The platform can request a bounded `TextInputContext`:

```text
slice start in UTF-16 units
total text length in UTF-16 units
UTF-8 slice text
selection
optional composition
```

Large document clients return only the requested surrounding context.
TextField can return the complete short value when appropriate. Platform
adapters must tolerate partial context and request another range when needed.

Client state changes outside an IME callback, including an authoritative
controlled TextField update, increment the client revision. Runtime
synchronizes the new state after reconciliation. A configuration or ownership
change requests a native restart; an ordinary selection change requests only
an update.

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
      .Placeholder("Name")
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

`TextFieldEvents` belongs in `event.h`, following the existing
`ToggleEvents` convention.

`OnChanged()` and `OnSubmitted()` are convenience wrappers over the matching
typed events.

Text semantics such as placeholder, multiline behavior, keyboard type, and
submission action are component configuration. Visual properties remain Theme
styles or modifiers rather than growing one-off TextField styling methods.

The public API should expose one value model. It should not provide a second
string-only TextField with different change events. Simple applications use
`TextEditingValue::FromText()`.

## Static text selection

Static text selection is deliberately separate from editable text input.
`Text` remains non-selectable by default, while `SelectionArea` enables
selection across descendant `Text` nodes:

```cpp
SelectionArea {
  Column {
    Text("Heading", TextRole::Title),
    Text("Selectable body text."),
  },
};
```

`SelectionArea` owns retained selection state and text layout geometry, but it
does not expose a `TextInputClient` and never starts an IME session. Runtime
routes Copy and Select All to the focused selection area through the same
`TextEditingAction` entry points used by editable clients. Cut and Paste remain
unavailable for static content.

Desktop hosts use pointer drag selection and the standard Ctrl or Command
shortcuts. Touch input uses runtime-owned long-press word selection and a
shared HuxerUI selection overlay with draggable handles. A newline separates
adjacent descendant `Text` nodes in copied plain text.

## Controlled value behavior

TextField follows the controlled control model while retaining a responsive
mounted working value.

The retained extension stores:

```text
latest declarative value
working TextEditingValue
last emitted value
composition cancellation baseline
text layout cache
horizontal scroll offset
caret blink state
pointer selection state
client synchronization revision
```

When a command batch succeeds:

- The reducer updates the working value immediately.
- TextField repaints from the working value.
- One `TextFieldEvents::Changed` event is emitted.
- Runtime synchronizes the resulting selection and composition to the host.

When composition produces provisional text, `Changed` is emitted with the
current composition range. The application echoes the complete value,
including selection and composition, on the next composition.

During reconciliation:

- An incoming value equal to the last emitted value acknowledges the working
  value and preserves the session.
- An incoming value different from the last emitted value is authoritative.
- An authoritative replacement clears the old composition baseline, replaces
  the working value, and requests native synchronization.
- An authoritative replacement during active composition restarts the native
  input connection.
- TextField never converts an authoritative replacement into inferred insert
  or delete commands.

If application code does not preserve the emitted value, a later
reconciliation can restore the declarative value. This is consistent with
other controlled controls and avoids an undocumented internal source of truth.

## TextField reducer

The built-in reducer is independent of Runtime and platform code. It accepts a
validated value, an optional composition baseline, and an ordered command
batch.

Composition cancellation requires retained state that is intentionally absent
from `TextEditingValue`:

```text
original replacement range
original replaced text
original selection
```

`BeginComposition` captures this baseline once. Composition updates retain it.
`CommitText` and `FinishComposition` discard it. `CancelComposition` restores
it.

Hardware keyboard movement and deletion use Unicode grapheme clusters.
Native `DeleteSurrounding` follows the explicit unit in the command. These
paths must not be implemented with UTF-8 byte movement or average character
width assumptions.

The first reducer does not own undo history. Undo and redo can be introduced
later as TextField-local history without changing the platform command
protocol.

## Text layout and geometry

The existing whole-string text measurement is insufficient for an editable
control. Correct selection, caret placement, pointer hit testing, and native
candidate windows require:

- Point-to-text hit testing.
- Text offset-to-caret geometry.
- Range-to-selection rectangles.
- Line metrics.
- Affinity at bidirectional boundaries.
- Glyph cluster and grapheme boundaries.

A minimal text layout capability is:

```cpp
struct TextHit {
  TextOffset offset = 0;
  TextAffinity affinity = TextAffinity::Downstream;
};

class TextLayout {
public:
  virtual ~TextLayout() = default;

  [[nodiscard]] virtual Size Measure() const = 0;
  [[nodiscard]] virtual TextHit HitTest(Point point) const = 0;
  [[nodiscard]] virtual Rect CaretRect(
      TextOffset offset,
      TextAffinity affinity
  ) const = 0;
  [[nodiscard]] virtual std::vector<Rect> RangeRects(
      TextRange range
  ) const = 0;
};
```

The first implementation can keep this capability internal to HuxerUI.
TextField needs it, but application code does not. A later public Canvas or
custom text control API can expose it after the lifetime and caching model is
proven.

The TextField caches layout by text, font, available width, multiline
configuration, and relevant style values.

SweetEditor supplies its own geometry from its existing layout engine. It does
not use the built-in TextField text layout.

Candidate geometry is reported in node-local logical coordinates. Runtime
applies layout and presentation transforms to obtain host-view coordinates.
The platform host converts those coordinates to native screen units.

Average character width must not be used for caret or hit-test behavior. It
fails for emoji, ligatures, CJK text, combining marks, and bidirectional text.

## TextField layout and painting

The first TextField is a single-line leaf node with an intrinsic height
provided by its style. `Frame` and normal layout modifiers can override its
size.

The content pipeline is:

```text
background and border
selection rectangles
text or placeholder
composition underline
caret
focus and interaction indication
```

The current DisplayList already provides the necessary primitives:

- `DrawRect` for selection, caret, and a thin composition underline.
- `DrawText` for text and placeholder.
- `DrawBorder` for the field border.
- `PushClip` and `PopClip` for content clipping.

The first implementation does not need a TextField-specific drawing command.

A single-line field maintains a retained horizontal scroll offset and keeps the
active caret visible. It does not create an internal ScrollView node.

The caret blink timer is retained by the TextField extension. It requests
frames through the existing mounted extension scheduling path and respects
reduced motion where appropriate. Pointer or keyboard edits reset the visible
caret phase.

The first pointer behavior includes:

- Focus on press.
- Place the caret using text layout hit testing.
- Drag to extend selection.
- Preserve pointer cancellation behavior when a parent scroll gesture wins.

Mouse or pen double-click and touch double-tap select a word. The runtime also
owns long-press word selection and paints the shared selection menu and handles
above the mounted tree. Magnifiers and more advanced gesture behavior remain
incremental.

## Theme

TextField uses a semantic style key:

```cpp
struct TextFieldStyle {
  Color background;
  Color foreground;
  Color placeholder;
  Color selection;
  Color caret;
  Color composition;
  Color border;
  Color focused_border;
  float border_width = 1.0F;
  float focused_border_width = 2.0F;
  float font_size = 14.0F;
  float corner_radius = 0.0F;
  EdgeInsets padding;
  float minimum_height = 0.0F;
  double caret_blink_interval = 0.5;
};

struct TextFieldStyleKey {
  using Value = TextFieldStyle;

  static Value Default();
};
```

Flat and Material Theme definitions provide their own TextField styles.
Hover, press, focus ring, and disabled opacity continue to use the common
interaction infrastructure where their semantics match other controls.

`TextFieldStyle` and `TextFieldStyleKey` belong in `theme.h` with the existing
built-in component styles.

Text input configuration, selection behavior, and placeholder content are not
Theme values.

The selection overlay resolves handle colors from the focused control:
`TextFieldStyle::caret` for editable text and the current Theme primary color
for `SelectionArea`. Menu surfaces, typography, shapes, and pressed states use
the current Theme. `TextSelectionMenuLabelsKey` provides environment-overridable
Cut, Copy, Paste, and Select All labels without coupling localization to Theme.
Material menu items use the shared ripple indication, while Flat menu items use
the shared hover and pressed state overlay. Editing actions execute on release;
the menu becomes non-interactive until the indication exit animation finishes.

A collapsed TextField selection uses a caret-anchored menu without selection
handles. This allows an empty field to expose Paste when the clipboard contains
text. A range selection uses the same menu together with themed start and end
handles.

## Keyboard path

Text-producing input and control keys remain separate:

- Native committed and composing text enters through input commands.
- `KeyEvent` handles navigation, shortcuts, deletion, submission, and focus
  traversal.
- The focused TextInputClient receives text-related keys before generic
  activation behavior.
- An unhandled key continues through the existing NodeExtension and typed View
  event path.
- A platform adapter suppresses native character events that duplicate an IME
  commit.

Default behavior is:

| Key | Single-line TextField |
| --- | --- |
| Left and Right | Move by grapheme cluster, or extend with Shift |
| Home and End | Move to the beginning or end |
| Backspace and Delete | Delete selection or adjacent grapheme |
| Tab and Shift+Tab | Move focus |
| Enter | Submit |
| Escape | Cancel composition, otherwise follow Runtime focus behavior |
| Enter and Space activation | Not applied to TextField |

Multiline TextField later changes Enter to newline insertion and defines
line-aware Up, Down, Home, and End behavior.

Clipboard uses the optional `PlatformClipboard` capability. Runtime maps
Ctrl/Command+A, C, V, and X to typed `TextEditingAction` values. TextField
supports Select All, Copy, Paste, and Cut subject to its read-only and secure
configuration; SelectionArea supports Select All and Copy. Undo and redo still
require a dedicated history capability.

## Android adapter

`HuxerUIView` remains the native input target. `HuxerUIInputConnection`
implements the input connection for the currently focused HuxerUI text input
client.

The Android adapter:

- Implements `onCheckIsTextEditor()`.
- Returns a custom `HuxerUIInputConnection` derived from
  `BaseInputConnection`.
- Maps `setComposingText()` to composition commands.
- Maps `commitText()` to `CommitText`.
- Maps `finishComposingText()` to `FinishComposition`.
- Maps `setSelection()` to `SetSelection`.
- Maps both surrounding deletion variants with their explicit units.
- Uses `updateSelection()` to synchronize selection and composition.
- Uses `CursorAnchorInfo` for candidate and insertion marker geometry.
- Rejects callbacks carrying a stale HuxerUI session.
- Resizes the logical viewport for visible IME insets and asks ancestor scroll
  containers to reveal the active caret.
- Forwards raw touch input while Runtime owns long-press recognition, editing
  actions, and the HuxerUI-drawn selection overlay.

The adapter can retain a bounded surrounding-text mirror for Android query
behavior. It does not own an authoritative copy of a complete SweetEditor
document.

`restartInput()` is used when client ownership or input configuration changes,
or when an authoritative state replacement invalidates the current native
composition. Normal recomposition and selection changes use ordinary state
updates.

## Apple adapter

The HuxerUI host view conforms to `NSTextInputClient` on macOS and the matching
UIKit text input protocols on iOS.

The macOS adapter maps:

- `insertText` to `CommitText`.
- `setMarkedText` to begin or update composition.
- `unmarkText` to `FinishComposition`.
- `selectedRange` and `markedRange` to Runtime state queries.
- `attributedSubstringForProposedRange` to bounded context queries.
- `firstRectForCharacterRange` to text geometry.
- `characterIndexForPoint` to client hit testing.

The host view remains first responder while one HuxerUI editable node transfers
focus to another. Runtime still creates a new logical input session so delayed
callbacks from the previous client are rejected.

## Windows adapter

The first Windows implementation uses IMM32:

- `WM_IME_STARTCOMPOSITION` begins composition.
- `WM_IME_COMPOSITION` publishes composing updates and committed results.
- `WM_IME_ENDCOMPOSITION` finishes composition when required.
- Candidate and composition window placement uses current caret geometry.
- `WM_IME_CHAR` and `WM_CHAR` paths are filtered to prevent duplicate commits.
- Selection, deletion, and control keys continue through the key path where
  IMM32 does not provide a direct operation.

TSF can replace or augment the adapter later without changing Runtime,
TextInputClient, TextField, or SweetEditor integration.

## NativeView focus

An embedded native control and a HuxerUI TextInputClient cannot own the same
host input session.

When input enters a native view:

- Runtime ends the active HuxerUI client session.
- PlatformHost stops the HuxerUI input connection.
- The native control receives native focus and owns its IME directly.

When focus returns to a HuxerUI editable node, Runtime creates a new session.

The Runtime remains authoritative for HuxerUI hit-test and focus ordering.
PlatformHost remains authoritative for native focus transfer and event
dispatch. This follows the NativeView ownership model in
`docs/sdk-cli-design.md`.

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

The bridge can map the HuxerUI session to an EditorCore session internally.
HuxerUI session IDs remain the authority for Runtime callback routing.

SweetEditor keeps:

- Document text.
- Multi-selection policy.
- Editor transactions and history.
- Composition snapshot and recovery.
- Linked editing.
- Large-document context behavior.
- Editor-specific command validation.

The bridge does not build a complete `TextEditingValue`, copy the document into
Runtime, or use the built-in TextField reducer.

SweetEditor's command path is the reference for ordered atomic native input,
session mismatch handling, bounded context queries, and finish or cancel
semantics. Editor-specific buffer protocols and recovery policies do not become
mandatory HuxerUI APIs.

## Threading and reentrancy

Text input mutation occurs on the Runtime owning thread.

The platform adapter must marshal native callbacks to that thread before
calling Runtime. Runtime validates session identity again after marshaling.

Applying one batch must not synchronously recompose the application in the
middle of the reducer. The client publishes its resulting event and requests a
frame after the atomic mutation completes.

Native update, restart, and close actions are executed after the client returns
from mutation. A platform callback must not recursively reopen the same input
connection while a command batch is being applied.

Destroying a host view invalidates every active session before destroying
clients or platform input objects.

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

Validation failure returns a structured result and leaves client state
unchanged. Platform adapters must not clamp arbitrary invalid commands into
apparently valid mutations. Application-provided TextField values can use
documented normalization helpers at the declarative boundary.

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
- NativeView focus closes the HuxerUI session.
- Pointer caret placement occurs before native state synchronization.
- External value changes request update or restart as appropriate.
- Key events do not duplicate committed text.
- Candidate geometry includes node and presentation transforms.

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

Platform adapter tests and manual regression checks reuse the command-oriented
portion of the SweetEditor IME regression matrix. Buffer-specific and
linked-editing cases remain SweetEditor tests.

## Public header ownership

The new public headers are the common text input protocol and clipboard
capability:

```text
include/huxerui/text_input.h
include/huxerui/clipboard.h
```

Existing public headers retain their current ownership:

```text
include/huxerui/view.h        TextField
include/huxerui/event.h       TextFieldEvents
include/huxerui/theme.h       TextFieldStyle and TextFieldStyleKey
include/huxerui/huxerui.h     umbrella export
```

The design does not create one public header per range, selection, command,
session type, or built-in control.

Implementation files can be:

```text
src/text_input.cpp
src/runtime_text_input.cpp
src/runtime_text_selection.cpp
src/text_field.cpp
src/selection_area.cpp
```

Platform adapters remain in their existing platform directories. Generic
input behavior must not move into Android, Apple, or Windows helper libraries.

## Initial delivery

The foundation contains:

- Common ranges, selections, editing values, and validation utilities.
- Input commands and atomic TextField reducer.
- Runtime session ownership and stale callback rejection.
- Fake platform input capability and unit tests.

The usable control contains:

- Single-line controlled TextField.
- Selection, caret, pointer placement, and drag selection.
- Composition painting and cancellation.
- Hardware navigation, deletion, and submission.
- Internal text geometry queries.
- Flat and Material TextField styles.
- Clipboard editing actions and native Android selection interaction.
- Automatic caret reveal when the Android IME reduces the viewport.
- Static selection through `SelectionArea`.

Windows and Android now provide end-to-end native IME adapters. macOS native
text input remains the next platform adapter milestone.

The extension milestone validates one non-TextField client through a
SweetEditor bridge or equivalent fake document client.

Multiline editing, TextField-local history, secure input, accessibility
semantics, iOS, OHOS, and TSF are incremental features built on the same
protocol.

## Final design constraints

The implementation should preserve these constraints:

- `TextEditingValue` is the declarative value of TextField, not the universal
  platform mutation protocol.
- Native text mutation uses typed ordered command batches.
- One native callback produces one atomic client mutation.
- Runtime owns focus and logical input session identity.
- PlatformHost owns native input connections and coordinate conversion.
- Editable clients own text, selection, composition, and editing semantics.
- Runtime and PlatformHost do not mirror complete editor documents.
- Session IDs isolate delayed callbacks from previous clients.
- UTF-8 text and UTF-16 offsets use one validated conversion policy.
- TextField retains transient editing and animation state in a mounted
  extension.
- Recomposition of the same TextField does not restart native input.
- Authoritative controlled updates are not converted into inferred edits.
- Text geometry is based on real layout data, not average character width.
- TextField and SweetEditor share the input protocol without sharing their
  state models.
- NativeView focus transfers IME ownership instead of creating two active
  clients.
- Text input protocol types remain concentrated in `text_input.h`; TextField,
  its events, and its style follow the ownership of existing built-in controls.
- The initial implementation is a reliable single-line control rather than an
  incomplete broad text editor.
