#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/geometry.h>
#include <huxerui/resource.h>
#include <huxerui/scroll.h>
#include <huxerui/text_input.h>

namespace huxerui {

class Runtime;

namespace detail {
struct ModifierDescriptor;
struct SemanticBuilderState;
class SemanticTree;
} // namespace detail

/// Identifies a Runtime-owned semantic entity; zero is invalid.
///
/// IDs remain stable while the same mounted node or compatible virtual child survives. They are not application
/// keys, automation identifiers, or process-global handles, and must not be persisted across Runtime lifetimes.
using SemanticNodeId = std::uint64_t;

/// Describes a control's meaning to assistive technology, independently of its appearance.
///
/// A role does not create input behavior or advertise an action by itself. Platform adapters map these shared roles
/// to native accessibility roles, using the closest available representation when necessary.
enum class SemanticRole {
  /// A semantic grouping without a more specific role.
  Generic,
  /// Static readable text.
  Text,
  /// A document or section heading; pair with Semantics::heading_level when its level is known.
  Heading,
  /// A meaningful image or self-drawn graphic with an accessible label.
  Image,
  /// A control that invokes a command.
  Button,
  /// A control that navigates to a destination.
  Link,
  /// An independently checkable option, optionally supporting a mixed state.
  Checkbox,
  /// One mutually exclusive choice within a group.
  RadioButton,
  /// An on/off setting.
  Switch,
  /// An adjustable numeric value, described by SemanticRange.
  Slider,
  /// Progress feedback rather than an adjustable input.
  ProgressIndicator,
  /// An editable or read-only choice field with expandable suggestions or options.
  ComboBox,
  /// An editable text field.
  TextField,
  /// A text field specifically used to enter a search query.
  SearchField,
  /// One selectable page destination within a TabList.
  Tab,
  /// A collection of tab destinations.
  TabList,
  /// A collection of commands or submenus.
  Menu,
  /// One command, checkable option, or submenu entry in a Menu.
  MenuItem,
  /// A dialog surface; modal isolation is owned by Runtime, not implied by this role.
  Dialog,
  /// A group of application navigation destinations.
  Navigation,
  /// A one-dimensional collection.
  List,
  /// One item in a List.
  ListItem,
  /// A collection with grid-like organization.
  Grid,
  /// One cell in a Grid, optionally carrying position and span metadata.
  GridCell,
  /// A scrollable viewport.
  ScrollView,
};

/// Describes the state of a checkable control, separately from selection within a collection.
enum class SemanticCheckedState {
  /// The option is not checked.
  Unchecked,
  /// The option is checked.
  Checked,
  /// The option represents a mixture of checked and unchecked values.
  Mixed,
};

/// Requests how assistive technology announces content changes without moving input focus.
/// Actual announcement timing and support depend on the platform accessibility adapter.
enum class SemanticLiveRegion {
  /// Does not request live announcements.
  None,
  /// Requests an announcement when it will not interrupt the current one.
  Polite,
  /// Requests prompt announcement of an important update, potentially interrupting current speech.
  Assertive,
};

/// Controls traversal into ordinary descendant Views when resolving owner semantics.
enum class SemanticDescendantPolicy {
  /// Keeps ordinary semantic descendants in committed child order.
  Preserve,
  /// Omits ordinary descendant Views; provide the owner's complete label instead of relying on automatic merging.
  Exclude,
};

/// Describes a numeric range for an adjustable control or progress indicator.
///
/// All supplied values must be finite, minimum must not exceed maximum, and current must lie inside the range.
/// This is accessibility metadata: it does not clamp or otherwise change the component's controlled value.
struct SemanticRange {
  /// Inclusive lower bound.
  double minimum = 0.0;
  /// Inclusive upper bound.
  double maximum = 0.0;
  /// Current value in the same units as the bounds.
  double current = 0.0;
  /// Positive increment when known; absence leaves the increment unspecified.
  std::optional<double> step{};

  bool operator==(const SemanticRange&) const = default;
};

/// Describes a collection's logical dimensions, including items not currently realized by a virtual layout.
/// Unknown counts remain absent; zero describes a known empty dimension.
struct SemanticCollection {
  /// Total number of logical items, not just published semantic children.
  std::optional<std::size_t> item_count{};
  /// Total row count when the collection has a row structure.
  std::optional<std::size_t> row_count{};
  /// Total column count when the collection has a column structure.
  std::optional<std::size_t> column_count{};

  bool operator==(const SemanticCollection&) const = default;
};

/// Locates an item within its logical collection; all indices are zero-based and all spans must be positive.
///
/// For example, a cell beginning in the second row and spanning two columns can declare:
/// @code
/// SemanticCollectionItem{.row_index = 1, .column_index = 0, .column_span = 2}
/// @endcode
struct SemanticCollectionItem {
  /// Linear item index when known, independent of the current realized window.
  std::optional<std::size_t> index{};
  /// First occupied row when known.
  std::optional<std::size_t> row_index{};
  /// First occupied column when known.
  std::optional<std::size_t> column_index{};
  /// Number of occupied rows, including the starting row.
  std::size_t row_span = 1;
  /// Number of occupied columns, including the starting column.
  std::size_t column_span = 1;

  bool operator==(const SemanticCollectionItem&) const = default;
};

/// Declares accessible meaning through View::With() or a SemanticBuilder contribution.
///
/// Unset fields leave earlier declarations unchanged. Explicit values, including false and an empty string, override
/// earlier values. View modifiers apply left to right after component defaults and NodeExtension contributions.
/// User-facing strings resolve through the effective Locale; identifier is never localized.
///
/// These declarations do not implement interactions. Runtime remains authoritative for enabled state, input focus,
/// editable selection, scrolling, secure text, visibility, and geometry. Do not copy protected text into labels, hints,
/// errors, or other application-authored metadata: secure-field redaction cannot identify secrets in such metadata.
///
/// A compound display can expose one complete description and omit its decorative descendant Views:
/// @code
/// return Row {
///   Text("Connection"),
///   Text("Online"),
/// }.With(Semantics{
///     .label = "Connection",
///     .value = "Online",
///     .descendants = SemanticDescendantPolicy::Exclude,
/// });
/// @endcode
struct Semantics {
  static const detail::ModifierDescriptor& Descriptor();

  /// Meaning of this element; does not create behavior for the chosen role.
  std::optional<SemanticRole> role{};
  /// Accessible name, kept separate from the current value, hint, and validation error.
  std::optional<StringVariant> label{};
  /// Human-readable current value; secure text-input values are removed by Runtime.
  std::optional<StringVariant> value{};
  /// Prompt for an empty value, not a substitute for a persistent label.
  std::optional<StringVariant> placeholder{};
  /// Additional usage guidance without repeating the label.
  std::optional<StringVariant> hint{};
  /// Human-readable state description when the standard state fields do not fully describe it.
  std::optional<StringVariant> state_description{};
  /// Application-owned validation message, separate from the invalid flag.
  std::optional<StringVariant> error{};
  /// Nonlocalized identifier for automation; it is neither the accessible name nor SemanticNodeId.
  std::optional<std::string> identifier{};
  /// Check state; absence means no check state is declared.
  std::optional<SemanticCheckedState> checked{};
  /// Whether this item is selected; absence is distinct from an explicitly unselected item.
  std::optional<bool> selected{};
  /// Whether expandable content is open; does not open or close it.
  std::optional<bool> expanded{};
  /// Whether an operation is in progress.
  std::optional<bool> busy{};
  /// Whether the value is read-only; text-input configuration takes precedence.
  std::optional<bool> read_only{};
  /// Whether the application requires a value; does not perform validation.
  std::optional<bool> required{};
  /// Whether the application considers the value invalid; does not reject edits.
  std::optional<bool> invalid{};
  /// Heading depth from one through six, normally paired with SemanticRole::Heading.
  std::optional<unsigned int> heading_level{};
  /// Numeric bounds, current value, and optional increment.
  std::optional<SemanticRange> range{};
  /// Normalized nonnegative, half-open UTF-16 range; actual text-input selection takes precedence.
  std::optional<TextRange> text_selection{};
  /// Finite nonnegative scroll metrics whose offset lies within maximum_offset; mounted scrolling takes precedence.
  std::optional<ScrollMetrics> scroll{};
  /// Logical collection dimensions rather than the number of currently mounted children.
  std::optional<SemanticCollection> collection{};
  /// This item's logical collection position and spans.
  std::optional<SemanticCollectionItem> collection_item{};
  /// Announcement priority for content changes; does not move keyboard focus.
  std::optional<SemanticLiveRegion> live_region{};
  /// Whether ordinary descendant Views contribute their own semantics; defaults to Preserve.
  std::optional<SemanticDescendantPolicy> descendants{};
  /// Removes this mounted owner and its subtree from accessibility when true, without changing drawing or input.
  std::optional<bool> hidden{};

  bool operator==(const Semantics&) const = default;
};

/// Identifies an accessibility request and determines its SemanticAction::value alternative.
/// Actions not explicitly described as taking a payload require std::monostate.
enum class SemanticActionKind : std::uint8_t {
  /// Invokes the primary command, toggle, or selection behavior.
  Activate,
  /// Requests Runtime input focus, not the screen reader's independent accessibility focus.
  Focus,
  /// Requests replacement text carried by a UTF-8 std::string.
  SetText,
  /// Requests a normalized nonnegative, half-open UTF-16 TextRange.
  SetSelection,
  /// Requests a finite double in the component's value units.
  SetValue,
  /// Requests the next value according to the component's increment and availability rules.
  Increment,
  /// Requests the previous value according to the component's increment and availability rules.
  Decrement,
  /// Requests a relative Point scroll delta in logical units, not an absolute offset or screen position.
  Scroll,
  /// Requests that the element be revealed by its scroll ancestors.
  ShowOnScreen,
  /// Requests opening expandable content.
  Expand,
  /// Requests closing expandable content.
  Collapse,
  /// Requests dismissal through the component's existing dismissal policy.
  Dismiss,
  /// Invokes a declared custom action identified by a std::uint64_t payload.
  Custom,
};

/// Returns the bit corresponding to an action in SemanticNode::actions.
/// @param action Action to test or combine with other action bits.
/// @return A single-bit mask for a defined action, or zero for an invalid enum value.
/// @code
/// const bool can_activate = (node.actions & SemanticActionMask(SemanticActionKind::Activate)) != 0;
/// @endcode
[[nodiscard]] constexpr std::uint64_t SemanticActionMask(SemanticActionKind action) noexcept {
  const std::uint8_t index = static_cast<std::uint8_t>(action);
  return index <= static_cast<std::uint8_t>(SemanticActionKind::Custom) ? std::uint64_t{1} << index : 0;
}

/// Carries a platform-neutral request to Runtime::PerformSemanticAction() or NodeExtension::OnSemanticAction().
///
/// The value alternative must match kind. Runtime rejects invalid payloads, stale targets, and unavailable actions;
/// the component still validates its own domain constraints and emits controlled-value proposals.
/// @code
/// SemanticAction activate{SemanticActionKind::Activate, std::monostate{}};
/// SemanticAction selection{SemanticActionKind::SetSelection, TextRange{0, 5}};
/// SemanticAction custom{SemanticActionKind::Custom, std::uint64_t{31}};
/// @endcode
struct SemanticAction {
  /// Requested operation, which also determines the payload type.
  SemanticActionKind kind = SemanticActionKind::Activate;
  /// Payload documented by SemanticActionKind; default construction supplies std::monostate.
  std::variant<std::monostate, std::string, TextRange, double, Point, std::uint64_t> value;

  bool operator==(const SemanticAction&) const = default;
};

/// Contains the resolved meaning, state, actions, and geometry of one committed accessible element.
///
/// Platform adapters read these values from a published SemanticFrame; applications declare Semantics instead of
/// editing snapshots. Strings are resolved UTF-8, and absent optional states remain distinct from false states.
struct SemanticNode {
  /// Nonzero Runtime identity, independent of the node's position in SemanticFrame::nodes.
  SemanticNodeId id = 0;
  /// Parent identity, absent for the synthetic host root.
  std::optional<SemanticNodeId> parent{};
  /// Runtime-owned PlatformView anchor identity for bridging a native accessibility subtree.
  std::optional<std::uint64_t> platform_view_identity{};
  /// Child identities in accessibility traversal order; entries are IDs, not vector indices.
  std::vector<SemanticNodeId> children{};
  /// Resolved shared role.
  SemanticRole role = SemanticRole::Generic;
  /// Accessible name; empty when no meaningful name was supplied.
  std::string label{};
  /// Formatted current value, empty for secure text input.
  std::string value{};
  /// Prompt associated with an empty value.
  std::string placeholder{};
  /// Additional usage guidance.
  std::string hint{};
  /// Human-readable state information beyond the standard state fields.
  std::string state_description{};
  /// Resolved application validation message.
  std::string error{};
  /// Nonlocalized author-supplied automation identifier.
  std::string identifier{};
  /// Check state when supported by this element.
  std::optional<SemanticCheckedState> checked{};
  /// Whether this item is selected, when selection state is present.
  std::optional<bool> selected{};
  /// Whether expandable content is open, when expansion state is present.
  std::optional<bool> expanded{};
  /// Whether an operation is in progress, when busy state is present.
  std::optional<bool> busy{};
  /// Whether edits are disallowed; resolved from text-input configuration when applicable.
  std::optional<bool> read_only{};
  /// Whether the application requires a value.
  std::optional<bool> required{};
  /// Whether application validation marks the value invalid.
  std::optional<bool> invalid{};
  /// Declared heading depth from one through six.
  std::optional<unsigned int> heading_level{};
  /// Numeric value metadata, when present.
  std::optional<SemanticRange> range{};
  /// Committed normalized UTF-16 selection; absent for secure text input.
  std::optional<TextRange> text_selection{};
  /// Resolved scroll metrics in logical units.
  std::optional<ScrollMetrics> scroll{};
  /// Logical collection dimensions, including unrealized items when known.
  std::optional<SemanticCollection> collection{};
  /// Logical position and spans within a collection.
  std::optional<SemanticCollectionItem> collection_item{};
  /// Requested announcement priority for content changes.
  SemanticLiveRegion live_region = SemanticLiveRegion::None;
  /// Effective availability; virtual children also inherit the mounted owner's disabled state.
  bool enabled = true;
  /// Runtime keyboard/input focus, not platform accessibility focus.
  bool focused = false;
  /// Whether the mounted text-input configuration accepts multiple lines.
  bool multiline = false;
  /// Whether text-input content is protected; value and text_selection are redacted when true.
  bool secure = false;
  /// Whether bounds have no intersection with the effective viewport and ancestor rectangular clips.
  bool offscreen = false;
  /// Available action bits, including Custom when custom_actions is nonempty; use SemanticActionMask to inspect them.
  std::uint64_t actions = 0;
  /// Available custom actions as (nonzero action ID, localized label) pairs; empty when disabled.
  std::vector<std::pair<std::uint64_t, std::string>> custom_actions{};
  /// Full axis-aligned bounds in host-view logical coordinates after presentation transforms, not clipped bounds.
  Rect bounds;

  bool operator==(const SemanticNode&) const = default;
};

/// Owns a complete semantic snapshot from the same committed layout and state as its FrameCommit.
///
/// Retain the FrameCommit::semantic_frame shared pointer while querying nodes. Publication is immutable; mutating a
/// copy does not update Runtime. The nodes array is flat, and hierarchy is expressed by node IDs, not array indices.
struct SemanticFrame {
  /// Monotonically increasing revision within a Runtime; unchanged semantic output keeps its existing revision.
  std::uint64_t revision = 0;
  /// Identity of the synthetic host root contained in nodes.
  SemanticNodeId root = 0;
  /// All published nodes, including the synthetic root and retained offscreen nodes.
  std::vector<SemanticNode> nodes;

  bool operator==(const SemanticFrame&) const = default;
};

/// Publishes one NodeExtension's owner contribution and flat virtual children during BuildSemantics().
///
/// The builder is borrowed for that call only and must not be retained. Local ID zero denotes the mounted owner;
/// nonzero child IDs are scoped to the extension and must stay stable for the same logical item across updates.
/// Use actual child Views when they already own the behavior; virtual semantics are for self-drawn items.
///
/// Declare actions here and handle them in NodeExtension::OnSemanticAction(), using the same validation and typed
/// events as ordinary input. Call InvalidateSemantics() when retained semantic state changes; declaration alone does
/// not schedule future updates or implement input behavior.
///
/// For a self-drawn chart with a reset button in the given local rectangle, the extension can forward the action to
/// the owning View's `.On<ResetZoom>(...)` binding:
/// @code
/// struct ResetZoom : Event<void()> {};
/// class ChartSemantics final : public NodeExtension {
/// public:
///   void BuildSemantics(SemanticBuilder& builder) const override {
///     builder.SetOwner(Semantics{.role = SemanticRole::Image, .label = "Monthly revenue"});
///     builder.AddChild(1, Rect{8.0F, 8.0F, 96.0F, 32.0F},
///                      Semantics{.role = SemanticRole::Button, .label = "Reset zoom"}, true);
///     builder.AddAction(1, SemanticActionKind::Activate);
///   }
///   bool OnSemanticAction(std::uint64_t local_id, const SemanticAction& action) override {
///     if (local_id != 1 || action.kind != SemanticActionKind::Activate) {
///       return false;
///     }
///     EmitEvent<ResetZoom>();
///     return true;
///   }
/// };
/// @endcode
class SemanticBuilder {
public:
  SemanticBuilder(const SemanticBuilder&) = delete;
  SemanticBuilder& operator=(const SemanticBuilder&) = delete;
  SemanticBuilder(SemanticBuilder&&) = delete;
  SemanticBuilder& operator=(SemanticBuilder&&) = delete;

  /// Declares or updates the mounted owner's contribution for local ID zero.
  ///
  /// Repeated calls merge supplied fields. Application Semantics modifiers take precedence over this contribution,
  /// and Runtime-owned state remains authoritative. Call this before adding actions for local ID zero.
  /// @param semantics Owner properties, resolved using the mounted owner's environment and resource service.
  /// @throws std::invalid_argument If supplied range, selection, heading, scroll, or span metadata is invalid.
  void SetOwner(Semantics semantics);

  /// Declares one virtual child beneath the mounted owner's semantic node without creating a View or input target.
  ///
  /// Disabled children remain discoverable with stable identities but expose no standard or custom actions. Apply
  /// the same availability check to actual input; this declaration does not disable pointer or keyboard handlers.
  /// Child hierarchy is flat. Tree visibility is controlled by the mounted owner; omit a child to hide that item.
  /// @param local_id Nonzero ID unique within this extension's current contribution and stable for the logical item.
  /// @param local_bounds Owner-local logical rectangle with finite coordinates and finite, nonnegative dimensions.
  /// @param semantics Child meaning and state, with user-facing strings resolved in the owner's environment.
  /// @param enabled Child availability; false disables this child, while true cannot enable a disabled owner.
  /// @throws std::invalid_argument If local_id is zero, bounds are invalid, or semantic metadata is invalid.
  /// @throws std::logic_error If local_id has already been declared in this contribution.
  void AddChild(std::uint64_t local_id, Rect local_bounds, Semantics semantics, bool enabled);

  /// Advertises a standard action handled by this extension's OnSemanticAction().
  ///
  /// Repeating the same declaration is harmless, but two extensions must not claim the same owner action.
  /// Runtime suppresses advertised actions when the effective target is disabled.
  /// @param local_id Zero after SetOwner(), or a nonzero child ID already supplied to AddChild().
  /// @param action Standard action kind; use AddCustomAction() for Custom.
  /// @throws std::invalid_argument If action is Custom or an invalid enum value.
  /// @throws std::logic_error If local_id has not been declared.
  void AddAction(std::uint64_t local_id, SemanticActionKind action);

  /// Advertises a named custom action routed back to this extension with a uint64_t action-ID payload.
  /// @param local_id Zero after SetOwner(), or a nonzero child ID already supplied to AddChild().
  /// @param action_id Nonzero ID unique on the target semantic node, including other owner contributions for ID zero.
  /// @param label User-facing action name; its localized value must contain a non-whitespace character.
  /// @throws std::invalid_argument If the resolved label is empty or whitespace-only.
  /// @throws std::logic_error If the target is undeclared or an action ID is zero or duplicated.
  void AddCustomAction(std::uint64_t local_id, std::uint64_t action_id, StringVariant label);

private:
  explicit SemanticBuilder(detail::SemanticBuilderState& state) noexcept : state_(&state) {}

  detail::SemanticBuilderState* state_;

  friend class detail::SemanticTree;
};

} // namespace huxerui
