# Semantics and Accessibility Design

Status: implemented foundation with staged shared-core completion and deferred platform coverage

This document defines the implemented platform-neutral semantics foundation and records deferred component and platform coverage explicitly.

Semantics is shared Runtime output.
Components and applications declare meaning, Runtime resolves the committed semantic hierarchy, and platform adapters expose that hierarchy through platform accessibility APIs.
Renderers do not infer semantics from pixels or PaintCommands.

## Goals

- Give built-in and custom controls one shared model for roles, names, values, states, actions, focus, collections, and geometry.
- Preserve the existing `View`, modifier, typed event, `NodeExtension`, Runtime, and PlatformAdapter boundaries.
- Publish immutable committed data that platform accessibility objects can retain safely after `BuildFrame()` returns.
- Support self-drawn composite controls without creating fake MountedNodes or platform Views.
- Keep Runtime input focus, text-input ownership, and platform accessibility focus distinct.
- Prevent TextField-owned secure content from entering committed semantics.
- Keep the shared type and action surface closed, platform-neutral, and explicit about which coverage remains deferred.

## Non-goals

The first implementation does not provide OCR, pixel-derived names, a DOM renderer, a platform View for every HuxerUI View, or platform-specific accessibility properties in shared application code.

It does not expose raw ARIA attributes, Android class names, Apple accessibility traits, UI Automation control types, or AT-SPI interface names.

PlatformView accessibility is a separate leaf-node integration.
A PlatformView supplies or bridges its platform accessibility subtree and suppresses an equivalent HuxerUI semantic subtree for that leaf.

## Ownership

| Layer | Responsibility |
|---|---|
| Components and application Views | Declare platform-neutral semantic properties |
| NodeExtension | Contribute retained state, virtual semantic children, and semantic-only behavior |
| Runtime | Resolve the hierarchy, identity, hard state, geometry, secure-data policy, and actions |
| Platform accessibility adapter | Retain `SemanticFrame`, expose platform objects, translate platform actions, and issue platform notifications |
| Renderer | Render `RenderScene`; it does not construct semantics |

The flow is:

```text
component defaults
    + NodeExtension contribution
    + application Semantics overrides
    + Runtime focus, visibility, security, and geometry
        -> SemanticFrame
        -> platform accessibility hierarchy
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

Text editing reuses the existing UTF-16 range contract, while scrolling reuses the existing controller metrics instead of introducing accessibility-only value types:

```cpp
struct ScrollMetrics {
  Axis axis = Axis::Vertical;
  float offset = 0.0F;
  float maximum_offset = 0.0F;
  float viewport_extent = 0.0F;
  float content_extent = 0.0F;
};
```

Ranges require finite values, `minimum <= maximum`, a current value inside the range, and a positive step when present.
Collection spans must be positive.
Collection and collection-item indices are zero-based; adapters convert them when a native API presents one-based positions.
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
  std::optional<TextRange> text_selection;
  std::optional<ScrollMetrics> scroll;
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

Runtime-derived enabled state, input focus, multiline editing, secure editing, actual editable selection, scroll metrics, clipping, and modal visibility are resolved from mounted behavior rather than application overrides.

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
Runtime derives ShowOnScreen only for descendants that already qualify for emission, so scroll ancestry never makes a layout-only node semantic.
Semantic order follows committed child order.
The initial API does not provide arbitrary traversal ordering.

The builder rejects duplicate extension-local child IDs, invalid child geometry, actions without a declared owner or child, and conflicting action routes as framework invariant failures.

## NodeExtension contribution

Most MountedNodes contribute at most one semantic node, but a self-drawn composite control may contribute flat virtual children.
A custom Canvas chart uses this capability when its meaningful items do not already exist as mounted Views.
Composite controls such as Tabs, NavigationBar, and NavigationPane instead publish semantics on their real retained item nodes so identity, geometry, enabled state, and activation continue to have one owner.

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
It cannot publish a frame, insert platform objects, or retain a MountedNode pointer.

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

A virtualized item that is fully evicted retires its semantic node and invalidates its action routes.
VirtualList and VirtualGrid can restore application state slots from their saved item-state cache, but the cache does not preserve mounted or semantic identity.
An item keeps its identity while its real MountedNode remains realized, including keyed movement and layout changes; returning after full eviction receives a new semantic ID.

`SemanticNodeId` is not an application key, automation identifier, platform object ID, or process-global handle.
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
  std::optional<TextRange> text_selection;
  std::optional<ScrollMetrics> scroll;
  std::optional<SemanticCollection> collection;
  std::optional<SemanticCollectionItem> collection_item;
  SemanticLiveRegion live_region = SemanticLiveRegion::None;
  bool enabled = true;
  bool focused = false;
  bool multiline = false;
  bool secure = false;
  bool offscreen = false;
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
A platform adapter retains the shared pointer for as long as platform accessibility queries may reference it.

## PlatformView semantic bridge

A PlatformView contributes one semantic anchor at its mounted position rather than converting its platform accessibility descendants into SemanticNodes.
An optional Runtime-owned platform-view identity marks anchor nodes in `SemanticFrame`; applications cannot supply it through the `Semantics` modifier.
The anchor carries the same stable PlatformView identity as `PlacePlatformViewCommand`, while its parent and sibling position come from ordinary semantic resolution.
Visual `RenderComposition` order and accessibility traversal are derived from the same committed mounted tree but remain distinct outputs: paint-only decoration does not become accessible merely because it occupies a later render slice.

The platform accessibility adapter resolves the anchor identity against the PlatformView instance from the same committed frame and exposes that platform object's accessibility root at the anchor position.
The anchor is a structural substitution point rather than an additional generic accessible object, so assistive technology encounters the platform root once.
It suppresses semantic descendants that would duplicate the platform subtree, but HuxerUI semantic siblings before and after the anchor remain in their declared order.
The bridge does not copy platform labels, actions, selection, or editable content into shared Runtime state.
Accessibility queries and actions inside the subtree remain owned by the platform object, while traversal into or out of the subtree returns through the HuxerUI anchor.
PlatformPayload events notify typed application EventBindings and do not substitute for platform accessibility queries or actions.
Conversely, an accessibility action inside the platform subtree is not mirrored as a platform-module event unless the platform component independently emits that documented application event.

Applying a new frame updates composition and semantic bridge references before issuing accessibility structure notifications.
Replacement or removal first makes the anchor unavailable to new queries, then invalidates retained platform accessibility wrappers, and only then destroys the PlatformView instance.
A stale query fails safely against the newest committed identity instead of dereferencing a removed platform object.

Android exposes the PlatformView as a real accessible child alongside provider-backed HuxerUI virtual nodes and preserves the anchor's sibling position.
UIKit and AppKit insert the platform accessibility root into their retained container-child order at the anchor.
Windows bridges a child HWND or provider fragment root at the matching UI Automation position.
Web uses the real PlatformView DOM subtree at the corresponding semantic DOM position and does not create a duplicate hidden element for the anchor.

Runtime increments the nonzero revision and creates a new `SemanticFrame` only when semantic content, structure, focus, or geometry changes.
A color-only render frame reuses the previous semantic frame.

The first implementation does not publish a separate change-set type.
An adapter compares retained frames internally when its platform notification API benefits from finer updates.

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
Their role and state let a platform adapter expose the appropriate native toggle or selection pattern, while the explicitly advertised Activate action proves that Runtime behavior exists.

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
TextField editing routes through its retained extension, while scrolling and ShowOnScreen use generic mounted scrolling capability.
Expand, Collapse, and Dismiss remain extension-owned because the retained component or presentation service owns the corresponding state transition.

## Geometry

Node geometry uses host-view logical coordinates after final layout offsets and presentation transforms.
Non-axis-aligned geometry is represented by a conservative axis-aligned bounds rectangle.
The frame publishes full node bounds and an `offscreen` result derived from the host viewport, ancestor scroll viewports, and rectangular `ClipChildren` intersections.
Partially clipped nodes retain their full stable bounds; the public snapshot does not add a second `visible_bounds` rectangle without a demonstrated native requirement.
ShowOnScreen uses retained Runtime geometry rather than reconstructing a target from the public snapshot.

## Focus

Runtime input focus is committed as node state.
The Focus action follows the same focus path used by keyboard and pointer input and starts TextInput only when the target owns a TextInputClient.

Platform accessibility focus remains platform state.
Moving VoiceOver, TalkBack, Narrator, or another screen reader to a semantic node does not emit `ViewEvents::FocusChanged`, alter keyboard traversal, or start text input.

Semantic modal isolation derives from the same topmost retained Layer focus trap used by keyboard and pointer focus.
Application content and lower layers outside that trap are excluded from accessible navigation, while higher notification and system layers may remain available for live announcements.
There is no author-settable modal boolean that can contradict Runtime behavior.

Presentation content declares semantics on its real visual root rather than the viewport-sized Layer entry.
Dialog and BottomSheet surfaces therefore publish their own bounds, while the barrier remains a visual and pointer concern.
When a focus trap belongs to a menu chain, the chain's active layers share one implementation-only modal-group identity so the parent Menu and its open submenus remain in the same semantic modal region.
The identity is a fieldless, strongly typed `SemanticModalGroupToken`; it carries no role, action, callback, menu state, or general Layer metadata.

Layer dismissal has one internal request path.
Outside presses, Back or Cancel, and semantic Dismiss actions ask the LayerController to dismiss an entry; the controller invokes `on_dismiss_request` when present and otherwise performs the dismissal itself.
`Dismiss()` remains the unconditional command used after controlled state has accepted the request.
A role never implies this behavior: a presentation surface advertises Dismiss only when its existing policy allows that request and a retained action route is attached.

An exiting Layer may remain in the render tree until its exit motion completes, but its content immediately stops receiving input, owning focus, and contributing semantics.
A modal interaction barrier remains until removal so pointer and keyboard input cannot fall through the still-visible presentation, while semantic modal isolation advances to the next active region immediately.
This keeps paint lifetime independent from content interaction and accessibility lifetime without exposing the application through a fading modal.
Runtime resolves this policy from the Layer metadata committed on the mounted entry rather than re-reading newer LayerController state during semantic construction.
If exit completion mutates the controller while a frame is building, the retained node therefore keeps the semantics of the snapshot that produced it until the next LayerStack reconciliation.

## Text editing and secure values

TextField contributes its role, label, nonsecure value, placeholder, validation state, read-only state, multiline state, secure state, Runtime input focus, and nonsecure UTF-16 selection.
An editable TextField advertises SetText, and a nonsecure TextField advertises SetSelection even when it is read-only.
The retained TextField extension handles both actions through the existing reducer, validation, length limit, history, and controlled change event rather than adding another editing protocol.

SetText replaces the complete value atomically, clears composition, and leaves the caret at the new UTF-16 end.
SetSelection accepts a normalized TextRange and resolves it to a downstream TextSelection after validating text boundaries.
An invalid UTF-8 value, out-of-range offset, surrogate split, disabled field, or unsupported secure operation returns false without partial mutation.

Runtime observes TextInputClient state before and after a semantic edit and applies the same state validation, invalidation, and active-session synchronization rules used by native command batches.
Content revisions invalidate layout, paint, semantics, and active native input state; selection-only revisions invalidate foreground paint, semantics, and active native input state without requiring a new input session.

An ordinary TextField publishes its committed value and normalized selection for platform accessibility editing.
Composition details remain in the existing bounded TextInputClient query path.

A secure TextField frame never contains TextField-owned plaintext, selected text, surrounding text, composition text, clipboard content, or plaintext-derived state descriptions.
Runtime commits protected state without a value, selection, or protected length.
An editable secure field may accept SetText, but it does not advertise SetSelection.
Copy and Cut remain unavailable through the existing editing policy.

Application-authored labels, hints, errors, and identifiers are trusted metadata and must not copy the protected value.
Platform adapters do not reconstruct content from mask PaintCommands or add another text query path.

The complete editing and security contract remains in [Text Input and TextField Design](text-input.md).

## Components and virtualization

The shared component contract is listed below.
The delivery-status section distinguishes implemented defaults from defaults completed by the staged work in this document.

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
| SegmentedButton | Horizontal collection containing labeled, checked, selected RadioButton items and Activate |
| ProgressCircle and ProgressBar | ProgressIndicator role, normalized range when determinate, or localized busy state when indeterminate |
| Slider | Slider role, range, SetValue, Increment, and Decrement |
| TextField | TextField or author-overridden SearchField role, value, UTF-16 selection, editing actions, and secure redaction |
| Tabs | TabList collection containing labeled, selected, enabled Tab items and Activate |
| NavigationBar | Navigation collection containing labeled, selected, enabled Button items and Activate |
| NavigationPane | The same Navigation collection in compact and expanded visual modes |
| ScrollView | ScrollView role, current ScrollMetrics, Scroll, and descendant ShowOnScreen |
| Dialog and BottomSheet | Dialog role, modal isolation, descendants, and Dismiss when allowed |
| Menu | Menu collection containing labeled, checked, expanded, enabled MenuItem nodes |
| Toast | Non-focusable polite live region containing its message |
| VirtualList | List collection with total count and realized item indices; a transparent item root defaults to ListItem |
| VirtualGrid | Grid collection with total count and realized item positions and spans; a transparent item root defaults to GridCell |
| Canvas | No inferred semantics; explicit owner semantics or virtual children |

Icon-only item constructors continue to require their existing semantic label.
Material, Flat, and third-party visual themes do not change component semantics.

Popup keeps the semantics of its supplied content because the presentation mechanism does not imply a shared role.
Virtual containers publish only realized retained items and never materialize every View for accessibility; scrolling advances the realized semantic window.
Cached items outside the viewport remain published with `offscreen = true` and reuse the existing ShowOnScreen action while they remain mounted.
An item root that already owns a meaningful role such as Button or Checkbox keeps that role and receives collection-item metadata; Runtime supplies ListItem or GridCell only when the item root has no component role.
Future component defaults use the same owner/real-child and retained action-routing contracts rather than adding component-specific Runtime branches.

### Virtual collection layout contract

Collection dimensions and item positions are layout facts.
VirtualList and VirtualGrid therefore publish them through the same `VirtualLayoutResult` that atomically commits realization and placement rather than through a second semantic layout pass:

```cpp
result.SetCollectionSemantics(
    SemanticRole::Grid,
    SemanticRole::GridCell,
    SemanticCollection{
        .item_count = item_count,
        .row_count = row_count,
        .column_count = column_count,
    }
);

result.Place(
    item,
    offset,
    SemanticCollectionItem{
        .index = index,
        .row_index = row,
        .column_index = column,
        .row_span = 1,
        .column_span = span,
    }
);
```

The two-argument `Place(item, offset)` remains the ordinary non-collection or decorative-item path.
A custom VirtualLayout can opt into the same collection contract without adding a component-specific Runtime branch.

Runtime retains the collection declaration and realized item metadata with the committed virtual placements.
During semantic construction it applies the collection role and item role as component defaults, attaches structural collection metadata to the corresponding direct item root, and then applies extension and author semantics with their ordinary precedence.
This creates no wrapper View, fake MountedNode, accessibility-only item cache, or platform-specific collection model.

A vertical VirtualList publishes one column and one row per source item; a horizontal VirtualList publishes one row and one column per source item.
VirtualGrid reuses its existing row plan, resolved column count, and column spans.
The collection's item count describes the complete logical source, while the semantic tree contains only the current viewport and cache realization.

Dialog and BottomSheet attach their default role to the presented surface instead of the Layer barrier.
Standard dialog actions are real Button semantic nodes, while custom content keeps its descendants and may override component defaults with the ordinary `Semantics` modifier.
Dialog Dismiss follows `dismiss_on_cancel`; BottomSheet also exposes Dismiss when its themed drag handle provides an existing dismissal path.

Menu separators are decorative and do not count as collection items.
Each real item node owns its label, zero-based collection index, enabled state, optional checked state, and optional submenu expansion state, and excludes its decorative icon, checkmark, label Text, and arrow descendants.
Not calling `Checked()` means that checked state is unsupported, while `Checked(false)` and `Checked(true)` publish Unchecked and Checked respectively.
Submenu items advertise Expand or Collapse through a retained action extension that uses the existing MenuChainState and anchored-layer path.

Toast applies `SemanticLiveRegion::Polite` directly to its message Text.
It does not create a Toast role, focus target, action, or announcement service.

## Platform mapping

Each adapter retains the newest `SemanticFrame`; adapters with retained platform accessibility objects cache them by SemanticNodeId.
Platform objects never retain MountedNode or NodeExtension pointers.

The Android, iOS, macOS, and Windows bridges are implemented.
The remaining platform subsections define the intended adapter boundary, not current support.

### Android

`HuxerUIView` exposes virtual descendants through `AccessibilityNodeProvider`.
The shared semantic root maps to `AccessibilityNodeProvider.HOST_VIEW_ID`, while every non-root SemanticNodeId is converted exactly to a positive 32-bit virtual View ID.
The Android encoder rejects an identity above `jint` maximum instead of truncating it, and Android actions convert the virtual View ID directly back to SemanticNodeId for validation against the newest Runtime frame.
This removes a second identity allocator, bidirectional maps, reuse policy, and retained mapping growth from the ordinary node path.
Custom semantic action IDs remain a separate 64-bit namespace and receive stable provider-local Android action IDs for the host View lifetime.

The Android frame commit returns a versioned binary semantic snapshot only when the SemanticFrame revision changes.
`HuxerUIAccessibilityProvider` decodes that snapshot into immutable Java-owned nodes, answers hierarchy, text search, focus, geometry, state, collection, range, and action queries without JNI, and calls native code only to perform an action.
The synthetic root contributes its children to the host View rather than appearing as another virtual descendant.
Bounds remain logical and View-local in the shared frame; the provider converts them to parent-relative pixels and applies the complete View-to-global matrix for screen bounds.

A PlatformView identity travels in the same version-one node record as its semantic anchor.
The provider omits that virtual anchor and adds the corresponding real `PlatformViewContainer` alongside virtual children in the declared sibling order.
The container is a non-focusable structural accessibility node whose parent points to the host or the matching virtual semantic parent; its Android View content retains its own accessibility behavior and descendants.
Provider touch exploration yields to a frontmost PlatformView subtree, and mount, removal, or visibility changes publish one host-subtree change after the frame has synchronized platform accessibility parents.

Roles map to the closest Android widget class, while checked, selected, expanded, editable, secure, range, collection, heading, live-region, invalid, and scrolling state use the corresponding AccessibilityNodeInfo contracts available on the current API level.
Collections containing RadioButton children map to Android single-selection collections without adding a platform role to the shared semantic model.
Secure fields never publish text or selection.
Activate, Focus, SetText, SetSelection, SetValue, Increment, Decrement, Scroll, ShowOnScreen, Expand, Collapse, Dismiss, and Custom actions return through `Runtime::PerformSemanticAction()` on the Android UI thread.
Accessibility focus and explore-by-touch hover stay provider-owned; Android input-focus requests call Runtime Focus and remain distinct from TalkBack focus.

Committed-frame diffs emit subtree, focus, selection, text, text-selection, scroll, state, dialog, and live-region events.
The provider retains the newest frame even while accessibility is disabled so enabling TalkBack does not require rebuilding shared semantics.
Detaching the HuxerUIView clears provider focus, hover, custom-action, and snapshot state together with the platform session.

### iOS

The `HuxerUIView` host is neither an accessibility element nor a text-input responder and exposes an ordered `UIAccessibilityContainer` hierarchy.
The bridge retains private UIKit elements by SemanticNodeId so compatible recomposition, geometry changes, and reparenting update UIKit properties without replacing a surviving VoiceOver target.
Private non-element containers represent semantic groups only when UIKit gains structure from them, including collections, tab lists, lists, grids, grid cells, navigation landmarks, menus, and dialogs.
ScrollView remains transparent in the UIKit hierarchy: its descendants attach to the nearest UIKit container while scrolling still resolves through the committed semantic parent chain.
Focus on a structural container preserves the shared keyboard route without creating a separate spoken element; direct activation, expansion, collapse, or custom actions still create an element before the container's children.
If a semantic node both owns an actionable element and contains descendants, the private container contains that one element followed by its semantic children; no spoken node is duplicated.

The host maps Text and Heading to static text and header traits, Button and Link to their direct traits, Image to image, SearchField to search field, Slider to adjustable, and disabled or selected state to the corresponding UIKit traits.
ShowOnScreen remains a navigation capability and does not by itself make a UIKit element report that it responds to user interaction.
Checkbox and Switch use the UIKit toggle trait when available and retain an iOS 13 fallback with button behavior and a checked value; RadioButton and Tab use button and selected behavior without treating checked state as an unrelated selection.
TabList uses the non-element tab-bar contract.
List and Navigation map to UIKit list and landmark container types, while Grid adopts the data-table protocols only when every logical item is represented by a queryable committed cell and otherwise remains a semantic group.
Label, value, placeholder fallback, hint, error, identifier, range, and geometry come from the retained committed frame.
Secure TextField content, selection, and protected length remain absent rather than being reconstructed as masking characters.

Activate, Focus, Increment, Decrement, Scroll, ShowOnScreen, Expand, Collapse, Dismiss, and labeled custom actions return to `Runtime::PerformSemanticAction()` on the main thread.
The default VoiceOver activation prefers Activate, otherwise chooses the currently valid Expand or Collapse action, and finally uses Focus for focusable fields.
Adjustable callbacks route Increment and Decrement, the two-finger escape gesture routes Dismiss, and scrolling uses the committed axis and viewport extent of the nearest direction-compatible semantic scroll ancestor.
SetText and SetSelection are not duplicated as custom accessibility actions: activating a TextField establishes the existing Runtime focus and a private non-accessible `UITextInput` view remains the only UIKit editing service.
On iOS 18.1 and later, only the Runtime-focused TextField or SearchField exposes that view through `accessibilityTextInputResponder`; earlier versions still activate the same Runtime session and private first responder without making the application accessibility container a text input.

VoiceOver focus stays UIKit-owned and never becomes Runtime input focus merely because an accessibility element became focused.
An offscreen focused element may request ShowOnScreen without changing input focus.
The bridge preserves the current UIKit element when its SemanticNodeId survives and issues conservative layout notifications only when the accessible hierarchy, role, collection structure, or PlatformView subtree changes.
If the focused element disappears, UIKit remains responsible for choosing the next target from the new committed order; the bridge does not impose a platform-independent fallback.
Completed semantic scrolling emits a page-scrolled notification only after a later frame confirms the offset change.
Live-region announcements are diffed from committed frames, coalesced per commit, and use queued polite speech or interrupting assertive speech according to the APIs available on the deployment version.

`UIKitPlatformViews` resolves a PlatformView semantic anchor to the registered `UIView` after PlatformView composition has committed.
The bridge inserts that view at the anchor's semantic sibling position and does not wrap or copy its UIView accessibility subtree.
HuxerUI slice and clipping views remain non-elements, while the PlatformView continues to own its labels, descendants, editing behavior, and actions.
The iOS frame transaction commits PlatformViews before accessibility and accessibility before paint invalidation so every platform query observes one coherent frame.

### macOS

The AppKit host currently exposes retained `NSAccessibilityElement` children with mapped roles, labels, basic values, hints, enabled, selected, and focused state, hierarchy, screen geometry, and press or range actions from the semantic frame.
It preserves mixed checked state and emits separate structure, title, value, and focus notifications by comparing retained frames.
For a PlatformView anchor, it resolves the committed identity through the AppKit host and substitutes the unignored NSView accessibility root at the anchor's sibling position instead of creating a duplicate `NSAccessibilityElement`.
The focused AppKit property represents keyboard focus and may route a Runtime Focus action; the VoiceOver cursor remains AppKit-owned and is not committed as Runtime input focus.
AppKit calls and Runtime actions remain on the main thread.
`NSTextInputClient` continues to own IME communication.

### Windows

The HWND exposes a UI Automation fragment root with cached providers keyed by SemanticNodeId.
Providers supply only the control patterns supported by committed role, state, and actions.
Each provider freezes its COM interface set when created; a changed pattern shape replaces the cached provider while preserving the semantic RuntimeId.
The initial mapping includes Invoke, Toggle, Value, RangeValue, Selection, SelectionItem, ExpandCollapse, ScrollItem, and Scroll.
It publishes stable runtime IDs, hierarchy, screen geometry, names, identifiers, state, collection position, live regions, and committed property, focus, selection, layout, and structure changes.

UI Automation may query off-thread.
Providers answer read-only queries from a retained immutable `SemanticFrame` and marshal actions to the HWND thread before calling Runtime.
Provider queries retain the owning frame while using a node pointer and never copy or retain mounted Runtime objects.
Secure fields advertise password state but reject Value reads instead of exposing either their contents or an ambiguous empty value.
TextField currently exposes Value rather than TextPattern because the semantic frame does not yet publish the native text-range geometry required for a correct `ITextRangeProvider` implementation.

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
Runtime owns TextInputClient mutation finalization so semantic editing cannot bypass layout, foreground paint, active native input, or semantic invalidation.
Scroll offset changes update scroll metrics, offscreen state, and realized virtual items in the same committed frame.
An extension changing unrelated paint and semantics requests both explicitly.

Runtime builds semantics after final presentation geometry and text-input session refresh and before returning `FrameCommit`.
It does not call platform accessibility APIs while building.

The initial Runtime always produces semantics.
There is no enablement API or assistive-technology detection race.
Unchanged frames reuse the immutable shared object, and additional caching is added only after profiling demonstrates a need.

## Threading

Runtime construction and action dispatch run on the Runtime UI thread.
NodeExtension semantic callbacks do not run concurrently with reconciliation, frame construction, or unmount.

Native read-only queries use a retained immutable frame and do not call Runtime for names, children, states, or geometry.
Platform actions arriving during frame construction are queued or marshaled and cannot re-enter `BuildFrame()`.

## Validation

Caller-supplied invalid ranges, spans, heading levels, virtual-child bounds, and custom action labels throw `std::invalid_argument` with an English HuxerUI diagnostic.
Duplicate local IDs, actions without a declared semantic item, and conflicting extension routes throw `std::logic_error`; stale platform actions return false.
Broader role-state and automation-identifier diagnostics remain deferred.

## Testing

Current shared tests cover:

- Public-header self-containment and umbrella export.
- Built-in basics, progress state, modifier precedence, explicit empty declarations, hidden nodes, descendant exclusion, and disabled actions.
- Compatible updates, replacement, unmount through replacement, identity stability, revision changes, focus, and stale action rejection.
- Virtual child identity, action routing, extension replacement, stale-route rejection, and identity retirement.
- Slider range actions, invalid payload rejection, shared value validation, and secure TextField redaction.
- Tabs, NavigationBar, and NavigationPane hierarchy, selection, disabled items, activation, identity stability, and compact or expanded visual modes.

The shared-core completion adds focused coverage in this order:

- TextField value, UTF-16 selection, SetText, SetSelection, secure and read-only policy, controlled replacement, active input synchronization, and stale actions.
- Scroll metrics, nested Scroll, ShowOnScreen, clipping, transforms, and offscreen changes.
- Dialog and BottomSheet modal isolation and dismissal, Menu collections and submenu expansion, Toast live-region lifecycle, and exiting layers.
- VirtualList and VirtualGrid counts, realized item metadata, scrolling, cache eviction, and semantic identity.

Focused Android codec coverage verifies deterministic snapshots, direct virtual IDs, UTF-8 content, and overflow rejection.
Focused Windows provider fixtures cover properties, stable fragment identity, static COM interfaces, provider-shape replacement, navigation, hit testing, pattern selection, secure-value rejection, scroll boundaries, and Runtime action routing.
The iOS bridge compiles against the iOS 13 Simulator boundary.
Physical-device VoiceOver validation covers primary ui_gallery traversal, shared controls, text input, and PlatformView substitution.
Broader manual coverage for modal isolation, scrolling, live regions, and less common actions remains ongoing.
Dedicated macOS accessibility fixtures and manual screen-reader validation remain deferred.
Manual validation uses the native screen readers and accessibility inspectors available on each platform.
Unavailable platforms and tools remain explicitly unverified.

## Shared-core completion sequence

The shared work is delivered in bounded stages so each contract is validated before platform adapters depend on it:

- Completed: accessible text editing adds `text_selection` and completes TextField actions without changing the TextInputClient protocol.
- Completed: scrolling and visibility extend ScrollMetrics with Axis, publish Scroll and ShowOnScreen, and compute offscreen without a second public bounds rectangle.
- Completed: presentation semantics derive Dialog, Menu, Toast, dismissal, live regions, and modal isolation from existing Layer ownership.
- Completed: collection semantics derive VirtualList and VirtualGrid metadata from the committed VirtualLayoutResult without eagerly composing unrealized content.

After these stages, the shared core answers native read-only queries and routes every advertised action without platform inference.
Android AccessibilityNodeProvider, UIKit, AppKit, and Windows UI Automation now consume that contract; Linux and Web mappings follow according to platform readiness.

## Delivery status

- Public value types, the `Semantics` modifier, `SemanticFrame`, Runtime-owned stable identity, immutable-frame reuse, secure TextField redaction, TextField value and editing actions, generic scrolling and visibility actions, virtual collection metadata, basic action routing, NodeExtension virtual children, destination-selection semantics, PlatformView semantic anchors, the Android AccessibilityNodeProvider bridge, the iOS UIKit bridge, the macOS AppKit bridge including native anchor substitution, and the Windows UI Automation bridge are implemented.
- Deferred: extend the platform adapter sequence to Linux and Web, and add Windows TextPattern after the shared text-range geometry contract exists.
- Deferred: add platform accessibility fixtures before advancing Web beyond technical preview.

Shared public API and Runtime changes require common tests and every affected platform build available locally.
Each platform adapter is validated on its platform; unavailable platforms remain unverified.

## Invariants

- Semantics is shared Runtime output, not renderer output or platform inference.
- `SemanticFrame` is immutable, owning, pointer-free with respect to mounted state, and safe to retain.
- Role does not create an action; every advertised action has a valid Runtime route.
- Runtime hard state and secure-data policy override declarations.
- Secure semantic frames never expose TextField-owned plaintext, selection, composition, or protected length.
- Semantic text editing uses the existing retained client and controlled change path rather than a second editor state.
- Scroll and ShowOnScreen use existing mounted scrolling capability rather than component branches.
- Modal accessibility derives from retained Layer focus trapping, and virtual collection semantics never force eager View materialization.
- One MountedNode may own stable flat virtual semantic children without fake Views.
- Semantic identity is Runtime-local and never reused for unrelated content.
- Input focus, text-input ownership, and platform accessibility focus remain distinct.
- Platform objects retain SemanticFrame and SemanticNodeIds, never MountedNode or NodeExtension pointers.
- Platform actions are validated against the latest committed frame.
- Visual themes do not change component semantics.
