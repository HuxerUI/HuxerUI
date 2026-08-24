# View Composition and Environment Design

Status: implemented

This document defines the boundary between immediate View declaration, deferred scope execution, Environment propagation, ViewSpec compilation, and MountedNode reconciliation.
Direct Environment and Theme content, mounted Environment propagation, and deferred ViewSpec compilation are the intended contract.
The current implementation uses the precise dependency model in this document and does not retain the former broad mounted-context invalidation path.
The public `[[huxerui::composable]]` marker and direct `UseXxx()` validation implement the composition boundary described below.

## Goals

- Keep ordinary HuxerUI syntax as immediate C++ execution that produces transient View values.
- Make ViewSpec the only declaration-level intermediate representation.
- Keep ViewSpec independent of the Environment in which it happens to be constructed.
- Defer only code that genuinely requires a composition lifetime or ambient values.
- Compile Theme, resources, and other inherited values while reconciling a ViewSpec into its mounted context.
- Let nested Environment providers accept ordinary View content without requiring a factory solely for ambient propagation.
- Preserve incremental reconciliation, measurement, layout, paint, semantics, and retained extension behavior.
- Keep one Runtime, one mounted tree, and one rendering pipeline without introducing a second resolved tree or a public Component abstraction.

## Non-goals

- It does not introduce a public Component type, a virtual-item Composer, or a second state model.
- It does not make every View constructor or primitive carry a deferred callable.
- It does not compile declarations during every frame, measurement, layout, or paint pass.

## Terminology

`View` is the public transient copy-on-write value used by the DSL.

`ViewSpec` is the private declaration data owned by a View.
It is the sole View intermediate representation and contains authored structure and configuration rather than mounted or Environment-derived state.

A composable is a function whose body must execute inside a RecomposeScope because it reads composition state, ambient values, lifecycle facilities, tasks, or root services.

Compilation is the local operation that interprets one ViewSpec under an effective mounted Environment and produces the final values needed to compare and update one MountedNode.
It is part of reconciliation rather than a separate tree-building phase, and its temporary result is not retained as another tree.

## Execution model

The public DSL remains immediate:

```cpp
View Toolbar() {
  return Row {
    Text(strings::title),
    Button("Save"),
  }.With(Spacing(8.0F));
}
```

Calling `Toolbar()` immediately executes the function and constructs a ViewSpec tree.
Constructing `Text`, `Button`, `Row`, or applying `.With(...)` also immediately records declaration data.
Application callbacks stored by those declarations are not invoked during construction.

Only a composable body is deferred:

```cpp
[[huxerui::composable]]
View Counter() {
  auto count = UseState(0);

  return Button(std::to_string(count)).OnClick([count] {
    ++count;
  });
}
```

The generated `Counter()` function immediately returns a Scope View whose factory contains the original body.
Runtime invokes that body only when it mounts or recomposes the Scope under an active Composer and effective Environment.

The complete path is:

```text
View DSL -> ViewSpec -> Runtime tree update
                           |- execute or recompose scopes
                           |- propagate Environment
                           |- compile ViewSpec values
                           `- reconcile MountedNode
                         -> measure -> layout -> paint -> RenderScene
```

Runtime tree update interleaves these operations according to tree structure.
There is no global compose pass that must finish before Environment propagation and no global compilation pass that produces another tree.
When Runtime encounters a Scope it executes that Scope, obtains its ViewSpec result, and continues reconciliation in the current Environment.
When Runtime encounters an Environment boundary it selects the child mounted Environment before compiling and reconciling that subtree.

## Why composition does not compile ViewSpec

Composition owns execution lifetime and dependency tracking, while compilation owns interpretation of a declaration in its mounted context.
Combining them would make the result depend on where the ViewSpec happened to be constructed rather than where it is mounted.

Not every ViewSpec is created by a Scope.
Ordinary helpers, retained values, virtual item declarations, and presentation content may construct a View before Runtime enters the eventual parent Environment.
A composed subtree may also contain a nested Theme or ProvideEnvironment boundary that changes the correct context partway through traversal.

The result of executing a Scope is therefore an unresolved ViewSpec tree.
Runtime consumes that result immediately during the same tree update, but compiles each node only after it has traversed the Environment boundaries that apply to that node.
This keeps Scope dependency tracking precise without creating a second declaration object or retaining a separate composed tree.

## View and ViewSpec ownership

View remains a transient copy-on-write handle around a private ViewSpec.
It may be created outside an active Composer and moved through ordinary C++ code.
Its meaning must not depend on the thread-local Environment that happened to be active at construction time.

ViewSpec retains authored declaration data:

- View kind and stable key.
- Child View declarations.
- Scope factory for genuinely deferred composable execution.
- Canvas painter, layout descriptor, virtual source, and PlatformView declaration.
- Typed event and activation declarations.
- Retained modifier declarations.
- User-authored layout values.
- Enabled, focusable, and pointer policy declarations.
- Raw component inputs such as StringVariant, image sources, and component configuration.
- Ordered generic property modifier declarations.

ViewSpec does not retain mounted or Environment-derived data:

- A captured Environment.
- Theme defaults or resolved component styles.
- Localized strings.
- Density, locale, or platform-selected image assets.
- A resolved default indication.
- Reduced-motion state.
- Layout results, paint records, interaction state, or animation state.

View construction must therefore stop capturing `CurrentEnvironment()` and stop applying Theme defaults.
The same ViewSpec can be reconciled under different nested Themes without being copied, rebuilt, or mutated for each Theme.

## Authored properties and final properties

Generic property modifiers are recorded immediately as one left-to-right property-modifier sequence without mutating final ViewProperties.
Reconciliation applies that sequence once after component defaults and component declarations, so a later modifier continues to win without per-field override flags, optional mirrors, or property masks.
An explicit false, zero, transparent color, empty inset, or default-looking enum remains explicit because the operation itself is retained.
Environment lookup, ordered composition, and complete modifier validation therefore share one authoritative ViewSpec compilation path.

Reconciliation resolves final properties with this precedence:

```text
generic initial values
  -> component Theme/default projection using component declaration configuration
  -> ordered property modifiers
```

Each primitive factory supplies one internal component-default operation rather than inserting a synthetic default modifier or relying on a Runtime NodeKind switch.
Component-specific fluent declarations are recorded separately from generic `.With(...)` modifiers and can parameterize that component's default projection.
`.LayoutValue(...)` remains a distinct parent-child metadata channel rather than another ViewProperties precedence layer.
Component-owned layout configuration may be completed with resolved Theme values during its default projection, while unrelated declared layout values pass through unchanged.
ViewSpec compilation starts with fresh generic defaults and applies those channels in the order above.
The resulting ViewProperties stored on MountedNode remain the authoritative final generic properties, and the temporary compiled ViewSpec is discarded after comparison and commit.

Property modifiers remain declaration data but do not create MountedNode extensions or retained runtime state.
Retained modifiers remain raw ordered declarations that reconcile to persistent NodeExtension objects.
An operation such as `AddDefaultIndication()` records a semantic request in ViewSpec; reconciliation chooses the actual Theme indication under the effective Environment.
An explicit indication remains explicit and overrides the default request.

`CompileViewSpec` owns one ordered modifier-compilation pass.
Each type-erased ModifierDescriptor may provide one internal compile operation that applies a property declaration, replaces a retained declaration value with its resource-resolved value, or does both.
Ordinary retained modifiers require no compile operation and proceed unchanged to NodeExtension reconciliation.
There is no separate modifier-value resolution pass and no implicit convention based on a modifier type exposing a specially named static function.
TextField strings and icons, Checkbox checkmarks, and Indication resources use this same local compilation pass before any mounted state changes.
Descriptor equality and layout equality subsequently compare those compiled values, so an Environment change that produces the same final value does not update the retained extension.

The internal entry point is named `CompileViewSpec` rather than introducing a Compiler object or a public compiled configuration type.
Ordinary custom components continue to compose public primitives and modifiers; this phase does not expose a callback that can mutate private ViewSpec state.
A custom function that directly reads Theme or another ambient value must be marked composable or use an explicit Scope.

## Reconciliation and commit behavior

Mount and reconciliation receive the effective `std::shared_ptr<const Environment>` explicitly.
They do not read a construction-time Environment from ViewSpec.

For each ViewSpec, Runtime computes a local compiled declaration before mutating the MountedNode:

- Final ViewProperties.
- Resolved text and text layout inputs.
- Resolved image properties and selected assets.
- Resolved component styles and indications.
- Final layout values.
- Final semantics.
- Retained modifier updates.
- Reduced-motion and other mounted Environment projections.

Only after local compilation succeeds does reconciliation compare and apply those values.
A compilation exception therefore leaves that ViewSpec's previously committed MountedNode values intact rather than partially applying a new Theme or resource context to the failing node.
Resource-bearing retained modifiers validate and resolve their declaration values during this local compilation.
Their NodeExtension create and update operations receive only resource-resolved values and do not perform Environment or resource lookup after MountedNode commit has started.
Validation that depends on retained interaction state remains owned by the NodeExtension.

Component-specific defaults stay with the module that owns the component.
Runtime must not gain branches for Button, Slider, TextField, NavigationPane, or other concrete controls.
Each primitive declaration supplies its own internal defaults operation; shared reconciliation only invokes the operation stored by the declaration.

Component defaults are applied only to that local compiled declaration during reconciliation.
Component-owned declaration configuration can be completed by that operation, and ordered property modifiers apply last to generic ViewProperties.
This operation reads a ViewSpec plus Environment and returns the final current-node values.
It must not mutate ViewSpec.
Invalid combinations that depend on preceding modifiers or resolved defaults propagate from `BuildFrame()` before the declaration is committed.

## Incremental invalidation

An Environment change does not automatically mark every descendant for measurement, layout, and paint.
It invalidates only RecomposeScopes that observed the changed typed entry; their declarations are then compiled again during ordinary reconciliation.
Runtime then compares final values and projects only real differences into existing dirty domains:

- Geometry-affecting differences invalidate measurement or layout.
- Content visual differences invalidate content paint.
- Indication or foreground differences invalidate foreground paint.
- Semantic differences invalidate semantics.
- Retained modifier configuration differences update compatible NodeExtension instances.

If two Themes resolve a particular node to equal final values, that node remains clean.
Compilation occurs on mount and compatible reconciliation, including reconciliation reached after an Environment dependency changes, not on every animation frame or paint traversal.

## Environment boundaries

Environment propagation uses an internal transparent node kind rather than a synthetic Scope.
An Environment boundary owns one ordinary content child, one local Environment declaration, and one stable mounted `Environment` object.
It has no independent RecomposeScope, State slots, factory, paint, semantics, hit-test surface, or layout geometry.

The public forms accept ordinary View content:

```cpp
return ProvideEnvironment(
    GreetingLocale{"fr"},
    Column {
      Text(strings::greeting),
      GreetingDetails(),
    }
);
```

The content ViewSpec may already exist when the boundary is declared because its semantic values do not capture an Environment.
When Runtime reaches the boundary, it uses the boundary's mounted Environment for ViewSpec compilation and for the ordinary child subtree.

An Environment used as a public value is a transient typed declaration bag with no parent or Runtime identity.
Runtime mounts that declaration into a stable `std::shared_ptr<Environment>`, installs its inherited parent, and never replaces the mounted object during compatible reconciliation.
The private Environment entry for each type contains an optional local value, its equality operation, and a shared composition dependency.
Copying an Environment copies only its declaration values and equality operations; it does not copy the mounted parent, dependency identities, or subscribers.

`UseEnvironment<T>()` walks from the current mounted Environment toward the root, observes the `T` entry at every visited Environment, and returns the first local value found.
If no Environment supplies `T`, it returns `T::Default()` after observing the absent entries along the complete path.
Observed entries remain allocated when a local value is removed, so their dependency identity stays stable.
Observing absent entries makes insertion of a nearer override visible, while stopping at the first present entry prevents an outer value from invalidating readers hidden by an inner override.

Environment dependency tracking reuses the existing RecomposeScope graph.
The common internal CompositionDependency owns subscribers, and both StateCellBase and private Environment entries use it.
RecomposeScope records dependencies without knowing their concrete source.
Environment does not add an observer tree, global registry, Runtime subclass, descendant walk, or Environment-specific revision.
The Composer that owns a declaration remains active through child reconciliation and ViewSpec compilation, so ambient reads performed while compiling direct children are attributed to the nearest real RecomposeScope.
A nested Scope installs its own Composer for its subtree and restores the parent Composer when it finishes.

Precise equality is part of the reactive value contract.
EnvironmentValue therefore requires copy construction, `Default()`, and equality comparison.
`Environment::Set<T>()` stores the type-erased equality operation for `T`, and compatible provider reconciliation compares only entries present in the changed local declaration.
It does not compare complete Environment chains or increment an Environment revision.
Root services are immutable startup entries rather than reactive EnvironmentValue declarations and do not require service-object equality.

Environment updates use a Runtime-owned transaction limited to one stable mounted Environment object.
Changed parent and entry values are staged without notifying subscribers while the Environment node itself is compiled.
Failure of that local compilation restores the staged Environment values and discards pending notifications.
Success commits the Environment before descendant reconciliation and invalidates subscribers other than the Scope already composing the update.
A later descendant failure does not roll back the committed Environment or earlier successfully reconciled siblings; the failing node retains its previous local values, no partial FrameCommit is published, and the dirty Scope can converge on a later frame.
This node-local commit model avoids a second resolved tree, a MountedNode undo journal, and a redundant second recomposition of the Scope that performed the update.

Stable shared Environment identity is important for content captured by a layer.
A layer opened from a nested Theme subscribes through its own existing Scope to the exact entries it reads, so later Theme or locale changes invalidate that Scope without scanning Layer entries or incrementing Layer revisions.
If the provider unmounts while the layer remains alive, shared ownership preserves the Environment and its last committed parent chain until the layer closes.

Root viewport class, locale, accessibility preferences, and root services continue to enter through the root Environment.
ResourceConfiguration is owned by AppResources and exposes its own CompositionDependency; resource resolution observes that dependency and equal configurations do not notify readers.
Exact layout constraints and safe-area geometry remain layout inputs rather than raw Environment dependencies.

## Theme API

Theme becomes an Environment boundary with direct View content:

```cpp
return MaterialTheme {
  Column {
    Text("Settings"),
    SettingsContent(),
  },
};
```

MaterialTheme, FlatTheme, their dark variants, and the generic Theme API become View-producing declarations.
They accept a content View and optionally an explicit Theme definition where that API is meaningful.
They do not require a content factory solely to make `UseTheme()` work.

Theme-aware built-in components store raw semantic inputs and resolve their styles while reconciling under the Theme boundary.
A reusable component that directly calls `UseTheme()` for composition logic must instead be composable:

```cpp
[[huxerui::composable]]
View BrandMark() {
  const auto& theme = UseTheme();
  return Text(theme.brand_name);
}
```

The direct-content migration removes the old factory-only Theme overloads rather than retaining two competing models.

## Deferred resources

Resource-bearing primitives retain semantic resource values until reconciliation.

`Text(StringResource)` stores a StringVariant containing the resource identifier and formatting arguments.
It does not call `UseString()` in the Text constructor.

`Image(ImageResource)` and resource-backed VisualFill declarations retain the raw image source.
They do not select a density or platform variant during View construction.

ViewSpec compilation resolves strings using the mounted locale and resource context, and resolves images using the mounted resource context, density, platform, and requested image behavior.
Existing resource caches remain responsible for avoiding repeated decoding and platform object creation.

`UseString()` remains available for composable application logic that genuinely needs an immediate `std::string`, such as branching or constructing a non-Text value.
Calling it requires a composable context.

The resource audit must include Text, Button, toggles, Tabs, SegmentedButton, navigation controls, Menu, Dialog, Toast, icons, Image, VisualFill, and Indication paths so no constructor performs a hidden ambient lookup.

## Composable marker

The public marker is:

```cpp
[[huxerui::composable]]
```

The generated function still returns an ordinary Scope View and the existing Scope, Composer, RecomposeScope, State, Lifecycle, and TaskScope runtime models remain authoritative.
No public Component type is added.

The application root already executes in an implicit RecomposeScope and remains unannotated.
Explicit `Scope(factory)` remains the lower-level path for code that cannot use code generation.

A reusable function requires the marker when its body directly uses composition-bound facilities, including:

- UseState and other ordered local state declarations.
- UseEnvironment and UseTheme.
- UseEvents and component event hubs.
- Lifecycle and UseTaskScope.
- Root-service access such as typed `UseXxx()` handles.
- UseString or other immediate ambient resource reads.

An ordinary helper that only constructs Environment-independent View declarations remains unmarked.
Calling an unmarked helper from a composable contributes its declarations to the caller's current scope, as it does today.

The marker applies to a function definition rather than an individual call.
Code generation removes the marker and wraps the original body in the existing Scope form while preserving captures, source locations, and return behavior.

The lightweight source transformer rejects direct unqualified and namespace-qualified calls named `UseXxx()` outside composable functions.
It ignores member calls, comments, literals, and preprocessor directives, and recognizes the Application root registered in the same translation unit as an implicit composable context.
A custom hook named `UseXxx()` may call other hooks without becoming a View-producing composable because it deliberately shares its caller's active composition scope.
This deliberately avoids a partial C++ call graph: aliases, function pointers, macro-generated calls, and indirect wrappers without the hook naming convention remain outside lexical enforcement and retain the existing runtime Composer checks.

## Virtual layout

A VirtualLayout item factory remains a genuine deferred factory because items are realized on demand.
It returns an Environment-independent ViewSpec tree or a Scope View produced by a composable function.
Direct composition API calls inside the raw virtual factory remain invalid because the factory itself is not a RecomposeScope.

Virtual mounted state retains the owner's effective Environment.
Realizing or reconciling an item passes that Environment into the normal Runtime path; no special virtual Composer or automatically inserted Scope is introduced.
Ambient and resource reads performed while a lazy declaration is compiled are retained by the nearest existing declaring RecomposeScope for that VirtualLayout node.
The retained dependency set is cleared when the virtual source is replaced and does not create a Scope, an Environment observer object, or another composition lifetime for each item.

Each realized item must declare a stable semantic root key.
Missing or duplicate keys are framework invariant failures rather than an invitation to infer identity from a viewport position.
A keyed composable item preserves its state identity when it moves, leaves the realization window, or re-enters under a changed Environment.

## Layers and overlays

Layer entries no longer read an Environment captured inside their root ViewSpec.
Presentation services capture the caller's shared Environment, and layer reconciliation carries each content View together with that Environment as one internal item rather than as parallel vectors.

Layer content uses its existing Scope to observe the exact Environment entry and resource dependencies consumed during ViewSpec compilation.
Provider updates therefore follow normal Scope invalidation; Runtime does not scan captured layers, force LayerEntry revisions, or pass a broad context-changed flag through reconciliation.
Layer ViewSpecs remain Environment-independent and use the same local compilation rules as application content.

TextSelection overlay content uses the focused MountedNode's effective Environment.
Window controls and debug overlays receive the root Environment explicitly unless a narrower presentation handle intentionally captures another boundary.

## Performance characteristics

Immediate primitive declaration remains allocation-compatible with the current View model and does not add a callable per View.
Only Scope, virtual items, navigation destinations, and presentation content that are semantically deferred retain factories.

Removing eager Environment capture makes View construction cheaper and removes accidental Theme work from helper execution.
The transparent Environment boundary replaces a provider-only RecomposeScope, reducing stateful scope ownership where no local state exists.

An Environment update invalidates only scopes subscribed to changed typed slots, and final-value comparison preserves incremental measurement, layout, paint, semantics, and extension updates inside those scopes.
Resource decoding, font lookup, image selection, and resolved style construction continue to use their existing caches.

## Error handling

Calling a composition-bound `UseXxx()` operation with no active Composer remains a `std::logic_error` with an English HuxerUI diagnostic.
Missing or duplicate virtual item keys are `std::logic_error` invariant failures.
Invalid public Theme, resource, or View configuration remains an early `std::invalid_argument` caller error.

A ViewSpec compilation failure propagates through the existing BuildFrame failure path before that node's mounted values are committed.
Reconciliation is not a whole-subtree rollback transaction, but a failed frame does not publish a partial semantic frame, RenderScene, layer state, or lifecycle commit.

## Migration

This was delivered as a coordinated breaking migration without retaining the former marker as a compatibility spelling.

The implemented Environment and Theme phases make ViewSpec independent of construction-time Environment, pass mounted context explicitly through mount and reconciliation, and use a transparent Environment node rather than a provider-only Scope.
Stable mounted Environment identity, typed entry dependencies, transactional updates, and Scope-owned ViewSpec compilation replace the removed broad mounted-context and captured-layer invalidation paths.
Property, component, resource, layer, TextSelection, virtual-layout, and window-control paths compile against their effective mounted Environment.
Theme and ProvideEnvironment accept direct View content and do not retain provider-only factories.
Composable code generation validates direct composition calls and lowers marked functions to the existing Scope runtime model.

No phase introduces a second declaration tree, a public resolved configuration type, or a concrete-component switch in Runtime.

## Validation

Focused validation must cover:

- Constructing the same ViewSpec outside a Composer and reconciling it under Material and Flat Themes.
- Explicit modifiers winning over Theme defaults, including false, zero, transparent, and empty values.
- Nested Theme and ProvideEnvironment boundaries with equal and changed effective values.
- Updating one Environment type without invalidating scopes that only read another type.
- Adding and removing a nearer Environment override while preserving outer-value shielding.
- Rolling back staged Environment entries and dependency notifications after a ViewSpec compilation failure.
- Theme switching preserving State, NodeExtension identity, focus, selection, and controlled values.
- Locale changes re-resolving strings without reconstructing unrelated declarations.
- Density and platform context changes selecting the correct image variant through existing caches.
- A composable component reconciling under an updated mounted Environment.
- An ordinary unmarked View helper remaining immediate and Environment-independent.
- Code-generated composables matching explicit Scope state, key, Lifecycle, and TaskScope behavior.
- Virtual items rejecting missing or duplicate root keys and preserving keyed state across recycling.
- Layers observing exact changes through their captured Environment and retaining its last values after provider teardown.
- TextSelection overlay, text input, debug overlay, and window controls resolving from the intended Environment.
- Environment boundary nodes producing no paint, semantics, pointer target, or extra layout geometry.
- Final-value equality avoiding unnecessary measure, layout, paint, semantics, and retained extension invalidation.
- ViewSpec compilation exceptions leaving the failing node's mounted values and the previously published frame intact.

Because this redesign changes shared public headers, Runtime, resources, Theme, virtualization, layers, and code generation, validation must include common unit and Runtime tests, public header checks, codegen tests, affected examples, the current Windows build, and every affected platform build available during implementation.
Renderers should not require semantic changes because MountedNode and PaintSequence remain the platform-neutral committed boundary, but each platform build must confirm that assumption.
