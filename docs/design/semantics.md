# Semantics and Accessibility Design

Status: implemented foundation with deferred component and platform coverage

This document defines the implemented platform-neutral semantics foundation and records deferred component and platform coverage explicitly.

Semantics is shared Runtime output.
Components and applications declare meaning, Runtime resolves the committed semantic hierarchy, and platform adapters expose that hierarchy through native accessibility APIs.
Renderers do not infer semantics from pixels or PaintCommands.

## Goals

- Give built-in and custom controls one shared model for roles, names, values, states, actions, focus, collections, and geometry.
- Preserve the existing `View`, modifier, typed event, `NodeExtension`, Runtime, and PlatformAdapter boundaries.
- Publish immutable committed data that native accessibility objects can retain safely after `BuildFrame()` returns.
- Support self-drawn composite controls without creating fake MountedNodes or native Views.
- Keep Runtime input focus, text-input ownership, and platform accessibility focus distinct.
- Prevent TextField-owned secure content from entering committed semantics.
- Keep the shared type and action surface closed, platform-neutral, and explicit about which coverage remains deferred.

## Non-goals

The first implementation does not provide OCR, pixel-derived names, a DOM renderer, a native View for every HuxerUI View, or platform-specific accessibility properties in shared application code.

It does not expose raw ARIA attributes, Android class names, Apple accessibility traits, UI Automation control types, or AT-SPI interface names.

NativeView accessibility is a separate leaf-node integration.
A NativeView supplies or bridges its native subtree and suppresses an equivalent HuxerUI semantic subtree for that leaf.

## Ownership

| Layer | Responsibility |
|---|---|
| Components and application Views | Declare platform-neutral semantic properties |
| NodeExtension | Contribute retained state, virtual semantic children, and semantic-only behavior |
| Runtime | Resolve the hierarchy, identity, hard state, geometry, secure-data policy, and actions |
| Platform accessibility adapter | Retain `SemanticFrame`, expose native objects, translate native actions, and issue native notifications |
| Renderer | Render `RenderScene`; it does not construct semantics |

The flow is:

```text
component defaults
    + NodeExtension contribution
    + application Semantics overrides
    + Runtime focus, visibility, security, and geometry
        -> SemanticFrame
        -> native accessibility hierarchy
        -> SemanticAction
        -> Runtime
```

There is no platform Runtime variant and no concrete-component switch in a platform adapter.

Runtime retains one internal semantic patch for component meaning and an optional patch for author declarations.
The optional patch's presence preserves an explicit empty `Semantics{}` without a separate declaration flag or wrapper type.
NodeExtension contributions are applied between those patches, and Runtime hard state remains authoritative.

## Public declarations

The public declarations live in `<huxerui/semantics.h>` and are re-exported from `<huxerui/huxerui.h>`.

`Semantics` is a reusable property modifier:

```cpp
return Canvas(...).With(
    Semantics{
        .role = SemanticRole::Image,
        .label = "Monthly revenue chart",
    }
);
```

User-facing strings use `StringVariant` and follow the existing localization model.
The optional `identifier` is machine-facing and is not localized.

### Roles

The initial shared roles are:

```cpp
enum class SemanticRole {
  Generic,
  Text,
  Heading,
  Image,
  Button,
  Link,
  Checkbox,
  RadioButton,
  Switch,
  Slider,
  ProgressIndicator,
  TextField,
  SearchField,
  Tab,
  TabList,
  Menu,
  MenuItem,
  Dialog,
  Navigation,
  List,
  ListItem,
  Grid,
  GridCell,
  ScrollView,
};
```

A role expresses a shared distinction, not a native class name.
An adapter maps an unsupported distinction to the closest native role while preserving name, state, actions, and hierarchy.

### State and structure

```cpp
enum class SemanticCheckedState {
  Unchecked,
  Checked,
  Mixed,
};

enum class SemanticLiveRegion {
  None,
  Polite,
  Assertive,
};

enum class SemanticDescendantPolicy {
  Preserve,
  Exclude,
};
```

Range and collection data remain small value structures because their fields have shared meaning across native APIs:

```cpp
struct SemanticRange {
  double minimum = 0.0;
  double maximum = 0.0;
  double current = 0.0;
  std::optional<double> step;
};

struct SemanticCollection {
  std::optional<std::size_t> item_count;
  std::optional<std::size_t> row_count;
  std::optional<std::size_t> column_count;
};

struct SemanticCollectionItem {
  std::optional<std::size_t> index;
  std::optional<std::size_t> row_index;
  std::optional<std::size_t> column_index;
  std::size_t row_span = 1;
  std::size_t column_span = 1;
};
```

Ranges require finite values, `minimum <= maximum`, a current value inside the range, and a positive step when present.
Collection spans must be positive.
Unknown counts remain absent instead of using sentinel values.

### Semantics modifier

The modifier is a flat set of optional overrides:

```cpp
struct Semantics {
  std::optional<SemanticRole> role;
  std::optional<StringVariant> label;
  std::optional<StringVariant> value;
  std::optional<StringVariant> placeholder;
  std::optional<StringVariant> hint;
  std::optional<StringVariant> state_description;
  std::optional<StringVariant> error;
  std::optional<std::string> identifier;
  std::optional<SemanticCheckedState> checked;
  std::optional<bool> selected;
  std::optional<bool> expanded;
  std::optional<bool> busy;
  std::optional<bool> read_only;
  std::optional<bool> required;
  std::optional<bool> invalid;
  std::optional<unsigned int> heading_level;
  std::optional<SemanticRange> range;
  std::optional<SemanticCollection> collection;
  std::optional<SemanticCollectionItem> collection_item;
  std::optional<SemanticLiveRegion> live_region;
  std::optional<SemanticDescendantPolicy> descendants;
  std::optional<bool> hidden;
};
```

Optional booleans distinguish an unsupported state from a supported false state.
Heading levels are limited to one through six.

There is no separate replace modifier, exclusion type, traversal-order type, or public security policy object.
`hidden = true` removes the owner and its descendants from semantics.
`SemanticDescendantPolicy::Exclude` keeps the owner and removes only its descendants.
`Preserve` is the default and retains ordinary semantic descendants.
Automatic descendant merging is intentionally absent until HuxerUI has concrete conflict and action-routing rules; a component that represents one semantic unit declares its complete label and excludes decorative descendants explicitly.

Runtime-derived enabled state, input focus, multiline editing, and secure editing are resolved from mounted behavior rather than application overrides.

## Resolution

Runtime resolves fields in this order:

- Built-in component defaults.
- Compatible NodeExtension contribution.
- Explicit `Semantics` modifiers from left to right.
- Runtime hard state, visibility, secure-data policy, actions, and final geometry.

The Runtime pass is authoritative.
An application cannot mark a disabled subtree enabled, publish input focus that Runtime does not own, or expose a secure TextField value through the `value` field.

Role never creates behavior.
A Button role without an activation binding is not actionable, while an explicit activation binding is advertised only on a semantic node that can still perform it.

## Names and descriptions

The accessible name resolves from:

- An explicit application label.
- A component-owned semantic label, including icon-only item labels.
- The content itself for a Text node.

Value, placeholder, hint, state description, and error remain separate fields.
Runtime does not concatenate them into the label because native platforms announce and update them differently.

Whitespace-only names are absent after localization.
Runtime does not substitute an identifier, resource name, file path, or component type as a user-facing name.

## Tree construction

`SemanticFrame` contains one synthetic root and a flat owning array of nodes.
The synthetic root represents the host View and lets application content, layers, and live regions share one native root.

Row, Column, Stack, visual wrappers, padding, backgrounds, indications, and other layout-only nodes are transparent unless they declare meaningful semantics.
Purely decorative nodes are absent.

A node is emitted when it has meaningful content, state, actions, collection structure, live-region behavior, or an explicit semantic declaration.
Semantic order follows committed child order.
The initial API does not provide arbitrary traversal ordering.

The builder rejects duplicate extension-local child IDs, invalid child geometry, actions without a declared owner or child, and conflicting action routes as framework invariant failures.

## NodeExtension contribution

Most MountedNodes contribute at most one semantic node, but a self-drawn composite control may contribute flat virtual children.
SegmentedButton, Tabs, NavigationBar, NavigationPane, and a custom Canvas chart need this capability without fake mounted Views.

`NodeExtension` gains one contribution method, one action method, and one invalidation operation:

```cpp
class SemanticBuilder;

class NodeExtension {
public:
  virtual void BuildSemantics(SemanticBuilder& builder) const;

  virtual bool OnSemanticAction(
      std::uint64_t local_id,
      const SemanticAction& action
  );

protected:
  void InvalidateSemantics();
};
```

Local ID zero identifies the owner's primary semantic node.
Virtual children use nonzero extension-local IDs stable across compatible updates and reordering.

`SemanticBuilder` has only the operations required by this extension boundary:

```cpp
class SemanticBuilder {
public:
  SemanticBuilder(const SemanticBuilder&) = delete;
  SemanticBuilder& operator=(const SemanticBuilder&) = delete;

  void SetOwner(Semantics semantics);
  void AddChild(
      std::uint64_t local_id,
      Rect local_bounds,
      Semantics semantics
  );
  void AddAction(
      std::uint64_t local_id,
      SemanticActionKind action
  );
  void AddCustomAction(
      std::uint64_t local_id,
      std::uint64_t action_id,
      StringVariant label
  );
};
```

The builder is valid only for the duration of `BuildSemantics()` and cannot be copied, moved, or retained.
SetOwner applies dynamic properties to the mounted owner's declaration.
AddChild creates one flat virtual child with owner-local bounds.
The action operations require an existing owner or child local ID and do not accept callbacks.
It cannot publish a frame, insert native objects, or retain a MountedNode pointer.

Runtime privately records the mounted owner, compatible extension handle, and local ID for each actionable semantic node.
An action looks up that route again on the Runtime UI thread, so platforms and extensions never retain mounted pointers.

Built-in and third-party controls use the same NodeExtension capability.
Runtime does not switch on concrete component types.

## Identity

```cpp
using SemanticNodeId = std::uint64_t;
```

Runtime allocates nonzero IDs monotonically and never reuses an ID for another semantic entity during its lifetime.
The primary ID follows a compatible MountedNode.
A virtual child identity combines the mounted owner with its stable local ID.

A virtualized item that is fully evicted is removed; returning content may receive a new semantic ID unless its retained virtual-item state preserves the identity.

`SemanticNodeId` is not an application key, automation identifier, native object ID, or process-global handle.
The optional author `identifier` is mapped to native automation identifiers.

## SemanticFrame

`SemanticFrame` is the complete committed semantic output:

```cpp
struct SemanticFrame {
  std::uint64_t revision = 0;
  SemanticNodeId root = 0;
  std::vector<SemanticNode> nodes;
};

struct FrameCommit {
  RenderFrame render_frame;
  std::shared_ptr<const SemanticFrame> semantic_frame;
  std::optional<double> next_frame_deadline;
};
```

The resolved node data is:

```cpp
struct SemanticNode {
  SemanticNodeId id = 0;
  std::optional<SemanticNodeId> parent;
  std::vector<SemanticNodeId> children;
  SemanticRole role = SemanticRole::Generic;
  std::string label;
  std::string value;
  std::string placeholder;
  std::string hint;
  std::string state_description;
  std::string error;
  std::string identifier;
  std::optional<SemanticCheckedState> checked;
  std::optional<bool> selected;
  std::optional<bool> expanded;
  std::optional<bool> busy;
  std::optional<bool> read_only;
  std::optional<bool> required;
  std::optional<bool> invalid;
  std::optional<unsigned int> heading_level;
  std::optional<SemanticRange> range;
  std::optional<SemanticCollection> collection;
  std::optional<SemanticCollectionItem> collection_item;
  SemanticLiveRegion live_region = SemanticLiveRegion::None;
  bool enabled = true;
  bool focused = false;
  bool multiline = false;
  bool secure = false;
  std::uint64_t actions = 0;
  std::vector<std::pair<std::uint64_t, std::string>> custom_actions;
  Rect bounds;
};
```

Standard actions occupy bits assigned by `SemanticActionKind`.
Custom action pairs contain the extension-local action ID and resolved label.
It contains no pointer to MountedNode, ViewSpec, NodeExtension, RenderNode, or a temporary builder.

`SemanticFrame` is immutable after publication.
It represents the same reconciliation, layout, presentation, focus, and layer state as the surrounding `FrameCommit`.
A platform adapter retains the shared pointer for as long as native queries may reference it.

Runtime increments the nonzero revision and creates a new `SemanticFrame` only when semantic content, structure, focus, or geometry changes.
A color-only render frame reuses the previous semantic frame.

The first implementation does not publish a separate change-set type.
An adapter compares retained frames internally when its native notification API benefits from finer updates.

## Actions

The first standard action set is:

```cpp
enum class SemanticActionKind {
  Activate,
  Focus,
  SetText,
  SetSelection,
  SetValue,
  Increment,
  Decrement,
  Scroll,
  ShowOnScreen,
  Expand,
  Collapse,
  Dismiss,
  Custom,
};
```

Checkbox, Switch, RadioButton, Tabs, segmented items, and navigation destinations use `Activate` rather than separate Toggle or Select actions.
Their role and state let a native adapter expose the appropriate native toggle or selection pattern, while the explicitly advertised Activate action proves that Runtime behavior exists.

`SemanticAction` uses one payload variant rather than one request class per action:

```cpp
struct SemanticAction {
  SemanticActionKind kind;
  std::variant<
      std::monostate,
      std::string,
      TextRange,
      double,
      Point,
      std::uint64_t> value;
};
```

The payload represents SetText, SetSelection, SetValue, Scroll, or a Custom action ID respectively; parameterless actions use `std::monostate`.
Standard action availability is stored as a compact bit mask directly on `SemanticNode`.
A custom action adds only a local integer ID and localized label.
There is no separate action descriptor hierarchy.

Copy, Cut, Paste, Select All, Undo, and Redo remain in the existing TextInput and `TextEditingAction` path.
The semantics layer does not duplicate that protocol.

The platform calls `Runtime::PerformSemanticAction(SemanticNodeId, SemanticAction)` on the Runtime UI thread.
Runtime validates the ID, latest committed action availability, enabled state, payload, mounted owner, extension compatibility, and local route before dispatch.
An action kind with the wrong payload alternative is invalid and returns false before routing.
Stale or otherwise invalid actions also return false.

Activate reuses the existing activation or typed Click path.
Focus reuses Runtime input focus.
An extension may advertise any other standard or custom action only when its `OnSemanticAction` implementation can handle it.
The current Slider extension implements SetValue, Increment, and Decrement through its existing controlled change event.
Built-in TextField editing, scrolling, ShowOnScreen, expand, collapse, and dismiss routes remain deferred.

## Geometry

Node geometry uses host-view logical coordinates after final layout offsets and presentation transforms.
Non-axis-aligned geometry is represented by a conservative axis-aligned bounds rectangle.
The current frame publishes full node bounds and honors explicit `hidden` and descendant exclusion.
Ancestor clip intersection, navigation and layer visibility, semantic hit testing, and offscreen ShowOnScreen behavior remain deferred and therefore are not represented by a partial `visible_bounds` field.

## Focus

Runtime input focus is committed as node state.
The Focus action follows the same focus path used by keyboard and pointer input and starts TextInput only when the target owns a TextInputClient.

Platform accessibility focus remains native state.
Moving VoiceOver, TalkBack, Narrator, or another screen reader to a semantic node does not emit `ViewEvents::FocusChanged`, alter keyboard traversal, or start text input.

Semantic modal isolation remains deferred.
It must eventually derive from the retained Layer and focus-trap state rather than an author-settable boolean that could contradict Runtime behavior.

## Text editing and secure values

TextField currently contributes its role, label, nonsecure value, placeholder, validation state, read-only state, multiline state, secure state, and Runtime input focus.
Semantic selection and editing actions remain deferred and will use the existing UTF-16 `TextRange` and TextInputClient contracts.

An ordinary TextField may publish its committed value and selection for native accessibility editing.
Composition details remain in the existing bounded TextInputClient query path.

A secure TextField frame never contains TextField-owned plaintext, selected text, surrounding text, composition text, clipboard content, or plaintext-derived state descriptions.
Runtime commits protected state without a value or protected length.
Copy and Cut remain unavailable through the existing editing policy.

Application-authored labels, hints, errors, and identifiers are trusted metadata and must not copy the protected value.
Platform adapters do not reconstruct content from mask PaintCommands or add another text query path.

The complete editing and security contract remains in [Text Input and TextField Design](text-input.md).

## Components and virtualization

The implemented component defaults are:

| Component | Semantic output |
|---|---|
| Text | Text role and content; Heading only when declared |
| Image | Decorative by default; Image role only with a label |
| Button | Button role, label, enabled state, and Activate |
| IconButton | Button role, required semantic label, enabled state, and Activate |
| TopAppBar | The required title is one Heading; leading and action semantics remain siblings |
| Chip | Button role, selected state where applicable, and Activate |
| Checkbox | Checkbox role, label, checked state, and Activate |
| RadioButton | RadioButton role, label, checked state, and Activate |
| Switch | Switch role, label, checked state, and Activate |
| Slider | Slider role, range, SetValue, Increment, and Decrement |
| TextField | TextField or author-overridden SearchField role and basic field metadata |
| Canvas | No inferred semantics; explicit owner semantics or virtual children |

Icon-only item constructors continue to require their existing semantic label.
Material, Flat, and third-party visual themes do not change component semantics.

Remaining composite controls, scrolling, virtualization, selection, destination navigation, presentation surfaces, and live-region defaults remain deferred.
Their future implementations must use the same owner/virtual-child and retained action-routing contracts rather than adding component-specific Runtime branches.

## Platform mapping

Each adapter will retain the newest `SemanticFrame` and cache native objects by SemanticNodeId.
Native objects never retain MountedNode or NodeExtension pointers.

Only the initial macOS bridge is implemented.
The remaining platform subsections define the intended adapter boundary, not current support.

### Android

`HuxerUIView` exposes virtual descendants through `AccessibilityNodeProvider`.
Adapter-local integer IDs map to SemanticNodeIds and are not reused for unrelated content during the host View lifetime.

Roles, states, collections, text, geometry, and actions map to `AccessibilityNodeInfo` and `performAction` on the Android UI thread.
Accessibility focus stays provider-owned; native input-focus requests call Runtime Focus.

### iOS

The UIKit host implements `UIAccessibilityContainer` and retains `UIAccessibilityElement` objects by SemanticNodeId.
Traits, values, custom actions, and geometry derive from the committed semantic frame.

VoiceOver focus stays UIKit-owned.
Runtime actions run on the main thread, while the existing `UITextInput` object remains the only native editing service.

### macOS

The AppKit host currently exposes retained `NSAccessibilityElement` children with mapped roles, labels, basic values, hints, enabled and focused state, hierarchy, screen geometry, and press or range actions from the semantic frame.
It preserves mixed checked state and emits separate structure, title, value, and focus notifications by comparing retained frames.
The focused AppKit property represents keyboard focus and may route a Runtime Focus action; the VoiceOver cursor remains AppKit-owned and is not committed as Runtime input focus.
AppKit calls and Runtime actions remain on the main thread.
`NSTextInputClient` continues to own IME communication.

### Windows

The HWND exposes a UI Automation fragment root with cached providers keyed by SemanticNodeId.
Providers supply only the control patterns supported by committed role, state, and actions.

UI Automation may query off-thread.
Providers answer read-only queries from a retained immutable `SemanticFrame` and marshal actions to the HWND thread before calling Runtime.

### Linux

The X11 adapter exposes roles, states, hierarchy, component geometry, actions, values, selection, text, and collections through AT-SPI over D-Bus.
AT-SPI events follow committed revision changes, and actions return to the X11 Runtime thread.

### Web

The Web adapter maintains minimal semantic DOM associated with the Canvas.
It maps meaningful nodes to native HTML semantics and uses ARIA only where HTML is insufficient.

Semantic DOM is not a visual renderer and does not mirror every View.
It coordinates browser focus with the hidden input and textarea so TextField does not create duplicate keyboard focus targets.

### Future platforms

OHOS consumes the same `SemanticFrame` and action entry point.
Platform-only native types do not enter shared roles, states, or actions.

## Invalidation and performance

Semantic dirtiness is separate from paint dirtiness.
A label or checked-state change may create a new semantic frame without recording a PaintSequence, while a color-only change reuses the current semantic frame.

Recomposition marks semantics dirty when declarations, events, enabled state, child structure, or semantic-capable retained modifiers change.
Layout, scrolling, presentation transforms, focus, and text editing are reflected when the next semantic frame is built.

`InvalidateSemantics()` requests a frame for retained semantic state without implying paint or layout invalidation.
An extension changing paint and semantics requests both explicitly.

Runtime builds semantics after final presentation geometry and text-input session refresh and before returning `FrameCommit`.
It does not call native accessibility APIs while building.

The initial Runtime always produces semantics.
There is no enablement API or assistive-technology detection race.
Unchanged frames reuse the immutable shared object, and additional caching is added only after profiling demonstrates a need.

## Threading

Runtime construction and action dispatch run on the Runtime UI thread.
NodeExtension semantic callbacks do not run concurrently with reconciliation, frame construction, or unmount.

Native read-only queries use a retained immutable frame and do not call Runtime for names, children, states, or geometry.
Native actions arriving during frame construction are queued or marshaled and cannot re-enter `BuildFrame()`.

## Validation

Caller-supplied invalid ranges, spans, heading levels, virtual-child bounds, and custom action labels throw `std::invalid_argument` with an English HuxerUI diagnostic.
Duplicate local IDs, actions without a declared semantic item, and conflicting extension routes throw `std::logic_error`; stale native actions return false.
Broader role-state and automation-identifier diagnostics remain deferred.

## Testing

Current shared tests cover:

- Public-header self-containment and umbrella export.
- Built-in basics, modifier precedence, explicit empty declarations, hidden nodes, descendant exclusion, and disabled actions.
- Compatible updates, replacement, unmount through replacement, identity stability, revision changes, focus, and stale action rejection.
- Virtual child identity, action routing, extension replacement, stale-route rejection, and identity retirement.
- Slider range actions, invalid payload rejection, shared value validation, and secure TextField redaction.

Dedicated macOS accessibility fixtures and manual screen-reader validation remain deferred.
Manual validation uses the native screen readers and accessibility inspectors available on each platform.
Unavailable platforms and tools remain explicitly unverified.

## Delivery status and sequence

- Public value types, the `Semantics` modifier, `SemanticFrame`, Runtime-owned stable identity, immutable-frame reuse, secure TextField redaction, basic action routing, NodeExtension virtual children, and the initial macOS AppKit bridge in `appkit_accessibility.mm` are implemented.
- Deferred: derive modal isolation and layer visibility from Runtime-owned presentation state; add clip-aware geometry, scrolling, navigation, live-region, collection, and virtualized-item resolution with focused tests.
- Deferred: complete defaults and actions for remaining composite controls, TextField editing, scrolling, selection, presentation, destination navigation, and virtualization.
- Deferred: extend the native adapter sequence from macOS to iOS, Android, Windows, Linux, and Web.
- Deferred: add platform accessibility fixtures before advancing iOS or Web beyond technical preview.

Shared public API and Runtime changes require common tests and every affected platform build available locally.
Each native adapter is validated on its platform; unavailable platforms remain unverified.

## Invariants

- Semantics is shared Runtime output, not renderer output or platform inference.
- `SemanticFrame` is immutable, owning, pointer-free with respect to mounted state, and safe to retain.
- Role does not create an action; every advertised action has a valid Runtime route.
- Runtime hard state and secure-data policy override declarations.
- One MountedNode may own stable flat virtual semantic children without fake Views.
- Semantic identity is Runtime-local and never reused for unrelated content.
- Input focus, text-input ownership, and native accessibility focus remain distinct.
- Native objects retain SemanticFrame and SemanticNodeIds, never MountedNode or NodeExtension pointers.
- Native actions are validated against the latest committed frame.
- Visual themes do not change component semantics.
