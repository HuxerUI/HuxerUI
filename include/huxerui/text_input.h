#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <huxerui/clipboard.h>
#include <huxerui/color.h>
#include <huxerui/event.h>
#include <huxerui/geometry.h>
#include <huxerui/text.h>

namespace huxerui {

/// Identity of one Runtime-owned text-input session.
///
/// Active IDs are nonzero, increase monotonically, and are not reused during a Runtime's lifetime. Zero means no
/// active session. Platform callbacks must carry the captured ID so delayed input cannot reach a different client.
using TextInputSessionId = std::uint64_t;

/// A directed selection in UTF-16 units; equal endpoints represent a caret.
///
/// Both endpoints refer to the same current text and must be in bounds without splitting a surrogate pair.
/// Direction is preserved even though Range() returns ordered endpoints.
/// @code
/// const TextSelection backward{5, 2, TextAffinity::Upstream};
/// const TextRange selected = backward.Range(); // [2, 5), with the active caret at 2.
/// @endcode
struct TextSelection {
  /// Fixed endpoint while extending the selection.
  TextOffset anchor = 0;
  /// Moving endpoint, or the insertion position for a collapsed selection.
  TextOffset active = 0;
  /// Visual side of the active endpoint at an ambiguous caret boundary.
  TextAffinity affinity = TextAffinity::Downstream;

  /// @return Whether anchor and active are equal; does not validate either offset against text.
  [[nodiscard]] bool IsCollapsed() const noexcept {
    return anchor == active;
  }

  /// @return The ordered half-open range covering the selection, without direction or text validation.
  [[nodiscard]] TextRange Range() const noexcept {
    return anchor <= active ? TextRange{anchor, active} : TextRange{active, anchor};
  }

  bool operator==(const TextSelection&) const = default;
};

/// Complete application-controlled value of a TextField, including selection and provisional IME text.
///
/// Accept the complete changed value rather than reconstructing it from text alone, which would discard selection
/// and composition. Session IDs and synchronization revisions belong to the input client, not this value.
/// With `<huxerui/huxerui.h>` in a composable source file:
/// @code
/// [[huxerui::composable]]
/// View NameEditor() {
///   auto name = UseState(TextEditingValue::FromText(""));
///   return TextField(name).OnChanged([name](const TextEditingValue& next) mutable {
///     name = next;
///   });
/// }
/// @endcode
struct TextEditingValue {
  /// Valid UTF-8 body, including any provisional composition text.
  std::string text;
  /// Directed selection expressed in UTF-16 offsets in text.
  TextSelection selection;
  /// Active provisional range in text. No value means no composition; a collapsed range is still a composition.
  std::optional<TextRange> composition;

  /// Initializes a plain value with a collapsed downstream caret at the end and no composition.
  /// @param text Valid UTF-8 body.
  /// @return The complete value; the caret offset is the UTF-16 length, not the byte length.
  /// @throws std::invalid_argument If text is not valid UTF-8.
  static TextEditingValue FromText(std::string text);

  bool operator==(const TextEditingValue&) const = default;
};

/// Editing intent normalized from one platform input operation.
///
/// Fields not used by a command kind must remain at their defaults. Ranges and selections must be valid in the
/// state at that point in the batch; malformed commands reject the entire batch.
enum class TextInputCommandKind {
  /// Changes only selection using the required selection_after; target and text must be absent or empty.
  SetSelection,
  /// Starts composition over a required absolute target and records the original text and selection for cancel.
  /// Does not replace text or move selection; rejected when a composition is already active.
  BeginComposition,
  /// Replaces provisional text, starting composition if needed.
  /// Without an active composition, target is absolute or defaults to the selection. During composition, an explicit
  /// target must be composition-relative; omitting it replaces the entire composition. Without selection_after,
  /// the caret collapses downstream at the resulting composition's end.
  UpdateComposition,
  /// Inserts text and finishes composition. With an active composition, replaces it and forbids an explicit target.
  /// Otherwise replaces the absolute target or current selection. Without selection_after, the caret collapses
  /// downstream after the inserted text.
  CommitText,
  /// Keeps provisional text and clears composition plus its cancellation baseline; otherwise does nothing.
  FinishComposition,
  /// Restores the active composition's saved text and selection, then clears composition.
  /// Requires a client-owned cancellation baseline when composing; does nothing when no composition exists.
  CancelComposition,
  /// Deletes delete_before units before the ordered selection and delete_after units after it, retaining its text.
  /// Counts are clamped at document ends; split surrogate pairs or deletion across a composition boundary are rejected.
  DeleteSurrounding,
};

/// Origin of TextInputCommand::target only; selection_after always uses absolute resulting-text offsets.
enum class TextInputCoordinateSpace {
  /// Absolute UTF-16 offsets in the current complete text.
  Text,
  /// UTF-16 offsets relative to the active composition's start, confined to that composition.
  /// Used for an explicit partial UpdateComposition target.
  Composition,
};

/// Unit for DeleteSurrounding counts; neither choice means user-perceived grapheme clusters.
enum class TextInputUnit {
  /// UTF-16 code units; a deletion endpoint inside a surrogate pair is rejected, not rounded.
  Utf16CodeUnit,
  /// Unicode scalar values, with supplementary characters counted once.
  UnicodeCodePoint,
};

/// One command in an ordered, atomic TextInputCommandBatch.
///
/// Populate only fields supported by kind. The default value alone is not a valid SetSelection command because
/// selection_after is required. All target offsets use UTF-16 regardless of delete_unit.
struct TextInputCommand {
  /// Operation and the contract that determines which other fields are allowed.
  TextInputCommandKind kind = TextInputCommandKind::SetSelection;
  /// Coordinate origin of target, never of selection_after; leave Text when target is omitted.
  TextInputCoordinateSpace coordinate_space = TextInputCoordinateSpace::Text;
  /// Replacement or composition range, interpreted according to kind and coordinate_space.
  std::optional<TextRange> target;
  /// Absolute UTF-16 selection in the resulting text, required for SetSelection and optional for text replacements.
  /// Platform-specific relative cursor-placement conventions must be converted before constructing this command.
  std::optional<TextSelection> selection_after;
  /// Valid UTF-8 replacement text for UpdateComposition or CommitText; empty can remove the replacement range.
  std::string text;
  /// Nonnegative number of delete_unit units before selection.Range().start for DeleteSurrounding.
  TextOffset delete_before = 0;
  /// Nonnegative number of delete_unit units after selection.Range().end for DeleteSurrounding.
  TextOffset delete_after = 0;
  /// Unit of deletion counts only; leave the default for other command kinds.
  TextInputUnit delete_unit = TextInputUnit::Utf16CodeUnit;

  bool operator==(const TextInputCommand&) const = default;
};

/// Ordered editing commands from one platform callback, applied as one transaction.
///
/// Each command sees the preceding command's result. A rejected command leaves the entire batch unapplied;
/// a successful batch publishes at most one resulting value change. Empty batches are rejected.
/// A platform adapter routes the batch through Runtime, not directly to a retained client pointer.
/// With the active session_id and a non-composing value "Hello", this batch requests "Hio" with the caret at offset 2:
/// @code
/// TextInputCommand select;
/// select.selection_after = TextSelection{1, 4};
/// TextInputCommand replace;
/// replace.kind = TextInputCommandKind::CommitText;
/// replace.text = "i";
/// const TextInputCommandBatch batch{session_id, {select, replace}};
/// @endcode
struct TextInputCommandBatch {
  /// Active session captured by the platform callback; Runtime rejects stale IDs before calling the client.
  TextInputSessionId session_id = 0;
  /// Nonempty commands in platform operation order.
  std::vector<TextInputCommand> commands;

  bool operator==(const TextInputCommandBatch&) const = default;
};

/// Platform keyboard/input hints, not validation or character filters; exact layouts are platform-dependent.
enum class TextInputType {
  /// General text entry.
  Text,
  /// Email-address entry.
  Email,
  /// Integer-oriented numeric entry.
  Number,
  /// Numeric entry with a decimal separator.
  Decimal,
  /// Telephone-number entry.
  Phone,
  /// Web-address entry.
  Url,
};

/// Automatic-capitalization hints for supported platform keyboards; does not rewrite the controlled value.
enum class TextCapitalization {
  /// No automatic capitalization requested.
  None,
  /// Requests uppercase entry for all characters.
  Characters,
  /// Requests capitalization at word starts.
  Words,
  /// Requests capitalization at sentence starts.
  Sentences,
};

/// Intent shown by the platform action key and routed through the focused client's Enter-key handling.
///
/// These values do not perform application navigation, searches, or submissions by themselves.
enum class TextInputAction {
  /// Resolves to Done for single-line input and Newline for multiline input.
  Default,
  /// Requests completion of input.
  Done,
  /// Requests an application-defined go action.
  Go,
  /// Requests advancement to the next input.
  Next,
  /// Requests an application-defined search.
  Search,
  /// Requests an application-defined send action.
  Send,
  /// Requests a line break; built-in TextField requires multiline input.
  Newline,
};

/// Platform input configuration supplied by the client and synchronized by Runtime.
///
/// Keyboard hints do not replace application validation. Changing an active client's configuration can restart its
/// platform input connection without creating a new HuxerUI session.
struct TextInputConfiguration {
  /// Input-purpose hint, not a restriction on valid text.
  TextInputType type = TextInputType::Text;
  /// Automatic-capitalization hint where supported.
  TextCapitalization capitalization = TextCapitalization::None;
  /// Action-key intent handled through the client's key path.
  TextInputAction action = TextInputAction::Default;
  /// Whether the client supports multiple lines; affects keyboard configuration and the default action.
  bool multiline = false;
  /// Protected entry: adapters must suppress platform surrounding/extracted text and clipboard disclosure.
  /// Built-in TextField masks displayed text, disables Copy/Cut, and rejects secure multiline configuration.
  bool secure = false;
  /// Requests platform spelling correction or suggestions where supported and allowed by secure-input policy.
  bool autocorrect = true;
  /// Disallows editing and prevents Runtime from opening a text-input session; selection can remain available.
  bool read_only = false;

  bool operator==(const TextInputConfiguration&) const = default;
};

/// Outcome of a session-aware command or query; result payloads are meaningful only for Ok.
enum class TextInputResultCode {
  /// The operation succeeded; this does not imply that the state changed.
  Ok,
  /// The request belongs to a different or ended session.
  SessionMismatch,
  /// Invalid input, unavailable geometry, or another operation the client could not accept.
  Rejected,
  /// An editing request was refused because the client is read-only.
  ReadOnly,
};

/// Additional platform input synchronization requested after an accepted batch.
///
/// Runtime also detects revision, configuration, and geometry changes; None does not suppress that synchronization.
enum class TextInputSyncAction {
  /// No additional synchronization requested.
  None,
  /// Refreshes state and geometry on the existing platform input connection.
  Update,
  /// Reinitializes the platform input connection for the same HuxerUI session.
  Restart,
};

/// Why Runtime ends a client's text-input session; not an instruction to cancel provisional text.
enum class TextInputEndReason {
  /// Focus moved away from the owning node or host.
  FocusLost,
  /// The owning node or its input-client capability was removed or replaced.
  ClientRemoved,
  /// The owning node became disabled.
  Disabled,
  /// The client became read-only.
  ReadOnly,
  /// The owning Runtime is shutting down.
  RuntimeDestroyed,
};

/// Whether focused text-key handling consumed an event.
enum class TextInputKeyResult {
  /// Allows Runtime to continue normal key routing.
  Unhandled,
  /// Stops further handling of the event, even when no text changed.
  Handled,
};

/// Lightweight client snapshot for Runtime/platform synchronization, without an authoritative text mirror.
///
/// Revisions must not decrease within a session. They are synchronization counters, not command preconditions or
/// application document versions. For example, moving the caret advances revision only; replacing text advances
/// both counters, even if the replacement keeps the same selection and composition offsets.
struct TextInputState {
  /// Active client session, or zero when inactive.
  TextInputSessionId session_id = 0;
  /// Advances for observable text, selection, composition, or client-owned geometry changes.
  /// Internal editor scrolling counts; ancestor layout, scrolling, and transforms are tracked by Runtime.
  std::uint64_t revision = 0;
  /// Advances only when text content changes, together with revision; allows Runtime to invalidate text layout.
  std::uint64_t content_revision = 0;
  /// Current directed selection in absolute UTF-16 text offsets.
  TextSelection selection;
  /// Current provisional range in absolute UTF-16 text offsets, or no active composition.
  std::optional<TextRange> composition;

  bool operator==(const TextInputState&) const = default;
};

/// Result of applying a batch; Runtime reads TextInputClient::State() to determine whether anything changed.
struct TextInputApplyResult {
  /// Acceptance or failure of the complete batch.
  TextInputResultCode result_code = TextInputResultCode::Rejected;
  /// Additional platform input synchronization request; must be None unless result_code is Ok.
  TextInputSyncAction sync_action = TextInputSyncAction::None;

  bool operator==(const TextInputApplyResult&) const = default;
};

/// An owning UTF-8 context slice with absolute UTF-16 selection and composition metadata.
///
/// The slice need not contain the complete document or either selection endpoint. Platform adapters must honor
/// slice_start rather than treating the slice as the complete value. This is trusted internal input context, not a
/// sanitized platform surrounding-text response: secure-entry adapters must enforce the disclosure policy.
/// For text == "abc" and slice_start == 100, the slice covers [100, 103), while selection offsets stay absolute.
struct TextInputContext {
  /// Whether the context is available; only Ok makes the content fields meaningful.
  TextInputResultCode result_code = TextInputResultCode::Rejected;
  /// Session associated with the query, including on failure.
  TextInputSessionId session_id = 0;
  /// Absolute UTF-16 offset corresponding to the first byte of text.
  TextOffset slice_start = 0;
  /// UTF-16 length of the complete client text, not this slice.
  TextOffset total_length = 0;
  /// Valid UTF-8 slice aligned to Unicode scalar boundaries within the complete text.
  std::string text;
  /// Current selection in complete-text coordinates, not relative to slice_start.
  TextSelection selection;
  /// Current composition in complete-text coordinates, or no active composition.
  std::optional<TextRange> composition;

  bool operator==(const TextInputContext&) const = default;
};

/// Caret and range geometry in logical units, derived from the same layout used to draw text.
///
/// TextInputClient returns node-local rectangles; Runtime transforms them once to host-view coordinates before
/// returning them to a platform adapter. Successful rectangles must be finite with nonnegative sizes.
struct TextInputGeometry {
  /// Whether geometry is available; only Ok makes the rectangles meaningful.
  TextInputResultCode result_code = TextInputResultCode::Rejected;
  /// Session associated with the query, including on failure.
  TextInputSessionId session_id = 0;
  /// Caret rectangle at the queried range's end.
  Rect caret;
  /// Visual fragments of the queried range; wrapping or bidirectional layout may produce multiple rectangles.
  std::vector<Rect> range_rects;

  bool operator==(const TextInputGeometry&) const = default;
};

/// A hit-tested logical insertion position, not a byte offset or screen coordinate.
struct TextInputPositionResult {
  /// Whether hit testing produced a usable position.
  TextInputResultCode result_code = TextInputResultCode::Rejected;
  /// Session associated with the query, including on failure.
  TextInputSessionId session_id = 0;
  /// Absolute UTF-16 position and visual affinity; meaningful only when result_code is Ok.
  TextPosition position;

  bool operator==(const TextInputPositionResult&) const = default;
};

/// Editable component capability exposed by NodeExtension::GetTextInputClient().
///
/// The client owns text, selection, composition, and edit semantics. Runtime owns focus and session routing;
/// PlatformTextInput owns the platform input connection. Custom editors may keep their own document model instead of a
/// TextEditingValue. Ordinary applications use TextField and do not implement this interface.
///
/// Expose one stable shared client per focusable node. Runtime retains it for the active session and can call
/// EndTextInput() after its extension is detached; the client must not rely on a dangling node or extension pointer.
/// Methods run synchronously on the Runtime's host thread. Geometry and hit-test points are node-local logical units,
/// not window coordinates or physical pixels; Runtime performs the node/host transform.
/// @see TextSelectionClient
class TextInputClient {
public:
  virtual ~TextInputClient() = default;

  /// @return The current platform input configuration, including whether the client is read-only.
  [[nodiscard]] virtual TextInputConfiguration Configuration() const = 0;
  /// Returns a snapshot without changing the document or synchronization counters.
  /// @return Current session, revisions, selection, and composition, obeying TextInputState's revision contract.
  [[nodiscard]] virtual TextInputState State() const = 0;
  /// Associates this client with a newly allocated Runtime session before platform input starts.
  /// @param session_id Nonzero ID supplied by Runtime; do not generate or replace it in the client.
  /// @return Initial valid state carrying exactly session_id.
  virtual TextInputState BeginTextInput(TextInputSessionId session_id) = 0;
  /// Applies the entire batch atomically, publishing at most one resulting change.
  /// @param batch Nonempty ordered commands for the active session.
  /// @return Acceptance and optional platform input synchronization; rejected batches must leave state unchanged.
  /// Check session identity, read-only policy, command fields, UTF-8, and range boundaries before publishing.
  /// Successful changes must be observable through State() with the appropriate increased revisions.
  virtual TextInputApplyResult ApplyTextInput(const TextInputCommandBatch& batch) = 0;
  /// Supplies surrounding text without requiring a full-document copy from a large editor.
  /// @param session_id Session to query; stale requests return SessionMismatch.
  /// @param start Nonnegative absolute UTF-16 start of the requested context.
  /// @param length Nonnegative requested length in UTF-16 units.
  /// @return An owning slice and absolute metadata, or a failure code; negative requests are rejected.
  /// A short TextField may return its complete value. Large clients should return bounded context and report its
  /// actual slice_start; adapters must tolerate partial slices and request additional context when needed.
  [[nodiscard]] virtual TextInputContext
  QueryTextInputContext(TextInputSessionId session_id, TextOffset start, TextOffset length) const = 0;
  /// Queries caret and visual range fragments from the client's current text layout.
  /// @param session_id Session to query; stale requests return SessionMismatch.
  /// @param range Ordered, in-bounds absolute UTF-16 range without split surrogate pairs.
  /// @return Node-local logical rectangles, or Rejected when the range or required layout is unavailable.
  /// Return the caret at range.end; preserve the selection's affinity when that is the active endpoint.
  [[nodiscard]] virtual TextInputGeometry
  QueryTextInputGeometry(TextInputSessionId session_id, TextRange range) const = 0;
  /// Hit-tests a point using the same layout and local text origin used for drawing.
  /// @param session_id Session to query; stale requests return SessionMismatch.
  /// @param point Finite point in the owning node's local logical coordinates.
  /// @return A valid absolute UTF-16 insertion position with affinity, or a failure code.
  /// For text drawn at local origin (12, 8), hit-test (point.x - 12, point.y - 8) in paragraph coordinates.
  [[nodiscard]] virtual TextInputPositionResult
  QueryTextInputPosition(TextInputSessionId session_id, Point point) const = 0;
  /// Handles focused text keys before ordinary key routing, including navigation and normalized IME actions.
  /// @param event Key event; platform action keys enter this path as Enter.
  /// @return Handled to consume the event, or Unhandled to continue routing.
  /// Keep selection, composition, and synchronization revisions consistent with any resulting edit.
  virtual TextInputKeyResult HandleTextKey(const KeyEvent& event) = 0;
  /// Notifies the client that scrolling changed its own or an ancestor viewport geometry.
  /// The default does nothing. An editor may suppress automatic caret reveal after deliberate viewport scrolling.
  virtual void ViewportScrolled() {}
  /// Releases session bookkeeping after Runtime ends focus or input ownership.
  /// @param session_id Session being ended; ignore a stale ID rather than ending a newer session.
  /// @param reason Why the session ended, not a request to cancel composition.
  /// Finish active composition by keeping provisional text and clearing its marker; explicit cancellation is a
  /// separate command. This call must remain safe after the owning node or extension has been detached.
  virtual void EndTextInput(TextInputSessionId session_id, TextInputEndReason reason) = 0;
};

/// Visible selection geometry in the owning node's local logical coordinates.
/// An endpoint can be absent because its logical block is outside the viewport.
///
/// Missing geometry does not clear logical selection. Runtime positions handles and the toolbar from the available
/// rectangles; all three may be absent while a logical selection remains outside the viewport.
struct TextSelectionGeometry {
  /// Caret-like rectangle at the ordered selection start, independent of anchor/active direction, when available.
  std::optional<Rect> start{};
  /// Caret-like rectangle at the ordered selection end, independent of anchor/active direction, when available.
  std::optional<Rect> end{};
  /// A visible selected fragment used to anchor the toolbar even when both endpoints are offscreen.
  std::optional<Rect> toolbar_anchor{};

  bool operator==(const TextSelectionGeometry&) const = default;
};

/// Selection gestures and overlay geometry exposed by NodeExtension::GetTextSelectionClient().
///
/// This capability does not start an IME session or require editable content. A SelectionArea can expose Copy and
/// Select All across virtualized blocks; an editor may implement both selection and input capabilities.
/// The extension owns the client and exposes a valid pointer while attached. Calls are synchronous on the host thread.
/// All points and returned rectangles use the owning node's local logical coordinates.
class TextSelectionClient {
public:
  virtual ~TextSelectionClient() = default;

  /// Tests whether an action is currently available without changing the selection or clipboard.
  /// @param action Requested shared editing action.
  /// @param clipboard Borrowed clipboard service, or nullptr if unavailable; do not retain the pointer.
  /// @return True if supported by the current selection and policy; the default returns false.
  [[nodiscard]] virtual bool CanPerformTextEditingAction(TextEditingAction action, PlatformClipboard* clipboard) const {
    static_cast<void>(action);
    static_cast<void>(clipboard);
    return false;
  }

  /// Performs an available shared editing action, rechecking current selection and disclosure policy.
  /// @param action Action to execute.
  /// @param clipboard Borrowed clipboard service, or nullptr if unavailable; do not retain the pointer.
  /// @return Whether the action succeeded; the default returns false.
  /// Read-only clients can support Copy and Select All without implementing editing or IME behavior.
  virtual bool PerformTextEditingAction(TextEditingAction action, PlatformClipboard* clipboard) {
    static_cast<void>(action);
    static_cast<void>(clipboard);
    return false;
  }

  /// Cancels the current range selection without changing text or starting an input session.
  /// @return Whether selection changed; false when there is nothing to clear.
  /// Read-only clients remove their logical selection. Editors collapse it to the active endpoint, preserving
  /// affinity and publishing the change through their usual controlled editing path. No visible geometry is required.
  /// Runtime uses this after a confirmed touch tap elsewhere, not when scrolling merely hides the selection UI.
  virtual bool ClearSelection() = 0;
  /// Selects the word or selectable text unit at a gesture position.
  /// @param position Point in the owning node's local logical coordinates.
  /// @return Whether selection was handled; false when no selectable content or layout is available.
  virtual bool SelectWord(Point position) = 0;
  /// Moves one ordered selection endpoint while retaining the opposite endpoint.
  /// @param position New handle position in node-local logical coordinates.
  /// @param start_handle True for the ordered start, false for the ordered end; not anchor/active direction.
  /// @return Whether the drag was handled, even if clamping left the selection unchanged.
  /// Keep endpoints ordered and use valid caret boundaries. Built-in clients clamp at the opposite endpoint.
  virtual bool ExtendSelection(Point position, bool start_handle) = 0;
  /// @return Available node-local anchors, or std::nullopt when no logical selection or editor caret exists.
  /// Return TextSelectionGeometry{} for an existing selection whose geometry is temporarily unavailable, such as
  /// fully virtualized-away text. Runtime preserves menu intent for that value; std::nullopt ends the old menu.
  [[nodiscard]] virtual std::optional<TextSelectionGeometry> QuerySelectionGeometry() const = 0;
  /// @return The color used to paint the shared selection handles.
  [[nodiscard]] virtual Color SelectionHandleColor() const noexcept = 0;
};

/// Optional platform text-input connection supplied by PlatformAdapter::TextInput().
///
/// Runtime calls this capability on the host thread. State and geometry in each call come from the same snapshot;
/// geometry is relative to the HuxerUI host view in logical units, not node-local or physical screen coordinates.
/// The adapter converts it to the input API's coordinates as needed and tolerates geometry whose result is not Ok.
///
/// Platform callbacks route commands and queries through Runtime with a captured session ID. Do not retain a raw
/// TextInputClient pointer or treat a platform text cache as the authoritative document. Secure configuration must
/// prevent surrounding/extracted text disclosure even when trusted internal context contains the real text.
/// For a node translated by (20, 30) with no scale or rotation, a local caret at (4, 6) arrives here at (24, 36).
class PlatformTextInput {
public:
  virtual ~PlatformTextInput() = default;

  /// Starts platform input after the client has begun the session.
  /// @param session_id New active HuxerUI session ID.
  /// @param configuration Keyboard hints and input policy for this client.
  /// @param state Initial client state carrying session_id.
  /// @param geometry Initial host-view caret and range geometry, possibly unavailable.
  /// Use the supplied geometry rather than querying Runtime again solely to recover the current caret.
  virtual void Start(TextInputSessionId session_id, const TextInputConfiguration& configuration,
      const TextInputState& state, const TextInputGeometry& geometry) = 0;
  /// Refreshes the existing platform input connection without restarting its composition.
  /// @param session_id Active session to update; ignore stale IDs.
  /// @param state Current client selection, composition, and revisions.
  /// @param geometry Current host-view geometry from the same snapshot, possibly unavailable.
  virtual void
  Update(TextInputSessionId session_id, const TextInputState& state, const TextInputGeometry& geometry) = 0;
  /// Reinitializes platform input while preserving the HuxerUI session identity.
  /// @param session_id Active session to restart; ignore stale IDs.
  /// @param configuration Current configuration, including any changed input policy.
  /// @param state Current client state for the same session.
  /// @param geometry Host-view geometry from the same snapshot, possibly unavailable.
  virtual void Restart(TextInputSessionId session_id, const TextInputConfiguration& configuration,
      const TextInputState& state, const TextInputGeometry& geometry) = 0;
  /// Releases the platform input connection for an ended session without affecting a newer one.
  /// @param session_id Session to stop; ignore stale IDs.
  virtual void Stop(TextInputSessionId session_id) = 0;
  /// Requests software-keyboard visibility without changing focus or creating a new input session.
  /// @param session_id Existing active session; ignore stale IDs.
  /// Runtime uses this for a confirmed tap on an already focused client. Hiding a keyboard does not end that
  /// session; platforms without a software keyboard can retain the default no-op implementation.
  virtual void RequestShow(TextInputSessionId session_id) {
    static_cast<void>(session_id);
  }
};

} // namespace huxerui
