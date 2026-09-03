# Architecture Design

This document defines the modifier, animation, interaction, theme, presentation, platform integration, and root extension foundation.
Code examples match the current public API unless a section explicitly documents an unsupported boundary.

The ViewSpec compilation, Environment dependency, and `[[huxerui::composable]]` function model is specified in [View Composition and Environment Design](view-composition.md).

HuxerUI uses the `Platform` prefix for framework abstractions that cross the shared-runtime boundary, including PlatformAdapter, PlatformView, PlatformModule, and their lifecycle state. Concrete operating-system objects keep their exact API names, such as `UIView`, `NSView`, `HWND`, and `HTMLElement`. Feature and type names do not use `Native` as a synonym for `Platform`; lowercase native remains valid when it describes operating-system behavior or an ecosystem term such as a Java native method or Gradle `externalNativeBuild`.

The design has four goals:

- Keep the common View API small and declarative.
- Give built-in and third-party features the same extension mechanisms.
- Preserve mounted state across recomposition without adding feature-specific branches to `Runtime`.
- Reuse the existing Scope, Composer, reconciliation, layout, event, and virtual layout systems.

## Architecture overview

Each window owns one internal runtime root:

```text
RuntimeRoot
├── Root Environment
├── application MountedNode tree
└── Layer stack
    ├── popup entries
    ├── modal entries
    ├── toast entries
    └── system entries
```

The application declares one process-level root and options value:

```cpp
const Application application{App};
```

`RuntimeRoot` is synthesized by the Runtime. It is not a public layout component and does not require applications to wrap their root View.

`PlatformAdapter`, `UIThreadDispatcher`, and `ProcessMetrics` form the public host-capability boundary in `platform_adapter.h`.
`app.h` includes that boundary for application and Runtime declarations, while platform helpers that need only host scheduling or services include `platform_adapter.h` directly.

The main data flow is:

```text
State / Environment changes
    ↓
dirty RecomposeScope
    ↓
ViewSpec and ModifierSpec
    ↓
reconciliation
    ↓
MountedNode and NodeExtension
    ↓
frame, measure, layout, hit testing, and paint
    ↓
RenderScene
```

## Public View surface

The current `View` API has four primary extension points:

```cpp
class View {
public:
  template <ViewModifier... Modifiers>
  View With(Modifiers&&... modifiers) &&;

  template <EventKey Key, class Handler>
  View On(Handler&& handler) &&;

  template <LayoutValueKey Key>
  View LayoutValue(typename Key::Value value) &&;

  View Key(ViewKey key) &&;
};
```

`OnClick()` remains a high-frequency convenience wrapper for the typed event API:

```cpp
template <class Handler>
View OnClick(Handler&& handler) &&
{
  return std::move(*this).On<ViewEvents::Click>(
      std::forward<Handler>(handler));
}
```

Visual effects, interaction behavior, animation, presentation, and parent layout data are modifier values passed to `With()`:

```cpp
return Button("Save")
    .With(
        Padding{12.0F},
        Frame{
            .width = 120.0F,
            .height = 44.0F,
        },
        Background{Colors::Blue},
        CornerRadius{8.0F})
    .OnClick([=] {
      Save();
    });
```

The variadic form keeps common declarations compact:

```cpp
return Text("Hello").With(
    FontSize{18.0F},
    Foreground{Colors::White},
    Padding{12.0F},
    Background{Colors::Blue});
```

It also gives third-party modifiers the same syntax as built-in modifiers:

```cpp
return Card().With(
    ThirdParty::Glow{
        .color = Colors::Cyan,
        .radius = 16.0F,
    });
```

The current API does not require a dedicated `View` member function for every new modifier type.

### Modifier order

Modifiers are processed from left to right, but property modifiers do not form wrapper nodes. `Padding`, `Frame`, `Background`, `Foreground`, `FontSize`, alignment, spacing, and similar values remain in one ordered declaration sequence. Reconciliation applies that sequence once after component defaults resolve, so a later modifier that writes the same property wins without per-property override flags. `Frame` merges only explicitly supplied width, height, minimum, and maximum fields so independent declarations can constrain separate axes.

```cpp
view.With(
    Padding{12.0F},
    Background{Colors::Blue});
view.With(
    Background{Colors::Blue},
    Padding{12.0F});
```

These declarations currently produce the same padding and background. They do not express inner and outer backgrounds.

`Frame(width, height)` is the positional fixed-size form. Its six optional fields also support a single preferred axis and independent minimum or maximum bounds. The runtime validates local bounds when the modifier is applied, then intersects them with the parent `Constraints` before measuring content. Preferred dimensions collapse the resulting range to the closest permitted value. Frame constraints describe the outer node size, so Padding is deflated only after the frame range has been resolved.

Grow is a parent layout policy rather than a frame constraint. Row and Column divide finite remaining main-axis space by grow factor and pass each grow child a tight allocation. An unbounded main axis has no remaining extent to divide, so Grow does not expand there.
Grow compiles to a private typed LayoutValue key instead of a ViewProperties field. It therefore crosses composition boundaries through the same effective LayoutValue lookup as other typed layout data rather than through copied declarations. A Scope's explicit value overrides its single composed child's effective value; otherwise the child value is visible to the parent layout. An Environment boundary always exposes its single child's effective value because Environment nodes cannot declare View behavior. The presence of the private key distinguishes an explicit zero Grow from an absent declaration. Other modifiers retain node-local ownership and are never copied between the boundary and its child. Spacing, MainAlign, CrossAlign, and Align remain strongly typed node-local ViewProperties because the layout node consumes them as its own generic policy rather than as metadata supplied to its parent.

Flow uses the same public `Layout<Derived>` protocol as Row, Column, and Stack. It first measures children at their natural widths to form horizontal lines, then distributes finite remaining width among Grow children within each line. Main alignment is resolved separately per line, cross alignment applies within the line height, and the common Spacing value is used for both item and line gaps. An unbounded width produces one intrinsic-width line without Grow expansion.

Retained modifiers such as `ScrollBar`, `Indication`, animated `Opacity`, and third-party modifiers with an extension preserve their relative order. Compatible retained entries reconcile by descriptor and position. Frame and foreground paint callbacks run in declaration order, while extension hit testing runs in reverse order.

## Modifier descriptions and node extensions

There are two modifier categories:

- A property modifier remains ordered in `ViewSpec` and applies while resolving the declaration for a MountedNode, but does not create retained MountedNode state.
- A retained modifier stores a type-erased `ModifierSpec` in `ViewSpec` and a persistent `NodeExtension` in `MountedNode`.

Conceptually:

```text
Padding / Background ── apply ──▶ resolved node properties

Ripple / ScrollBar / Glow
    └── type erasure ──▶ ModifierSpec
                            └── reconciliation ──▶ NodeExtension
```

Each modifier type has a stable descriptor identity. Reconciliation compares modifier type and position:

- A compatible modifier updates its existing node extension.
- An incompatible modifier destroys the previous node extension and creates a new one.
- Reusing a `MountedNode` also preserves compatible modifier animation, gesture, and presentation state.
- Reordering modifiers is a semantic change and may recreate affected node extensions.

A third-party modifier can expose its node extension without changing `View`:

```cpp
class GlowExtension;

struct Glow {
  using Extension = GlowExtension;

  Color color;
  float radius = 12.0F;
};
```

The framework-provided adapter performs type erasure and dispatches typed updates:

```cpp
class GlowExtension final : public NodeExtension {
public:
  GlowExtension(MountedNode& node, const Glow& spec);

  void Update(MountedNode& node, const Glow& spec);

  void Paint(
      const MountedNode& node,
      PaintContext& context) const override;
};
```

The framework detects `Glow::Extension`, creates the node extension, and dispatches typed updates without requiring the modifier to expose descriptor or type-erasure details.

## Composition lifecycle

`Lifecycle(setup, dependencies...)` declares post-commit external setup and cleanup in the current `RecomposeScope`.
It is a composition function rather than a modifier because component lifetime follows scope identity, not the identity or kind of one returned root View.
The application root uses its implicit scope, ordinary helper functions contribute to their caller's scope, and `[[huxerui::composable]]` establishes an independent reusable lifetime.

Lifecycle identity combines the current scope, source location, and occurrence at that location.
Dependencies are listed last and accept State, StateList, or ordinary copyable equality-comparable values.
State dependencies subscribe the scope and compare cell identity plus version; ordinary dependencies compare captured values.
An unchanged declaration retains its active cleanup, while a changed dependency cleans up the old setup before starting the new declaration.
A successfully omitted declaration cleans up, and scope teardown cleans active declarations in reverse declaration order.

Composition records declarations without invoking application callbacks.
After reconciliation, layout, semantics, RenderScene generation, and damage calculation succeed, Runtime cleans changed and retired declarations in reverse composition order and then runs new setups in declaration order.
Failed composition discards pending declarations without disturbing active resources.
State writes from setup or cleanup schedule a subsequent frame rather than re-entering composition.
Lifecycle accepts ordinary `void` cleanup callables, while its internal cleanup boundary remains non-throwing.
Setup exceptions retain the Runtime's normal `BuildFrame()` propagation behavior, and an escaping cleanup exception terminates the process.

Virtual item caching preserves State slots only.
Evicting a mounted virtual scope therefore runs cleanup, while realizing it again restores State and runs a new setup.
Runtime teardown destroys mounted scopes and drains lifecycle cleanup before releasing Root Services.

## Structured task scope

`UseTaskScope()` returns the lazily created `TaskScope` owned by the current RecomposeScope without allocating an ordered composition slot.
The scope retains launched `Task<void>` executions across compatible recomposition and keyed movement.
Successful scope retirement queues TaskScope closure for the same commit boundary as Lifecycle retirement, while failed composition leaves active tasks unchanged.

Runtime performs retired Lifecycle cleanup before closing the corresponding TaskScopes, then releases Root Services and platform state.
TaskScope uses the owning PlatformAdapter's existing UIThreadDispatcher for initial execution, HuxerUI awaitable resumption, and lifecycle-bound external-thread posting.
RunWorker, WorkerSequence, and non-Web local File asynchronous operations share one bounded process-wide C++ worker executor, while platform-native asynchronous transports keep their existing owners.
WorkerSequence contributes per-instance FIFO admission and cooperative cancellation without owning threads or adding Runtime state.
Worker execution is process-local and does not imply platform background execution.
The Task model does not add Runtime branches to State, EventBindings, Lifecycle declarations, or concrete asynchronous services.

The complete public and execution contract is defined in [Task and Structured Concurrency Design](tasks.md).

The built-in HttpClient Root Service is the first platform asynchronous API built directly on Task.
It uses one private HttpTransport capability from PlatformAdapter, preserves HTTP values in shared C++, and resumes platform completions through the owning Task execution without routing requests through PlatformModule.
The complete request, cancellation, error, and backend contract is defined in [HTTP Client Design](http.md).

## NodeExtension lifecycle

`NodeExtension` operates directly on a controlled public `MountedNode`. There is no separate `ModifierHost` and no context object for every phase.

The current public lifecycle is:

```cpp
class NodeExtension {
public:
  enum class PaintInvalidation {
    None,
    Content,
    Foreground,
    Both,
  };

  struct FrameResult {
    bool needs_frame;
    std::optional<double> wake_after;
  };

  enum class PointerResult {
    Ignored,
    Observe,
    Handled,
    Capture,
    CancelTarget,
  };

  virtual ~NodeExtension() = default;

  virtual FrameResult OnFrame(
      MountedNode& node,
      const FrameInfo& frame);

  virtual PaintInvalidation PrepareGeometry(MountedNode& node, TextMeasurer& text_measurer);

  virtual void OnInteraction(
      MountedNode& node,
      const InteractionState& state,
      const std::optional<InteractionEvent>& event);

  virtual float OnPreScroll(MountedNode& node, Axis axis, float available, ScrollSource source);
  virtual float OnPostScroll(MountedNode& node, Axis axis, float consumed, float available, ScrollSource source);
  virtual float OnPreFling(MountedNode& node, Axis axis, float available_velocity);
  virtual float OnPostFling(MountedNode& node, Axis axis, float consumed_velocity, float available_velocity);
  virtual void OnScrollActivity(MountedNode& node, const ScrollActivity& activity);

  virtual bool HitTest(
      MountedNode& node,
      Point position) const;

  virtual bool HoverHitTest(
      MountedNode& node,
      Point position) const;

  virtual bool HoverWhenDisabled() const noexcept;

  virtual void OnHover(MountedNode& node, const HoverEvent& event);
  virtual void OnFocusChanged(MountedNode& node, bool focused);
  virtual bool OnKey(MountedNode& node, const KeyEvent& event);

  virtual PointerResult OnPointer(
      MountedNode& node,
      const PointerEvent& event);

  virtual void PaintBehindContent(
      const MountedNode& node,
      PaintContext& context) const;

  virtual void PaintAboveContent(
      const MountedNode& node,
      PaintContext& context) const;

protected:
  void InvalidatePaint(
      PaintInvalidation invalidation = PaintInvalidation::Foreground);
};
```

`PaintBehindContent()` records after the normal background and before the resolved border and node content.
`PaintAboveContent()` records after node content and descendants, before the framework-owned focus ring.
`NodeExtension` does not wrap measure, layout, or paint, and it has no `Next` continuations.
Custom child measurement and placement belong to `Layout<Derived>` or `VirtualLayout<Derived>`.

`PrepareGeometry()` runs after final presentation transforms are resolved and before paint consumes geometry.
It receives the active `TextMeasurer` as a callback-scoped borrowed reference so geometry-dependent extensions can measure labels without retaining a platform service.
It reports exactly which retained paint sequence changed.
This phase lets geometry-dependent extensions retain value snapshots without storing raw mounted-node references or forcing unrelated clean PaintSequences to rerecord.

During either paint callback, extensions append node-local PaintCommands through `PaintContext`.
Runtime stores the resulting content or foreground PaintSequence on the node's RenderNode, and platform renderers apply the inherited layout and presentation transform while traversing RenderScene.
Corner radii remain declaration values until a concrete paint, clip, path, shadow caster, or hit-test rectangle is known.
The shared geometry boundary scales overconstrained radii proportionally, so fills, borders, descendant clips, and pointer containment apply the same circular-corner normalization on every renderer.
Paint may extend beyond `Bounds()` unless an explicit clip limits it, and Runtime derives render visibility from recorded PaintSequence bounds and visible descendants.
`PresentationBounds()` is the transformed axis-aligned host-view logical layout bounds.
Pointer positions delivered to `NodeExtension::HitTest()` and `OnPointer()` are mapped back into the node's local coordinate space.

An extension whose `HitTest()` returns true keeps its node on the topmost pointer route and prevents lower visual branches from receiving that pointer. Runtime may query `HitTest()` while constructing the route and again before dispatch, so implementations keep it deterministic and free of side effects.

Every matching hover extension on the deepest hit node receives the complete `HoverEvent` through `OnHover()` rather than competing for one exclusive hover slot.
Public Hover handlers on the resolved ancestor route receive their own node-local Enter, Move, and Leave lifecycle without turning those Views into ordinary pointer targets.
`HoverWhenDisabled()` opts a hover-only affordance into disabled targets without enabling focus, touch, Click, or other pointer behavior.
An extension that returned `Observe` on pointer down continues receiving the pointer sequence without owning it; returning `CancelTarget` after recognizing a competing gesture sends a raw Pointer Cancel update to the active target and suppresses its activation.

Clean content and foreground PaintSequences remain attached to their stable RenderNode.
An extension calls `InvalidatePaint(PaintInvalidation)` after changing paint-visible retained state; the operation invalidates only the declared owner sequence or sequences and schedules a frame when called outside frame construction.
During frame construction, the current recording pass consumes that invalidation and `FrameResult` remains the only extension-controlled source of follow-up scheduling.

The existing `LayoutContext` and `VirtualLayoutContext` remain because they represent real child measurement sessions.

## MountedNode capabilities

The public `MountedNode` surface exposes controlled operations needed by layouts and modifiers:

```cpp
class MountedNode {
public:
  Size LayoutSize() const;
  Rect Bounds() const;
  Point LayoutOffset() const;
  Rect PresentationBounds() const;
  Point LocalToWindow(Point point) const;
  std::optional<Point> WindowToLocal(Point point) const;
  Rect LocalToWindowBounds(Rect bounds) const;
  float PresentationOpacity() const;
  bool IsEnabled() const;
  bool IsFocused() const;

  std::size_t ChildCount() const;
  MountedNode& ChildAt(std::size_t index);
  const MountedNode& ChildAt(std::size_t index) const;

  template <class Key>
  const typename Key::Value* LayoutValue() const;

  template <class T, class... Arguments>
  T& Cache(Arguments&&... arguments);
};
```

`Bounds()` has a zero origin and the node's layout size. `LayoutOffset()` is parent-relative. `PresentationBounds()` and the local/window conversion operations derive from the resolved ancestor transform chain for platform-boundary queries and diagnostics. Rectangle conversion returns a window-space axis-aligned bound, and inverse point conversion returns no value for a non-invertible transform. `PrepareGeometry()` observes the final resolved transform for the current frame; earlier extension callbacks may observe the previous resolution.

It does not expose Runtime ownership, Environment storage, reconciliation internals, or direct child insertion and removal. A `NodeExtension` requests a continuing frame or a delayed wake-up through the `FrameResult` returned from `OnFrame()` and uses its protected paint invalidation operation when retained visual state changes. General application-facing measure and layout invalidation APIs are deferred.

## Frame lifecycle

The frame sequence is:

```text
apply State invalidations
recompose dirty scopes
reconcile ViewSpec and MountedNode
advance scroll motion
measure
layout
refresh interaction state
advance retained node extensions and prepare geometry
resolve presentation properties
bring focused text input into view
refresh the text-input session
resolve and publish SemanticFrame
record dirty PaintSequences
compute damage
commit composition Lifecycle cleanup and setup
close retired composition TaskScopes
return FrameCommit with RenderFrame and an optional absolute deadline
invalidate and present platform damage
schedule the returned deadline
```

The current Runtime reuses clean measurement and placement results, retains clean content and foreground PaintSequences, and changes a RenderNode revision only when its commands or scene properties change. PaintSequence revisions and lightweight committed-scene snapshots produce conservative DamageRegion rectangles. Node-extension frame traversal caches whether a subtree contains any extensions and skips extension-free subtrees. A modifier that is waiting for a delayed transition schedules one wake-up rather than running empty frames.
Runtime may request a platform frame when state changes outside frame construction, but never calls the platform scheduler from inside `BuildFrame()`.
Continuous animation and delayed extension work are returned in `FrameCommit::next_frame_deadline`, allowing each host to present the current commit before arming the next frame.

Runtime calls fixed node and modifier lifecycle functions. It does not contain branches for concrete features such as ScrollBar, Ripple, Dialog, or a particular animation.

The retained scene and incremental invalidation architecture are defined in [Incremental Layout and Rendering Design](incremental-rendering.md).
Local geometry, the scene boundary, PaintSequence reuse, transform and opacity presentation updates, retained ScrollView movement, layout and virtual-realization caching, equality-aware modifier and layout-value diffs, and precise shared-runtime damage are implemented.
Windows and macOS consume shared DamageRegion output for platform partial redraw.
Android retains the same shared damage calculation and committed-scene path but currently invalidates its complete `HuxerUIView`.

The semantics pipeline is a parallel Runtime output rather than a RenderScene branch.
After reconciliation and final presentation geometry, Runtime resolves component declarations, NodeExtension contributions, application overrides, focus, visibility, and secure-data policy into one immutable owning `SemanticFrame`.
`FrameCommit` publishes a shared pointer to that frame beside `RenderFrame`, allowing platform accessibility objects to retain committed data without retaining MountedNode pointers.
The complete declaration, frame, action, identity, virtualization, security, and platform mapping contract is defined in [Semantics and Accessibility Design](semantics.md).

## Platform content integration

Libraries provide three platform integration forms:

| Requirement | Integration |
| --- | --- |
| Permission, Audio, Camera control, or another nonvisual capability | Registered PlatformModule instance owned by a component Lifecycle or typed Root Service |
| WebView, map, document preview, or another platform interactive hierarchy | Registered PlatformView factory and a real leaf View |
| Camera preview, video decode, or another high-frequency visual stream | ExternalTexture composed by the HuxerUI renderer |

The categories may coexist in one library.
A Camera library may expose a shared Camera service or a component-owned camera session and return an ExternalTexture for preview, while a WebView library installs a PlatformView factory.
PlatformView and nonvisual PlatformModule instances share registration ownership, stable names, event naming, surface isolation, and teardown safety without forcing their different update and request models through one factory type.
This does not introduce a Runtime subclass, a public Library base class, platform API types in shared headers, or an application-visible generic library lookup.

### PlatformRegistry contract

Every surface owns one internal `PlatformRegistry` with a single case-sensitive UTF-8 name space for PlatformModule and PlatformView registrations.
The registry records the registration kind, exact C++ value types, and an arbitrary compatible direct factory or platform-language bridge.
It rejects empty names, duplicate names across both registration kinds, incompatible C++ value types, and mutation after root installation completes.
Registration names have no required separator, hierarchy, prefix, or grammar beyond being nonempty valid UTF-8; `web/WebView`, `WebView`, and a reverse-domain name are equally valid library contracts.
Using `/` to group names is an optional library convention and never changes lookup or ownership semantics.
The registry is not a public service locator, a process-global singleton, or an application-selectable composition mode.

`RootContext` exposes only the operations required by explicit library installers and root-owned instances:

```cpp
root.RegisterPlatformModule<AudioPlayer, AudioPlayerOptions>("audio/Player", audio_factory);
root.RegisterPlatformView<WebViewProperties, WebViewController>("web/WebView", web_view_factory);

auto player = root.OpenPlatformModule<AudioPlayer>(
    "audio/Player",
    AudioPlayerOptions{.session = session}
);
```

There is no `root.Platform()` accessor and no public generic `Register`, `Find`, or arbitrary `std::any` entry point.
Each registration binds its exact Module type and optional Options type or its exact Properties type and optional Controller type.
Those types are inferred from a compatible direct factory or stated by a typed bridge adapter, so registration and opening validate the C++ contract without enumerating business methods or events.
The registered Module type is the exact handle returned by its factory and open operations; it may be a value facade, move-only owner, shared interface pointer, or another library-defined RAII type.
Root Service ownership is optional rather than the PlatformModule object model.
Applications consume a library's concrete service, component, or `UseXxx()` API rather than opening modules or spelling registered names directly.

Registration names remain necessary because Java, Kotlin, Objective-C, Swift, JavaScript, C++, and future platform languages need one stable rendezvous identity.
They are library contracts rather than application configuration and normally remain behind the library's concrete component, installer, and service API.
A string name does not weaken the associated C++ values: registration and use validate both the name and the concrete value type.

The registry constrains factory behavior, not how a library constructs or organizes that factory.
A library may register a direct callable, a retained factory object, a framework bridge adapter, or its own adapter that satisfies the same creation and lifecycle contract.
It may define one shared Install function, provide the same public Install function from mutually exclusive platform sources, or split common and platform installation into private helpers.
HuxerUI does not require a public factory base class, a `CreateXxxFactory()` convention, a Backend or Service type, or the same internal construction pattern on every platform.
The application still selects one documented RootHook without platform-specific registration code.

### Strongly typed C++ path

C++ PlatformModule and PlatformView implementations receive the library's concrete values directly.
Windows and Linux factories do not encode Options, Properties, requests, results, or events into `PlatformPayload`, and Apple or Android implementations written in C++ or Objective-C++ follow the same direct path.
`PlatformValue` is the public low-level in-process carrier that preserves exact C++ type identity and optional equality after immutable type erasure.
It is shared by RenderScene and platform factory adaptation but never crosses a language boundary; ordinary components and direct factories continue to use their concrete Properties, Controller, Options, and event types.
Registry erasure never exposes `std::any`, unchecked casts, or a dynamic call API to library code.

Every direct PlatformModule factory receives the owning surface's non-owning `PlatformAdapter&`, followed by `const Options&` when the Module declares Options.
This is the single host-capability dependency for direct factories; there is no alternate no-adapter signature or second factory context.
The adapter remains surface-owned and a Module must not retain it beyond that surface's lifetime.
PlatformView factory erasure also binds against the owning adapter, while direct create, update, Controller, and disposal callbacks receive only their exact platform handles and typed values.

PlatformModule is an ordinary library-defined C++ object or interface.
The library chooses virtual functions, a concrete value, callbacks, pimpl, or its own type erasure and independently chooses synchronous returns, Task or Future values, callbacks, streams, and error types:

```cpp
class AudioPlayer {
public:
  virtual ~AudioPlayer() = default;

  virtual void Play(AudioSource source) = 0;
  virtual void Pause() = 0;
};
```

A direct platform implementation may implement that interface itself, while a cross-language implementation may return a library-defined bridge-backed object implementing the same interface.
The registry never inspects `Play`, `Pause`, or any other business method.
HuxerUI does not require Module methods to return `Task`, `PlatformResult`, or a cancellation handle.

The low-level PlatformView declaration keeps its stable string name and strongly typed controlled Properties, while a concrete library component exposes its optional Controller and ordinary typed EventBindings:

```cpp
return WebView(
    WebViewProperties{
        .url = url,
        .allows_navigation = allows_navigation,
    }
)
    .Controller(controller)
    .On<WebViewEvents::NavigationChanged>(on_navigation_changed)
    .On<WebViewEvents::LoadFailed>(on_load_failed);
```

Properties are complete controlled declarative state.
They must be move-constructible and equality-comparable so View copies may share the immutable value and reconciliation can suppress unchanged updates deterministically.
A direct C++ factory receives `const Properties&`; only the common payload bridge invokes `Properties::Encode()`.
Controller objects, callbacks, platform objects, and executable closures never enter Properties.
`PlatformView(name)` remains the no-properties form and does not synthesize a null dynamic value in the C++ path.
There is no separate `.Events<Key...>()` declaration.

Each structured value carried by the common payload bridge owns the static `Encode(const T&)` or `Decode(const PlatformPayload&)` operation required by its direction of travel.
There is no separate `PlatformCodec<T>`, per-platform codec, factory-provided codec, method-key declaration, or mandatory Methods list.
Direct C++ calls use their concrete parameters and return values without a payload round trip.
`void` is the type-level no-value contract, while typed `PlatformChannel` calls use `std::monostate` for a strictly validated Null argument or result; HuxerUI does not add a public `Unit`, `EmptyOptions`, or placeholder request type.

### PlatformModule ownership

Registration and instance ownership are separate.
RootHooks register immutable factories before first composition, while each strongly typed Module instance belongs to the application abstraction that opens it.
A typed Root Service may own one shared window-lifetime instance, but a component may instead open an independent instance in `Lifecycle` setup and release it during cleanup.
Options are present only when the library contract requires them:

```cpp
template <class Module>
Module OpenPlatformModule(std::string name);

template <class Module, class Options>
Module OpenPlatformModule(std::string name, Options options);

Lifecycle(
    [session] {
      auto player = OpenPlatformModule<AudioPlayer>(
          "audio/Player",
          AudioPlayerOptions{.session = session}
      );

      return [player = std::move(player)] {};
    },
    session
);
```

The free `OpenPlatformModule(name, options)` operation is valid only while Runtime is executing a committed `Lifecycle` setup.
The declaring `RecomposeScope` already identifies its Runtime, and Runtime installs a scoped internal lifecycle execution context containing that surface's `PlatformRegistry` while invoking setup.
The operation resolves the registered name, verifies the PlatformModule kind and concrete Options type, and creates a new instance through the selected direct C++ factory or platform-language bridge.
The execution context is restored after setup, is never a process-global registry, and does not remain available to composition, event handlers, asynchronous callbacks, or cleanup.
Calling the free operation without an active Lifecycle setup is a framework usage error.

Registry freeze prevents later factory mutation but never prevents repeated instance creation.
The returned exact Module handle owns its direct implementation or cross-language bridge through the library's ordinary RAII model.
Capturing it in the returned cleanup gives dependency replacement and component unmount the ordinary Lifecycle cleanup order; the instance implementation owns any requests, subscriptions, and cancellation behavior required by its own API.
RootHooks use `RootContext::OpenPlatformModule(name, options)` instead because `RootContext` already identifies the same surface registry directly.
These are two lifetime-specific access paths to one registry rather than separate Module systems.

A library may wrap component ownership in a typed custom hook such as `UseAudioPlayer(options)`.
Such a hook uses stable composition state for any returned handle, opens and attaches the low-level instance from Lifecycle setup, and detaches it during cleanup.
It may instead return no value when declaring the external lifetime is the complete API.
HuxerUI does not add a generic `UsePlatformModule`, public registry handle, provider, mandatory service base class, or application-visible generic instance.
Library handles diagnose use before attachment or after cleanup and avoid strong reference cycles between their state, the implementation, and implementation-owned event handlers.

### PlatformView controllers

A PlatformView Controller is a stable library-defined C++ facade for imperative operations on one mounted PlatformView.
It is neither controlled Properties nor a platform object and never crosses a language boundary itself.
The library chooses its public synchronous, asynchronous, callback, stream, and error semantics:

```cpp
class WebViewController {
public:
  [[nodiscard]] bool IsConnected() const noexcept;

  void Reload() const;
  void GoBack() const;
  void EvaluateJavaScript(std::string script, EvaluationCompletion completion) const;
};
```

The Controller is the exact public type bound by registration and carried by the concrete component declaration.
It must be safely retainable by a View declaration, and equal Controller values must denote the same logical command target across recomposition.
HuxerUI does not inspect or prescribe its internal storage, inheritance, indirection, or platform bridge.
Calling `.Controller(controller)` creates a framework-internal typed binding for that value; the Controller does not embed a binding, State, pimpl, Access helper, Backend, or Connection required by HuxerUI.
The factory adapter receives the exact Controller type and defines how its mounted instance attaches and detaches.
The framework erases the binding only inside retained storage after validating the registered Controller type and preserves its stable identity across compatible reconciliation.
Runtime and the platform adapter retain the mounted implementation only while the PlatformView is committed, so the framework binding never keeps an unmounted platform object alive.

A Controller may be declared before its PlatformView mounts and becomes connected only after the candidate platform instance commits.
One Controller may be connected to at most one committed PlatformView at a time; a second simultaneous attachment is a framework usage error.
Compatible reconciliation preserves an unchanged binding, replacing a Controller detaches the old binding and attaches the new one without updating Properties or recreating the platform object, and unmount disconnects the Controller before invalidating calls and disposing the platform instance.
Disconnected calls are not queued or replayed; the library maps them to its own synchronous, callback, Future, Task, or error convention.
Initial URL, options, and other initial facts remain Properties rather than imperative commands issued before attachment.

A direct C++ factory may attach its platform instance to the Controller without an intermediate proxy.
A cross-language implementation may compose a reusable framework call channel that owns payload conversion, result delivery, thread transfer, cancellation, disposal invalidation, and late-result rejection.
Using that channel does not require the Controller to inherit a proxy base, and a library may instead implement its complete JNI, Objective-C++, Emscripten, or other bridge directly.
Both forms attach through the internal typed Controller binding and therefore share the same mount, replacement, and teardown semantics.

### Typed events

Platform events remain ordinary HuxerUI Event Keys and use `Event<Result(Arguments...)>`.
An event with several fields carries one owning, non-reference structured `T` rather than a multi-argument signature, giving every platform boundary one value to validate and decode.
Every `.On<Key>(handler)` call creates the typed EventBinding and, when the Key provides platform-boundary metadata, records its stable event name and concrete argument type in the same binding.
This behavior belongs to generic event binding construction rather than a PlatformView-specific Runtime branch, so fluent calls before or after ordinary View modifiers cannot lose the descriptor.
Multiple event types require multiple `.On<Key>()` calls and no parallel event list.

The Event Key inherits `Event<Result(Arguments...)>`, which already provides `Signature`; it never redeclares that alias.
A cross-language Event Key adds only its stable boundary `Name`:

```cpp
struct NavigationState {
  std::string url;
  bool can_go_back = false;

  static NavigationState Decode(const PlatformPayload& payload);
};

struct NavigationChanged : Event<void(const NavigationState&)> {
  static constexpr std::string_view Name = "navigationChanged";
};

struct LoadStarted : Event<void()> {
  static constexpr std::string_view Name = "loadStarted";
};
```

`Event<void()>` requires the Null payload and no decoder.
An event with one argument decodes an owned `std::remove_cvref_t<T>` through that value type's static `Decode(const PlatformPayload&)`, then invokes the handler with the declared signature for the duration of dispatch.
Ordinary local HuxerUI events may continue to have several arguments, but a cross-language event has zero arguments or one value; several boundary fields belong in one owning structured argument.
The inverse `Encode` operation belongs to the result type only when a value-returning event crosses back toward another language.

A direct C++ factory receives a `PlatformEventEmitter` and emits values without encoding:

```cpp
events.Emit<WebViewEvents::NavigationChanged>(navigation_state);
events.Emit<WebViewEvents::LoadFailed>(failure);
```

A PlatformView event may return a synchronous decision:

```cpp
struct NavigationRequested : Event<NavigationDecision(const NavigationRequest&)> {
  static constexpr std::string_view Name = "navigationRequested";
};

const NavigationDecision decision =
    events.Emit<WebViewEvents::NavigationRequested>(request).value_or(NavigationDecision::Allow);
```

A void `Emit<Key>()` returns `void`.
A value-returning `Emit<Key>()` returns `std::optional<Result>` so a missing binding remains distinct from a valid false, zero, empty, or Null result.
The event key owns the meaning of its result; a non-void result does not generically mean consumed input.

A platform-language bridge emitter receives an event name and its local immutable `PlatformPayload`; the SDK serializes it to the binary envelope, and C++ invokes the argument type's static `Decode()` operation before emitting the same typed Event Key.
The result crosses the same mounted route in reverse as an optional payload; an absent outer result is distinct from a present Null payload.
An event with no matching binding is ignored without decoding.
Duplicate subscribed wire names are invalid configuration, while malformed subscribed payloads and late events from obsolete instances are rejected without invoking application code.
Changing a handler reconciles EventBindings only and does not update or recreate the platform object.
An emitter becomes active only after successful creation.
PlatformView emission is synchronous on the owning UI thread so a native delegate can receive an immediate decision without posting and blocking.
Emission during candidate creation, after detachment, from a disabled mounted View, or from another thread produces no result and invokes no handler.
Disposal invalidates the emitter before releasing platform state, so later emissions are harmless no-ops.

Method results and events remain distinct.
A per-invocation `PlatformResult` completes or fails one cross-language call at most once, while an instance-level `PlatformEventEmitter` may publish any number of unsolicited notifications or synchronous PlatformView decisions.
Neither endpoint dictates the library's public callback or return type.
A PlatformChannel accepts only void event subscriptions; request results remain on `Invoke` rather than becoming result-returning Module events.
A direct C++ Module may expose ordinary callbacks or state without either endpoint, while a cross-language implementation may adapt Result and notification delivery into the library's chosen API.

### PlatformPayload boundary

`PlatformPayload` is the value model used by HuxerUI's common bridge when data crosses between C++ and another platform language.
It does not appear in ordinary PlatformView construction, direct C++ factory signatures, direct C++ method handlers, typed Root Service APIs, or Windows and Linux parameter flow.
The presence of static `Encode()` or `Decode()` members does not imply that they run for a direct C++ implementation or a library-owned bridge with explicit typed boundary conversion.

The corresponding structured C++ type is the single owner of its boundary schema:

```cpp
struct WebViewProperties {
  std::string url;
  bool allows_navigation = true;

  static PlatformPayload Encode(const WebViewProperties& value);
};

struct NavigationState {
  std::string url;
  bool can_go_back = false;

  static NavigationState Decode(const PlatformPayload& payload);
};
```

An outbound-only type defines only `Encode`, an inbound-only type defines only `Decode`, and a bidirectional type defines both.
Framework concepts diagnose a missing operation when a cross-language bridge requires it.
Scalar and framework data types have built-in boundary conversion; a library wraps an external structured type in an explicit boundary value rather than defining a detached codec specialization.

`PlatformPayload` remains an immutable equality-comparable tree containing null, boolean, signed 64-bit integer, double, UTF-8 string, `Bytes`, list, string-keyed object, and the closed framework capability `ExternalTexture`.
It is an in-process boundary value rather than a persistence, network, or general serialization format.
It never contains callbacks, arbitrary C++ objects, system handles, pointers, platform Views, or executable closures.
Large or continuous media frames do not travel through it; an ExternalTexture payload retains the same shared platform-owned texture object used by rendering.

#### Platform-language value API

The Android Java SDK, Web JavaScript bridge, and Apple Objective-C/Swift adapters expose one immutable `PlatformPayload` value type rather than separate Reader, Writer, Builder, or Codec abstractions.
Platform naming follows the language convention, while each common adapter provides the same explicit construction operations:

```text
nullValue()
booleanValue(value)
int64(value)
doubleValue(value)
string(value)
bytes(value)
list(values)
object(fields)
```

An adapter with an implemented ExternalTexture capability bridge additionally provides `externalTexture(value)`.

Constructors copy or safely freeze mutable byte and collection inputs.
Byte reads return a defensive copy or an immutable platform view and never expose mutable backing storage.
Java maps Int64 to `long` and Bytes to copied `byte[]`.
The Apple adapters map them to Swift `Int64` and `Data`, while Web maps them to JavaScript `bigint` and copied `Uint8Array`.
The Web contract never constructs Int64 from Number and currently rejects every ExternalTexture payload; every adapter rejects non-finite Double values, invalid Unicode, and unsupported capability values.

The same value type provides exact inspection and navigation:

```text
kind()
isNull()

requireNull()
requireBoolean()
requireInt64()
requireDouble()
requireString()
requireBytes()

requireField(name)
field(name)
fields()
rejectUnknownFields(names)

elements()
element(index)
```

An adapter with ExternalTexture capability transport additionally provides `requireExternalTexture()`.

These operations never coerce between Boolean, Int64, Double, String, or Bytes.
`field(name)` represents absence with the language's native nullable or optional result, while an explicitly present Null remains a non-absent `PlatformPayload` whose `isNull()` is true.
`fields()` and `elements()` return immutable child Payload values, and `rejectUnknownFields()` is an explicit boundary-type decision rather than a global policy.

A child returned by field or element navigation shares its immutable backing node and carries a lazy diagnostic path such as `properties.headers[2].value`.
That path exists only for error reporting and does not participate in value equality, hashing, object ordering, or HUXP encoding.
Missing fields, kind mismatches, invalid values, unknown fields, and range failures report the complete path through the platform's native exception or error mechanism with the same semantic error category on every platform.

The corresponding platform type owns its conversion just as the C++ type does.
Java uses a type-local static `decode(PlatformPayload)` and instance `encode()`, Swift uses `init(platformPayload:)` and `encodePlatformPayload()`, Objective-C uses the corresponding initializer or factory and `encodePlatformPayload`, and JavaScript or TypeScript uses type-local `decode(payload)` and `encode()` operations.
HuxerUI does not reflect arbitrary objects, require Java serialization or Swift `Codable`, inspect JavaScript object shapes implicitly, or add a generic `decode(Class<T>)` operation.
The SDK exposes no public JSON conversion, numeric coercion, or raw ExternalTexture slot.

The common cross-language bridge transports one HuxerUI binary representation rather than recursively translating the payload tree through JNI, Objective-C collections, or JavaScript interop calls.
The C++ bridge encodes its `PlatformPayload` to binary, the platform SDK automatically decodes that binary to its immutable local `PlatformPayload`, and the reverse path performs the corresponding platform encode and C++ decode.
Library implementations receive their platform SDK's `PlatformPayload` value and never parse or produce the transport bytes themselves.

```text
C++ T::Encode
  -> C++ PlatformPayload
  -> HuxerUI binary envelope
  -> platform PlatformPayload

platform PlatformPayload
  -> HuxerUI binary envelope
  -> C++ PlatformPayload
  -> C++ T::Decode
```

The envelope starts with the four ASCII bytes `HUXP`, a little-endian unsigned 16-bit format version, and a little-endian unsigned 16-bit flags field.
Version 1 requires zero flags and contains exactly one value followed by no trailing bytes.
The one-byte tags are Null `0`, Boolean `1`, Integer `2`, Double `3`, String `4`, Bytes `5`, List `6`, Object `7`, and ExternalTexture `8`.
Integer values use signed 64-bit little-endian representation, Double values use their IEEE 754 binary64 bits in little-endian order, and all byte lengths and container counts use unsigned 32-bit little-endian values.
Strings contain a byte length followed by UTF-8 bytes, lists contain a count followed by values, and objects contain a count followed by length-prefixed UTF-8 keys and values.
Object keys are serialized in ascending UTF-8 byte order so one payload has one canonical encoding.
ExternalTexture contains capability kind `1` as one byte followed by an unsigned 32-bit envelope-local slot.

The binary format preserves all payload kinds without implicit coercion.
Decoders require the declared tag and range instead of converting strings to numbers, truncating doubles to integers, or treating bytes as text.
Strings and object keys must be valid UTF-8, doubles must be finite, and containers enforce unique object keys plus nesting and allocation limits.
The maximum nesting depth is 64, matching the in-process payload contract.
All bridge implementations use the same framework constants for maximum envelope bytes, scalar bytes, and container entries, plus capability slots where supported, and validate them before allocation; a platform must not substitute looser local limits.
Unknown versions, flags, tags, or capability kinds, duplicate object keys, invalid UTF-8, non-finite doubles, integer or length overflow, truncated input, excessive allocation, and trailing bytes are malformed payloads.

An `ExternalTexture` is an opaque capability and therefore travels beside the binary data in a bridge-private capability table.
The binary stream contains only an envelope-local slot, while JNI global references, Objective-C objects, JavaScript handles, or C++ shared objects remain in the owning bridge for that crossing.
Slots are unique only within one envelope and are not public texture identifiers.
Repeated references to the same slot preserve capability identity within that decode.
A bridge without ExternalTexture capability transport rejects the value before encoding.
A bridge that supports the capability rejects a missing slot, duplicate capability-table entry, or kind mismatch while decoding.
On successful decode, the resulting shared reference or language wrapper retains the capability before the temporary table is released; on failure, the bridge releases the complete table.
Library code cannot forge, retain, or reuse a slot, and the closed table cannot carry arbitrary platform objects.

| Platform boundary | Binary data | Capability table |
| --- | --- | --- |
| Android | `byte[]` | Bridge-owned Java references |
| Apple Objective-C/Swift adapters | `NSData` | Bridge-owned Objective-C objects |
| Web common adapter | `Uint8Array` | ExternalTexture is not supported by this bridge |

Application callback objects never enter the envelope, and Objective-C, Java, JavaScript, or C++ exceptions are contained by the bridge that owns them.
`PlatformError` has a stable UTF-8 `code`, an English `message`, and optional structured `details` carried by the same payload envelope.
Framework codes reserve the `huxerui/` prefix and library codes use a library-owned prefix.
A bridge failure that prevents a library result from being represented becomes a stable framework error rather than escaping through another language runtime.

### RootHook platform registration

RootHooks are the only PlatformModule and PlatformView registration entry point.
Android, Apple, and Web applications do not repeat library registration in their host view, application delegate, or mount call, and platform packages do not mutate a process-global registry during static initialization.
The selected library RootHook registers either a direct factory or a bridge descriptor, and Runtime freezes the completed surface registry before first composition.

Android supplies common Java and Kotlin `PlatformViewFactory`, `PlatformView`, `PlatformModuleFactory`, and `PlatformModule` interfaces plus a JNI bridge adapter that resolves a library implementation class through the host application ClassLoader.
Web supplies JavaScript structural factories through `web::JavaScriptPlatformModuleFactory` and `web::JavaScriptPlatformViewFactory`, while retaining its direct Emscripten C++ factory path.
Apple libraries register either direct Objective-C++ factories or actual Objective-C/Swift factory objects through the platform-specific adapter.
Factories and instances are separate because one surface registration may create multiple independently owned View or Module instances.
One registered factory belongs to its surface registry, may create many independent instances, owns no created instance, and is released when that registry tears down.
Every successfully created instance is disposed exactly once; a failed creation has no instance disposal and publishes no event.

Common platform-language adapters expose only the event, result, and cancellation endpoints required by the boundary.
A factory's `create` operation receives its complete Properties or Options and a `PlatformEventEmitter`.
When a supported platform needs an owning host object, its protocol receives that exact platform type:

| Platform | PlatformView factory host | PlatformModule factory host |
| --- | --- | --- |
| Windows | Parent `HWND` | Owning `HWND` |
| macOS | Owning `NSWindow` | Owning `NSWindow` |
| Linux | Adapter-owned parent `GtkWidget` | Owning `GtkWindow` |
| Web | None; the factory returns a detached `HTMLElement` | None; browser globals are directly available |
| Android | Owning `android.content.Context` | Owning `android.content.Context` |
| iOS | Owning `UIViewController` | Owning `UIViewController` |

These are explicit platform protocol parameters rather than fields of a universal Context object.
The platform shell establishes every required owner before Runtime executes RootHooks, and a required host's absence is an integration failure rather than a nullable or partially initialized Context.
A future platform defines only the narrow host values required by its own factory contracts and does not widen the shared API.
A PlatformView exposes its platform View, `update`, `invoke`, and `dispose`.
A PlatformModule exposes `invoke` and `dispose`; `invoke` receives its method, arguments, and one `PlatformResult`, then returns an optional `PlatformCancellation`.
`PlatformResult.complete` and `fail` are accepted at most once, `PlatformEventEmitter.emit` publishes an instance event and may return a PlatformView decision, and `PlatformCancellation.cancel` is idempotent.
Request identities, late-result rejection, thread transfer, and bridge invalidation remain framework implementation details rather than platform-language API parameters.

```text
PlatformViewFactory.create -> PlatformView
PlatformView.getView / view, update, invoke, dispose

PlatformModuleFactory.create -> PlatformModule
PlatformModule.invoke, dispose

PlatformEventEmitter.emit
PlatformResult.complete, fail
PlatformCancellation.cancel
```

The Android SDK passes one framework-owned `HuxerUIPlatformChannel.Events` to each successful Java factory creation.
The Java implementation retains that emitter and calls `emit(name)` or `emit(name, payload)` from its platform callbacks; the emitter performs the common JNI call, binary transport, and instance-generation validation.
PlatformView emission runs synchronously on the owning UI thread and may return a nullable decision payload, while PlatformModule notification delivery remains asynchronous and always returns no decision.
Libraries do not declare one JNI callback per event.
The Web and Apple adapters receive equivalent framework-owned emitters, while direct factories emit through the C++ `PlatformEventEmitter` supplied by their registered factory contract.

The common cross-language adapters are asynchronous-capable transport contracts rather than library API models.
An implementation may complete a Result inline or later and may return no cancellation endpoint, while a library bridge may expose that operation as a callback, Future, Task, fire-and-forget method, cached synchronous state, or another abstraction.
The common call channel never blocks the owning UI thread to manufacture a synchronous API.
Libraries that require a genuinely synchronous boundary operation may supply their own platform bridge.

The optional C++ `PlatformChannel` is the reusable implementation of that transport contract.
It exposes named invocation, typed payload encoding and decoding helpers, typed event subscription, cancellation, and close semantics without becoming a PlatformModule base class or a public registry lookup result.
A library-defined Module or Controller may retain a channel internally, wrap it in any API shape, or ignore it and use a custom bridge.
Invocation, transport cancellation, and transport disposal are always scheduled in submission order through the owning adapter's `UIThreadDispatcher`.
`Invoke` allocates and returns its request identity before platform work begins, while `Cancel` and `Close` invalidate C++ delivery synchronously.
A canceled queued invocation is skipped; cancellation discovered while invocation is in progress runs on that same platform thread before a queued dispose, and all late results or events are ignored.

Direct C++ integrations remain strongly typed.
A Windows WebView factory may directly attach its instance to the registered WebViewController and emit `events.Emit<Key>(value)` from WebView2 callbacks; a Linux PlatformView implementation follows the same model with its GTK object.
Neither path constructs a payload or uses a method string.
An Android C++ bridge may connect the same Controller and translate `Reload()` to a Java invocation or a callback-oriented evaluation operation to the corresponding result endpoint.
The Java instance implements `invoke`, completes the supplied Result, and emits navigation events through the common emitter.
An Objective-C++ implementation uses the direct typed path, while an Objective-C or Swift instance uses the platform-specific framework bridge.
A Web implementation uses a direct Emscripten C++ factory or the common JavaScript adapter.
In every case the application sees only the same concrete WebView, Controller, and typed Event API.

The Android protocol shape is:

```java
public interface HuxerUIPlatformView {
    interface Factory {
        HuxerUIPlatformView create(
                Context context,
                PlatformPayload properties,
                HuxerUIPlatformChannel.Events events);
    }

    View getView();
    void update(PlatformPayload properties);
    default HuxerUIPlatformChannel.Cancellation invoke(
            String method,
            PlatformPayload arguments,
            HuxerUIPlatformChannel.Result result) {
        result.fail(
                "huxerui/unsupported-method",
                "HuxerUI PlatformView does not support controller calls",
                PlatformPayload.nullValue());
        return null;
    }
    void dispose();
}

public interface HuxerUIPlatformModule {
    interface Factory {
        HuxerUIPlatformModule create(
                Context context,
                PlatformPayload options,
                HuxerUIPlatformChannel.Events events);
    }

    HuxerUIPlatformChannel.Cancellation invoke(
            String method,
            PlatformPayload arguments,
            HuxerUIPlatformChannel.Result result);
    void dispose();
}
```

`HuxerUIPlatformChannel.Cancellation` is nullable when an invocation cannot be canceled.
The framework emitter owns the common native entry point, so a Java implementation calls `events.emit(...)` from Android listeners instead of declaring event-specific JNI methods.
A PlatformView without a Controller uses the default unsupported `invoke` implementation, and its C++ bridge does not create or connect a call channel.

The iOS and macOS adapters independently expose equivalent Objective-C protocols so Objective-C and Swift share one language contract without a common Apple host abstraction.
The iOS PlatformView protocol exposes a non-null `UIView*`, the macOS protocol exposes a non-null `NSView*`, and their factories receive the owning `UIViewController*` or `NSWindow*` plus Properties and the framework emitter.
Both View protocols expose optional `update` and `invoke` operations plus required `dispose`; Module protocols expose `invoke` and `dispose` and receive the platform-specific owning host at creation.
An Objective-C++ installer registers the actual Objective-C or Swift factory object through `ios::ObjectiveCPlatformModuleFactory`, `ios::ObjectiveCPlatformViewFactory`, or the corresponding `macos` adapter.
The framework retains that object in the surface registry and never performs class-name lookup or asks the application host to register it again.

```objc
@protocol HUXPlatformEventEmitter <NSObject>
- (nullable HUXPlatformPayload*)emit:(NSString*)event payload:(HUXPlatformPayload*)payload;
@end

@protocol HUXUIKitPlatformView <NSObject>
@property(nonatomic, readonly) UIView* view;
@optional
- (void)updateWithProperties:(HUXPlatformPayload*)properties;
- (nullable id<HUXPlatformCancellation>)invoke:(NSString*)method
                                      arguments:(HUXPlatformPayload*)arguments
                                         result:(id<HUXPlatformResult>)result;
@required
- (void)dispose;
@end
```

The AppKit protocol has the same operations and substitutes `NSView*` for `UIView*`.
Each platform installs its own pure Objective-C Clang module named `HuxerUIPlatform`; the iOS XCFramework module includes only UIKit headers, while the macOS SDK module includes only AppKit headers.
Swift imports that module without Swift/C++ interoperability, and both modules link to the existing HuxerUI library rather than introducing a second runtime binary.

The Web common adapter uses a structural factory and instance contract without requiring inheritance:

```js
export const webViewFactory = {
  create(properties, events) {
    const element = document.createElement("iframe");
    return {
      element,
      update(nextProperties) {},
      invoke(method, arguments, result) {},
      dispose() {},
    };
  },
};
```

The returned `element` is non-null and initially unparented, the adapter alone attaches it, and the implementation retains the supplied emitter for repeated events.
The factory object is linked and loaded before RootHooks execute, then the RootHook passes its actual `emscripten::val` to `web::JavaScriptPlatformViewFactory`; `mountHuxerUIApp()` does not register it.

The Android adapter owns binary payload transfer, instance creation, updates, invocations, events, results, cancellation, and disposal.
A class-based adapter uses a public no-argument constructor and resolves through the host ClassLoader rather than `FindClass` from an arbitrary thread.
PlatformView registration prepares its Java factory when the RootHook registers the View; PlatformModule registration resolves its Java factory lazily when the Module is first opened.
Each successful creation validates the returned instance contract, and PlatformView `invoke` is required only when the registered C++ View has a Controller.
The Android SDK publishes consumer keep rules for any implementation class referenced by stable runtime name so shrinking cannot rename or remove it.
A library may instead register its own JNI-backed factory when the common class adapter does not fit.

The C++ RootHook selects the common class adapter explicitly with `android::JavaPlatformModuleFactory<Module, Options>` or `android::JavaPlatformViewFactory<Properties, Controller>`.
Their callback fields use `std::function` directly: the Module adapter constructs the exact Module from a `PlatformChannel`, while the View adapter connects that channel to the exact Controller.
HuxerUI does not add callback aliases or require a proxy base.

An Objective-C++ Apple library may register a direct strongly typed callable like a Windows implementation or adapt a conforming Objective-C/Swift factory object.
The adapter always receives the actual factory object; runtime class lookup, a generated registrant, and application-delegate registration are not supported paths.

Web selects either its direct Emscripten C++ path or `web::JavaScriptPlatformModuleFactory` and `web::JavaScriptPlatformViewFactory` in the RootHook.
The JavaScript factories are linked before RootHooks execute and passed as actual `emscripten::val` objects, so the framework does not add a name-based JavaScript registry and `mountHuxerUIApp()` remains outside registration.
Windows and Linux normally register direct strongly typed C++ factories.

Factory and bridge construction remain library decisions on every platform.
The framework-provided class, protocol, export, payload, and call-channel adapters are conveniences that implement the registry contract; they do not define the only valid factory representation, require proxy inheritance, or force a library to use reflection.
Only the common payload bridge invokes the boundary type's static `Encode()` and `Decode()` operations.
A library-owned bridge may use explicit typed JNI, Objective-C++, Emscripten, or equivalent conversion while preserving the same public Properties, Controller, Module, and Event contracts.
Android, Web, and Apple common-bridge instances plus direct PlatformView factories receive the framework-owned event emitter so instance generation checks, creation-time queueing, UI-thread delivery, disposal invalidation, and late-event rejection remain uniform.

The common PlatformView adapter uses Create, Update, Invoke, Result, Cancel, Event, and Dispose when its library exposes imperative commands; a declaration without a Controller has no application path to those commands.
The common PlatformModule adapter uses Create, Invoke, Result, Event, Cancel, and Dispose, while a direct C++ Module or library-owned bridge uses its ordinary library-defined methods.
An adapter that supports asynchronous invocation owns monotonically assigned internal request identities and ignores late results after cancellation or teardown.
The platform adapter's `UIThreadDispatcher` preserves UI-thread delivery and event order without invoking callbacks inline from platform drawing, reconciliation, or a foreign-language call stack.
Events produced while creating a visual candidate remain queued until that candidate enters the committed RenderComposition; failed candidates publish nothing.
Disposal rejects new calls, cancels pending requests, detaches event delivery, and then releases platform state.
PlatformView creation, update, attachment, placement, removal, and disposal run on the owning UI thread.
Its platform root object is non-null, stable for the instance lifetime, initially unparented, and attached only by the adapter; updates receive the complete Properties value and must be idempotent.
PlatformModule creation, invocation, cancellation, and disposal run on the owning platform thread, while its Result and Event endpoints may be called from any thread and are marshalled by the bridge.
Even a result completed inline is delivered asynchronously after `invoke` returns.
Cancellation races resolve to exactly one terminal result, cancellation is idempotent, and absence of a cancellation endpoint means the invocation is not cancelable.
Disposal rejects new invocations, invalidates endpoints, cancels outstanding work where possible, and safely drops every later result or event.

### PlatformView

PlatformView is a real built-in leaf View rather than a modifier.
Runtime owns its mounted identity, compatible reconciliation, measurement, final geometry, visibility, hit-testing boundary, focus participation, semantic anchor, and unmount timing.
The platform adapter owns the corresponding `NSView`, `UIView`, Android `View`, `HWND`, DOM element, or equivalent platform object.

The low-level declaration has only the registration name and complete controlled properties:

```cpp
class PlatformView final : public View {
public:
  explicit PlatformView(std::string name);

  template <class Properties>
  PlatformView(std::string name, Properties properties);
};
```

`PlatformView("WebView")` is the explicit no-properties form.
Ordinary `.On<Key>()` calls attach both application handlers and any optional platform-boundary event metadata, so the declaration has no second event-list API.
PlatformView participates in shared focus traversal by default, while the ordinary `Focusable(false)` modifier removes a non-focusable PlatformView from that order.

A library exposes a concrete component such as `WebView()` and internally constructs `PlatformView(name, properties)` with a stable registration name and immutable strongly typed Properties.
The concrete component may expose `.Controller(WebViewController)` or accept the Controller as a required constructor value; the generic View API does not require every PlatformView to have a Controller.
The resulting internal binding retains the exact registered Controller type, value, and stable identity as implementation metadata rather than making the Controller a property modifier or encoded value.
Compatible recomposition retains the mounted PlatformView instance when the registration name and key remain compatible.
A changed registration name, incompatible Properties type, or incompatible key replaces it, while changed properties update the retained instance after the next successful commit.
Changing only the Controller detaches the previous binding and attaches the new binding to the retained platform instance after commit without resending Properties.

PlatformView measurement remains platform-neutral and never creates or synchronously measures a platform object during shared layout.
PlatformView has zero intrinsic logical size under loose constraints; ordinary parent constraints and size modifiers such as `Frame` produce its final axis-aligned layout bounds.
A concrete library may require dimensions or derive a Frame from controlled application data.
Platform intrinsic-content changes do not mutate mounted geometry behind Runtime or start an adapter-to-layout feedback loop.

PlatformView follows final RenderScene paint order rather than a separate platform plane or a component-tree depth number.
Content, children, foreground painting, sibling order, and LayerStack entries therefore determine PlatformView composition in exactly the same order as ordinary HuxerUI drawing.
Registration does not select a behind, above, overlay, or texture composition mode, and applications do not move content into LayerStack merely to cover a PlatformView.

The PlatformView leaf records one `PlacePlatformViewCommand` in its retained PaintSequence.
The immutable command carries the stable mounted identity, registration name, `PlatformValue` instances containing immutable strongly typed Properties and any controller binding, their revisions, and the final local axis-aligned destination rectangle.
It carries no platform handle and performs no raster drawing.
Those values preserve exact types, equality or identity, and direct-factory access without introducing a declaration wrapper or encoding to `PlatformPayload`.
Only the built-in PlatformView leaf records it; Canvas and public PaintContext do not expose an operation for placing an arbitrary PlatformView identity.
One mounted identity contributes exactly one placement to a committed scene, and a duplicate identity is a framework invariant failure rather than an ordering convention.
The surrounding RenderNode supplies the accumulated transform, clip, visibility, and paint position in the same way it does for every other PaintCommand.

`FrameCommit` remains a `RenderFrame` and `SemanticFrame`; it does not gain a parallel `PlatformViewFrame`.
Before raster presentation, a shared internal builder traverses the committed RenderScene for the platform adapter and derives an immutable `RenderComposition`.
The shared builder owns ordering and state-boundary semantics so platform renderers do not independently reinterpret command order.
The composition is an ordered sequence of HuxerUI render slices and resolved PlatformView placements:

```text
HuxerUI slice
PlatformView placement
HuxerUI slice
PlatformView placement
HuxerUI slice
```

Encountering `PlacePlatformViewCommand` flushes preceding HuxerUI drawing into the current slice, resolves the PlatformView's world bounds and rectangular clip, emits its placement, and begins a following slice with the correct inherited scene state.
Consecutive drawing remains in one slice, adjacent PlatformViews do not create empty slices, and a scene without PlatformViews retains the existing single-surface path.
`RenderComposition` is a platform-adapter implementation detail rather than a second public rendering tree or application-visible layer API.

Each nonempty slice has stable retained identity derived from its surrounding PlatformView boundaries and scene role.
Adapters reuse the platform-appropriate representation of compatible slices across frames, diff PlatformView identity and property revisions, and mutate the platform hierarchy only while applying a committed RenderComposition on the platform UI thread.
Platform objects are never inserted, reordered, or removed from `drawRect:`, `onDraw`, `dispatchDraw`, a paint callback, or renderer command replay.
A changed composition creates, updates, positions, clips, shows, hides, reorders, or destroys only the affected PlatformView instances and adapter-owned slice representations.
Moving or removing an item damages its previous and current visible bounds without rerecording unrelated clean PaintSequences.
An offscreen or temporarily hidden PlatformView remains mounted and preserves platform state; committed removal detaches input and accessibility references before destroying its platform object.
On Windows, a visible removed HWND remains retained behind the previous transparent aperture until the replacement HuxerUI surface is successfully presented, then it is destroyed before the next frame; this prevents the root background from flashing between frame commit and presentation.

The adapter creates a PlatformView instance when its identity first enters a committed RenderComposition and applies controlled properties before making it visible.
A property revision sends the complete controlled Properties to the compatible instance through an idempotent Update, while bounds-only changes update placement without resending unchanged Properties.
Factories should validate an Update before mutating observable platform state and apply the complete controlled value idempotently.
Replacement prepares the new instance before retiring the old one.
A factory exception is a library integration error that aborts the platform commit; adapters contain platform exceptions at their boundary but do not attempt to roll back arbitrary library-owned platform state.
Runtime shutdown detaches input, focus, and accessibility bridges, destroys PlatformViews and adapter-owned composition resources, and only then releases library Root Services in their existing reverse installation order.

Initial PlatformView presentation supports translation, axis-aligned layout, rectangular clipping, visibility, and deterministic ordering among PlatformViews.
Arbitrary rotation, path clipping, group opacity spanning a PlatformView, backdrop filters, and offscreen effects are unsupported until every platform can preserve their semantics.
The framework rejects unsupported declarations instead of approximating them silently.
Exact z-order does not imply support for an otherwise unsupported visual effect.

Platform pointer, keyboard, IME, and internal gesture handling remain inside the PlatformView hierarchy when the PlatformView is the active hit target.
Runtime hit testing and the committed `RenderComposition` use the same front-to-back order, so visually higher HuxerUI content wins before a covered PlatformView receives platform input.
Platform slice hosts remain hit-test transparent outside HuxerUI interactive regions instead of blocking the complete PlatformView rectangle below them.
Once the PlatformView wins hit testing, its platform hierarchy owns pointer sequences and gestures until completion or cancellation; the host does not duplicate those events into Runtime.
Focus traversal treats the PlatformView as one HuxerUI leaf, and platform focus changes synchronize that leaf without exposing platform responder objects.
A focused platform text editor owns its platform text service; Runtime suspends any HuxerUI text-input session until focus returns.

The semantic tree contains one PlatformView anchor at the mounted position.
The platform accessibility adapter attaches the platform object's accessibility root beneath that anchor, preserves its position among HuxerUI semantic siblings, and excludes duplicate HuxerUI descendants.
Removing or replacing the PlatformView invalidates the bridge before destroying the platform object so retained accessibility references fail safely.

Platform adapters preserve the same contract through platform-specific composition machinery:

| Platform | Composition strategy |
| --- | --- |
| Windows | One transparent DirectComposition surface replays every HuxerUI slice, while child HWNDs remain beneath it. Each placement clears a rectangular aperture in command order, and later HuxerUI drawing may cover that aperture without allocating a surface per slice. |
| macOS | Transparent HuxerUI slice views or layers and NSViews are retained as ordered siblings under one host NSView. AppKit hierarchy changes occur outside `drawRect:`. |
| Linux | PlatformView hosting is not implemented. |
| Web | HuxerUI Canvas slices and DOM PlatformViews are ordered siblings in one isolated CSS stacking context. The adapter coordinates DOM event targeting with Runtime hit testing. |
| Android | The host is a ViewGroup that alternates HuxerUI slice replay with ordinary child drawing in committed order. A `TextureView` participates as a regular child, while any `SurfaceView` subtree is rejected because its system composition cannot preserve this Canvas order. |
| iOS | Transparent HuxerUI slice views or layers and UIViews are retained as ordered siblings under one host UIView. CoreGraphics replay targets only damaged slices. |

These strategies are conformance requirements, not application-selectable composition modes.
A factory whose platform object cannot preserve exact ordering on the current platform fails with a diagnostic identifying the PlatformView registration name or unavailable adapter capability at the layer that detects it.
It must not silently flatten the declaration into a global foreground or background plane, capture an interactive platform hierarchy as stale pixels, or discard covering HuxerUI content.
Libraries may choose a different platform implementation internally, while high-frequency visual content without platform interaction remains an ExternalTexture.

Current shared tests cover payload invariants, per-surface registration, leaf layout, identity and property revisions, typed event delivery, frontmost hit testing, basic paint order, adjacent PlatformViews, keyed movement, replacement, and unsupported transforms and opacity.
The macOS integration fixture covers PlatformView creation, property update, HuxerUI slice ordering, retained identity, unchanged placement, focus synchronization, accessibility identity resolution, removal, stale-event rejection, and disposal.
Android focused tests cover semantic-anchor encoding, while the Android PlatformView example and device validation cover PlatformView creation, controlled updates, slice ordering, hit testing, focus and IME transfer, platform accessibility attachment, removal, and recreation.
The Windows integration fixture covers PlatformView creation and update, retained identity across hiding, nested HWND focus and UI Automation resolution, stale-event rejection, presentation-delayed retirement, remount, and deterministic disposal.
Each available platform adds an integration fixture with HuxerUI content below and above one PlatformView, verifies frontmost pointer ownership, platform focus and IME transfer, accessibility traversal through the anchor, retained platform state across recomposition and temporary hiding, and deterministic teardown.
Android integration rejects `SurfaceView` where system composition cannot preserve the shared order.
Web integration validation covers DOM stacking, platform events, focus boundaries, and disposal across supported browsers.

#### Windows PlatformView composition

The default Windows 10 adapter uses one premultiplied-alpha DirectComposition surface as the complete HuxerUI composition plane whenever the committed scene contains PlatformViews.
It does not create one HWND, swap chain, bitmap, or DirectComposition surface per HuxerUI slice.
The existing non-PlatformView presentation path remains unchanged until this composition mode is committed for the window.

The coordinator derives and consumes the ordered `RenderComposition` for PlatformView identity, placement, and z-order without deleting or replacing its slice boundaries.
The single-surface renderer traverses the committed scene once; ordinary HuxerUI drawing remains on that surface, each `PlacePlatformViewCommand` clears its resolved visible rectangle to transparent, and later HuxerUI drawing continues over the aperture.
This is observably equivalent to replaying the ordered slice ranges while avoiding a surface or replay pass per slice, and the retained slice contract remains available if Windows later needs a multi-surface mapping.
Child HWND z-order follows committed PlatformView order, so a later placement replaces earlier HuxerUI drawing within its aperture while drawing after that placement can cover the HWND content.
This preserves the shared observable paint order for axis-aligned opaque platform islands without pretending that HWND pixels participate directly in Direct2D blending.

Each mounted PlatformView owns one framework-private clipping container HWND.
The factory creates its root HWND as a child of that container, while the adapter positions the container at the visible rectangle and offsets the root HWND by the clip origin.
The container performs no drawing, never takes focus, contributes no semantic node, and is not a public composition concept.
It provides a stable ownership and rectangular-clipping boundary without modifying the factory-owned control through `SetWindowRgn`.
Placement and HWND z-order changes are applied on the window UI thread.

The Windows convenience factory contract lives in `<huxerui/windows/platform_registry.h>` and retains a library-defined instance rather than reducing it to an HWND plus untyped callbacks:

```cpp
namespace huxerui::windows {

template <class Properties, class Instance>
struct PlatformViewFactory {
  std::function<std::shared_ptr<Instance>(HWND parent, const Properties&, PlatformEventEmitter)> create;
  std::function<HWND(const std::shared_ptr<Instance>&)> view;
  std::function<void(Instance&, const Properties&)> update;
  std::function<void(Instance&)> dispose;
};

} // namespace huxerui::windows
```

`Instance` is an arbitrary library type and does not inherit a HuxerUI base class.
For a controlled registration, the factory adapter connects `Instance` to the exact registered Controller type; for an uncontrolled registration it needs no controller-related API.
The factory's `view` operation must return a same-process, same-UI-thread `WS_CHILD` HWND whose parent is the supplied clipping container.
After successful creation, HuxerUI owns destruction of that root HWND.
The adapter calls `dispose` once before `DestroyWindow`; `dispose` releases library callbacks, subclass state, and other owned resources but does not destroy the HWND, then the retained `Instance` is released.
Creation, update, disposal, placement, focus changes, and `PlatformEventEmitter` delivery run on the owning UI thread.
The emitter dispatches through the active mounted event route synchronously and returns no result from an inactive route.

A single framework-private transparent input-shield HWND covers the client area while PlatformViews are present.
It asks Runtime for the committed frontmost hit target.
When a PlatformView wins, the shield returns `HTTRANSPARENT` so the same-thread child HWND receives ordinary pointer input; when HuxerUI content wins, the shield routes input through the existing adapter path and prevents the covered child HWND from receiving it.
Pointer capture remains with the side that won the initial sequence.

Runtime-directed PlatformView focus selects the root HWND or its first focusable descendant.
After Win32 message dispatch, the adapter maps `GetFocus()` back through each hosted root and its descendants and synchronizes the matching mounted identity.
Tab traversal remains within standard child HWNDs while a next tab stop exists, then crosses the PlatformView boundary through Runtime focus order.
A focused HWND editor owns Win32 text services and suspends the HuxerUI text-input session until focus returns to a HuxerUI editor.
The UI Automation adapter exposes the hosted HWND provider below the existing PlatformView semantic anchor rather than publishing both as sibling roots.

Windows PlatformViews are opaque rectangular HWND islands.
A factory that returns a foreign-process HWND, a different-thread HWND, a non-child root, or platform content requiring per-pixel composition outside its aperture fails explicitly.
Rotation, path clipping, cross-boundary group opacity, backdrop effects, and other unsupported declarations remain rejected by the shared composition builder before platform mutation.
The Windows 7 compatibility renderer does not silently flatten PlatformViews into a global foreground or background plane; a Windows 7 session that requests this DirectComposition capability fails with an explicit unavailable-capability diagnostic.

### ExternalTexture

`ExternalTexture` is the platform-neutral shared identity of one live visual producer.
It owns immutable logical intrinsic size, monotonically increasing frame revision, and committed-visibility subscriptions, while a concrete platform subclass owns the native mailbox and publication API.
Application and renderer APIs retain it through `std::shared_ptr<ExternalTexture>`; there is no copyable wrapper, separate source object, public numeric identifier, or texture registry.

```cpp
class ExternalTexture {
public:
  virtual ~ExternalTexture() = 0;

  ExternalTexture(const ExternalTexture&) = delete;
  ExternalTexture& operator=(const ExternalTexture&) = delete;

  [[nodiscard]] Size IntrinsicSize() const noexcept;
  [[nodiscard]] std::uint64_t Revision() const noexcept;
  [[nodiscard]] bool IsActive() const noexcept;

protected:
  explicit ExternalTexture(Size intrinsic_size);
  void NotifyFrameAvailable();
};
```

The implemented concrete producers are `android::BitmapTexture`, `android::GlTexture`, `android::SurfaceStreamTexture`, `ios::PixelBufferTexture`, `ios::MetalTexture`, `macos::PixelBufferTexture`, `macos::MetalTexture`, `windows::PixelTexture`, `windows::D3D11Texture`, `linux::PixelTexture`, and `web::VideoFrameTexture` in their explicit platform headers.
Apple Objective-C and Swift use `HUXPixelBufferTexture` and `HUXMetalTexture`, imported as `PixelBufferTexture` and `MetalTexture`; each object is itself an `ExternalTexture` capability rather than an owner exposing a second `.texture` value.
Android accepts a retained `android.graphics.Bitmap`, synchronously copies current `GL_TEXTURE_2D` content through EGLImage, or owns a SurfaceTexture/OES consumer behind a producer-facing `android.view.Surface`; Apple retains immutable `CVPixelBufferRef` frames or synchronously copies a completed `MTLTexture`; Windows copies borrowed `PixelFrame` rows or a completed D3D11 texture; Linux copies borrowed `PixelFrame` rows; Web clones an open WebCodecs `VideoFrame` on the browser main thread.
Retained Android Bitmap and Apple pixel-buffer storage remains immutable while HuxerUI may render it; copied and cloned inputs may be reused or released after publication returns.
Future GPU-native producers such as DMA-BUF, AHardwareBuffer, or WebGPU textures require matching renderer import and synchronization support; they are added as new platform subclasses rather than optional fields or fake handles on the current CPU-backed types.

Apple `MetalTexture::Publish()` accepts level zero of a non-framebuffer-only 2D BGRA8 or RGBA8 texture after producer writes have completed.
It waits for an immutable GPU snapshot before advancing the texture revision, normalizes opaque or straight alpha with Metal Performance Shaders when required, and retains the previous frame if allocation, encoding, or execution fails.
The producer may reuse or release its source when the call returns.
UIKit and AppKit keep Metal textures in the ordinary `DrawExternalTextureCommand` path: a renderer imports the snapshot through a `CIContext` for its `MTLDevice`, applies the declared vertical origin, creates its cached `CGImage`, and replays it through the existing Core Graphics transform, clip, sampling, and opacity state.
The per-renderer device-to-context list is a native resource cache, not a shared service or texture registry.

`PlatformPayloadKind::ExternalTexture` transports `std::shared_ptr<ExternalTexture>` only across a platform-language boundary, including when nested in lists and objects.
The HUXP v1 bytes contain validated slots into a companion capability table; they never contain a process pointer or persistent identifier.
Constructing a payload from a null pointer is invalid, `AsExternalTexture()` requires the exact kind, and equality compares shared identity.
Platform bridges retain the same shared object, so publication never changes payload equality and does not create another mailbox.
This closed capability does not make PlatformPayload a generic object transport, and its binary envelope remains an ephemeral language-bridge format rather than a persistence or network format.
Strongly typed C++ PlatformModule results, events, and PlatformView properties pass their concrete types directly and require no payload traversal.

The existing Image component accepts ExternalTexture directly and reuses ImageFit, alignment, sampling, measurement, clipping, transform, and opacity behavior:

```cpp
View CameraView(const CameraSession& camera) {
  return Image(camera.PreviewTexture())
      .Fit(ImageFit::Cover);
}
```

No separate TextureView or ExternalTextureView class is added.
Tint remains vector-only and rejects ExternalTexture at the public configuration boundary.
Camera orientation, mirror state, crop metadata, and color conversion belong to the producer and platform renderer.
Intrinsic size is immutable because asynchronously changing it would mutate layout without controlled application state; a structural size or format change produces a new shared texture object.

Painting records a distinct `DrawExternalTextureCommand` in PaintCommand.
The command owns a `std::shared_ptr<ExternalTexture>` plus source and destination rectangles, sampling, and opacity.
It remains distinct from DrawImageCommand because immutable encoded images use decode caches while a platform renderer resolves the latest frame from its matching concrete texture type.
The command contains no frame revision, so publishing a frame does not make a clean PaintSequence unequal or require rerecording it.
Adding the command requires explicit handling in every renderer; a backend must not silently draw an empty rectangle.

Frame production does not write application State, recompose a scope, or rerecord an otherwise clean PaintSequence.
The concrete texture accepts frame publication from its supported producer context, atomically advances the shared revision, replaces its latest immutable frame snapshot, and requests at most one platform frame from each committed Runtime that currently displays it.
Runtime records visible texture uses while publishing the RenderScene and retains a committed snapshot of each identity, revision, and transformed destination.
On the next BuildFrame it compares revisions with that snapshot, damages every changed visible destination, and advances the RenderFrame revision while retaining the PaintSequence and RenderNode structure.
The same texture may appear in several nodes; each visible destination participates independently in damage.

ExternalTexture uses a latest-wins mailbox rather than an unbounded frame queue.
The capture or decoder thread never waits for Runtime, intermediate frames may be dropped, and a renderer acquires the newest frame available when processing damaged content.
The mailbox retains one immutable latest frame and each active renderer may retain the snapshot it imported.
Acquisition does not remove the mailbox value, so several renderers or windows can consume the same publication independently.
If no newer frame is ready during an unrelated redraw, the renderer reuses its last imported resource.
Before the first frame, Image contributes transparent visual content without treating the valid texture as an error.
After `Finish()`, rendering freezes on the last published frame until the final shared texture and renderer caches release it.

Committed visibility controls scheduling rather than production ownership.
When no committed visible command references the texture, publication updates the mailbox but does not continuously wake the UI.
`IsActive()` reports whether at least one live Runtime currently displays the texture, but Runtime does not own or pause the producer automatically.
Becoming visible through an ordinary application frame schedules the newest published revision without requiring a new Publish call.
Visibility is a private weak subscription from each PlatformAdapter, not a single surface binding.
The same texture may be displayed by several Runtimes; unmounting one removes only that Runtime's subscription.

Frame acquisition and synchronization remain platform-specific because a safe common return type cannot represent `CVPixelBuffer`, `IOSurface`, `AHardwareBuffer`, `SurfaceTexture`, DXGI resources, DMA-BUF, `VideoFrame`, and future native handles.
The shared command retains only the abstract identity and immutable drawing data, while each renderer casts to its matching concrete platform type and uses that type's private mailbox interface.
Each backend chooses a platform-specific direct-import path when its renderer and producer share a compatible graphics API and otherwise uses a bounded platform-owned conversion path.
The API promises no copy through shared Runtime; it does not claim universal zero-copy on the current CoreGraphics, Android Canvas, or Cairo backends.
The Apple implementations accept `CVPixelBufferRef` and use Core Image conversion compatible with their existing renderers.
The Android API 23 path keeps Canvas as the primary renderer and draws retained `Bitmap` frames directly.
`GlTexture::PublishCurrent()` reads level-zero `GL_TEXTURE_2D` content from the current EGL context, duplicates an optional native acquire fence, and synchronously converts it once into a compositor-owned premultiplied 2D texture without CPU readback.
Without an acquire fence the call waits for producer GL work; after `PublishCurrent()` returns, the source texture storage is no longer retained and may be reused or deleted.
`SurfaceStreamTexture` creates a producer-facing `Surface`, latches its SurfaceTexture on the private EGL thread, applies the SurfaceTexture transform, and converts each ready OES image once into the same immutable internal representation.
Revision advances only after that import completes, so Runtime never observes a revision whose frame is not drawable.
Each GPU draw occurrence owns an internal non-interactive TextureView output while it remains in the committed scene.
The Android renderer encounters that ordinary `DrawExternalTextureCommand` during Canvas replay, renders its selected source rectangle into the TextureView surface, and invokes `drawChild()` under the current Canvas transform, clip, and save-layer stack.
This platform-private child does not become a PlatformView, public View, accessibility node, or shared RenderComposition layer.
Frames containing GPU texture children move the base Canvas slice from `onDraw()` into `dispatchDraw()` so all Canvas, GPU texture, and PlatformView content retains command order.
Direct `AHardwareBuffer` import remains future work.
The Linux implementation copies borrowed straight-alpha RGBA8888 or BGRA8888 rows into Cairo premultiplied ARGB32 storage, keeps one Cairo surface per active texture, and leaves DMA-BUF import and explicit synchronization as future renderer work.
The Windows `PixelTexture` implementation copies the same borrowed formats into premultiplied BGRA storage, updates a retained Direct2D bitmap once per physical frame, and preserves the last CPU frame across D3D device recreation.
It reuses the bitmap allocation while pixel dimensions remain unchanged.
`D3D11Texture::Publish()` accepts a completed one-mip, one-slice, single-sampled BGRA8 default-usage texture, copies it once into a new HuxerUI-owned NT-handle shared resource, waits for that copy, and publishes only the immutable snapshot.
The producer may reuse or release its source after publication returns, but it must submit writes first and externally serialize access to its immediate context.
The renderer opens the snapshot on its own same-adapter D3D11 device and creates a Direct2D bitmap from that DXGI surface without a renderer-local texture copy or CPU readback.
Every renderer precollects the snapshots used by a frame, acquires their keyed mutexes in stable resource order, submits all Direct2D reads with `EndDraw()`, and releases the mutexes in one cleanup path.
A zero-timeout contention result abandons that render attempt and schedules a retry instead of blocking the UI thread; abandoned synchronization and adapter mismatch fail explicitly.
Renderer-local device resource recreation discards only the opened resource, mutex, and Direct2D wrapper, then reopens the retained snapshot while its producer device remains valid.
A removed producer device invalidates snapshots owned by that device, so the application must publish a replacement from a valid device before another render.
The modern NT-handle path is unavailable in Windows 7 compatibility builds.
The Web implementation accepts only open WebCodecs `VideoFrame` values, clones each publication synchronously, and leaves the caller responsible for closing its original value.
Because `emscripten::val` is thread-affine, texture construction, publication, finish, and destruction remain on the browser main thread.
The Canvas renderer clones the latest mailbox frame at most once per physical frame, shares that renderer-owned clone across every Canvas slice, maps logical crop coordinates through `displayWidth` and `displayHeight`, and closes replaced or inactive clones.
WebCodecs may share the clone's underlying media resource, but Canvas drawing and browser color conversion may still copy, so this backend does not claim zero-copy.

GPU-native expansion remains backend-owned rather than copying Android's TextureView mechanism into shared Runtime.
Each platform preserves its primary renderer and introduces native composition only when that renderer cannot import the producer inline.

Payloads and retained PaintCommands share the same opaque texture lifetime without a registration record.
Unmount first removes committed drawing references and visibility subscriptions.
Renderer caches are weakly keyed and release imported frames when the owning renderer prunes the entry or is destroyed; platform mailbox resources are released when the texture loses its final owner.
Runtime destruction releases RenderScene and platform-content frames before Root Services are destroyed in reverse registration order.

ExternalTexture is visual content, not a PlatformView interaction or accessibility subtree.
Image semantics apply unless the library supplies a more specific HuxerUI semantic declaration, and controls layered over a Camera preview remain ordinary HuxerUI nodes.

## Animation model

Animation separates immutable timing policy, retained scalar execution, synchronized presentation projection, component motion, and explicit whole-scene transitions. The complete contract is defined in [Animation and Scene Transition Design](animation.md).

`AnimationSpec` is a value and does not own runtime state. `MotionController` retains scalar value, target, velocity, delay, repetition, and time. `Animated<T>` remains the declarative target accepted by presentation modifiers, while `Transition` projects one retained progress value onto several presentation properties that must remain synchronized. Advancing either form updates mounted presentation state without recomposing the component.

Explicit scene transitions freeze only committed render data. The new mounted tree becomes authoritative for input, focus, text input, semantics, and window appearance immediately; the frozen old scene has no logical lifecycle.

### View lifecycle transitions

General View insertion and removal transitions are not part of the public API.
Dialog, BottomSheet, Menu, Toast, and SnackBar retain Layer entries through their component-owned exit motion and reuse the shared animation engine.
Future work may generalize lifecycle transitions only after identity, interruption, retained-state ownership, and reduced-motion behavior have one shared contract.

### Reduced motion

Accessibility and platform preferences enter through Environment. Theme motion resolution can replace animations with `SnapSpec` or shorter motion without changing each component.

## Interaction and indication

The complete interaction-state, indication, visual-fill, and retained paint contract is specified in [Interaction and Indication Design](interaction-indication.md).
The repeated-tap, long-press, drag, recognition, and pointer-ownership model is specified separately in [Gesture Recognition and Arbitration Design](gestures.md).
In-process typed transfer builds on that ownership path and is specified in [Typed Drag-and-Drop Design](drag-drop.md).

Pointer input follows one shared pipeline:

```text
PointerEvent
    ↓
hit testing and gesture arbitration
    ↓
Runtime-owned mounted InteractionState and ordered InteractionEvent
    ↓
retained NodeExtension::OnInteraction
    ↓
Indication animation and phase-specific paint invalidation
```

Each accepted Press receives a Runtime-unique `press_id`. Release and Cancel retain that identifier, and pointer Press position is node-local. The mounted snapshot coalesces effective enabled, hover, focus, focus-visible, and aggregate pressed facts while pointer and keyboard sessions preserve ordered interaction edges. Runtime submits complete next snapshots through one internal update path before notifying retained extensions. This supports multiple simultaneous pointers and multiple active ripple instances without turning Indication into an input recognizer.

`OnClick()` and `.On<ViewEvents::Click>()` register the same typed event and make an ordinary View participate in click interaction. Intrinsic controls such as Button, IconButton, and Chip own their activation capability and default indication independently of whether an application registers a handler; `Enabled(false)` explicitly disables them. Flat themes use state layers, while Material themes combine state layers with ripple. `InteractionScheme` provides one complete default `Indication` and one `FocusRing`; a typed component style may provide an explicit `Indication` when its surface differs from the theme-wide treatment. Reduced-motion themes snap those transitions.

`Enabled` is a semantic modifier. Effective enabled state is resolved from the root toward its descendants, so a child cannot re-enable itself beneath a disabled parent. Disabled controls remain hit-test barriers without receiving pointer, scroll, focus, or Click interaction. A control that directly establishes the disabled boundary uses its component-specific disabled state colors. A non-control boundary applies disabled group opacity once; inherited descendants keep their enabled paint colors so the subtree is not dimmed again.

`Focusable` lets a custom View participate in the window focus order. Button is focusable by default. Runtime owns one focused mounted-node identity, dispatches `FocusChanged`, and moves focus for Tab or Shift+Tab. Enter activates a focused Button on key down; Space publishes ordered keyboard Press and Release interactions and activates on key up. Meaningful keyboard input, including an unmapped key reported as `Key::Unknown`, makes focus visible; the explicit left and right Shift, Control, Alt, and Meta keys do not reveal a pointer-focused ring by themselves. Focus ring, disabled opacity, and indication motion resolve from Theme.

`Key` identifies portable keys independently of layout-resolved `KeyEvent::text`. Left and right modifiers, main-row and numeric-keypad keys, punctuation, international keys, and F1 through F24 remain distinct. Modifier booleans intentionally report their collapsed active state. Release events have empty text and are never repeats; unmapped keys still travel through the route as `Key::Unknown`.

Keyboard dispatch has one ordered decision path: platform text/IME filtering, `KeyIntercept` from the active focus-scope root to the focused View, the focused text client, focused-node `NodeExtension::OnKey`, the focused View's `KeyDown` or `KeyUp`, Runtime defaults, and finally the platform default. Every decision stage returns `true` only when it consumes the event. `KeyIntercept` stops its root-to-target traversal at the first true result and does not bubble. `KeyDown` and `KeyUp` are direct focused-target events, so a parent that must override component behavior uses `KeyIntercept` instead of a second routing convention.

Runtime defaults own Escape/Back, Tab traversal, keyboard context menus, and Enter or Space activation. A consumed Space release cancels any pending keyboard Press without emitting Click. `Runtime::HandleKeyEvent` returns the final consumption result to the adapter, which suppresses its native default only for a handled event. A PlatformView keeps ordinary keys inside its native subtree; when an implemented host reports that Tab reached the native focus boundary, it re-enters this same Runtime key path rather than calling focus movement directly.

The topmost modal Layer is the active focus traversal root. Opening a nested modal captures the current focus, and dismissing it restores the previously focused mounted node when that node still exists and remains enabled.

When a pointer drag crosses the scroll threshold, the selected scroll container wins gesture arbitration. The original click target receives a raw Pointer Cancel update, Click is suppressed, and its indication runs the cancellation animation.

### Indication

`Indication` is one immutable retained modifier and Theme value. Focus, hover, and press layers can provide a `VisualFill`, border, corner radii, placement, and animation. Ripple remains a color-based ordered Press visual with independent placement. An empty explicit value disables built-in state layers and ripple without introducing a sentinel type:

```cpp
return Button("Save").With(Indication{});
```

`Brush` owns the closed Color, linear-gradient, and radial-gradient source vocabulary shared by rectangle fills and Path fills or strokes. `VisualFill` stores either a Brush or an image fill and is shared by generic `Background` and indication layers. Generic `Border` remains independent so transparent surfaces can be outlined. Runtime resolves image resources during mounted reconciliation, while platform renderers execute only immutable paint commands.

Retained extensions paint through `PaintBehindContent` and `PaintAboveContent`. The content sequence contains shadow, normal background, behind-content indication, resolved border, and node content; children retain their own sequences; the foreground sequence contains above-content indication and focus ring. Mounted nodes keep only the authoritative interaction snapshot, aggregate Press count, current animated border and radii, and an optional geometry-resolved indication bounds override.

## ScrollBar as a modifier

ScrollBar is a View modifier:

```cpp
return VirtualList(items, ItemView).With(
    ScrollBar{});
```

Explicit values override Theme defaults:

```cpp
return VirtualList(items, ItemView).With(
    ScrollBar{
        .thickness = 8.0F,
        .minimum_thumb_extent = 32.0F,
    });
```

`ScrollBarExtension` owns:

- Opacity animation state.
- Delayed hide scheduling.
- Hover and drag state.
- Thumb geometry and pointer handling.
- Foreground painting.

Scroll activity, hover, and drag update this modifier. Runtime does not retain ScrollBar-specific animation or pointer functions.

## Environment

Environment is a typed, hierarchical value system:

```cpp
template <EnvironmentValue Value>
const Value& UseEnvironment();
```

The Environment value type is also its lookup identity and provides its fallback through `Value::Default()`.

```cpp
struct GreetingLocale {
  std::string language;

  static GreetingLocale Default() {
    return {"en"};
  }

  bool operator==(const GreetingLocale&) const = default;
};

const GreetingLocale& locale = UseEnvironment<GreetingLocale>();
return ProvideEnvironment(GreetingLocale{"fr"}, Content());
```

Use a semantic wrapper when two ambient values share the same underlying representation. Primitive or third-party representation types are not separate Environment keys by themselves.

An Environment used as a public value is a detached declaration.
Runtime mounts that declaration into a stable shared Environment, installs its inherited parent, and updates its private typed entries in place during compatible reconciliation:

```cpp
class Environment {
  std::shared_ptr<const Environment> parent_;
  std::unordered_map<std::type_index, Entry> entries_;
};
```

Each entry retains an optional local value, its equality operation, and a shared composition dependency.
Copying an Environment copies only detached declaration values, not its mounted parent, dependency identities, or subscribers.
Each composed subtree receives its current shared Environment, and a nested provider shadows only the value type it supplies while inheriting every other value through the parent chain.
`UseEnvironment<T>()` observes each `T` entry it visits until it finds the nearest local value, including absent entries whose later insertion could change the result.
Provider updates invalidate only RecomposeScopes subscribed to entries whose values actually changed.

Environment carries:

- Theme values.
- Platform and accessibility values.
- The runtime-managed viewport width class.
- Per-window services.
- Other typed third-party values.

Theme and services reuse Environment rather than introducing parallel tree propagation systems.

The public `UseViewportClass()` read resolves an internal Environment value with Compact, Medium, and Expanded states. `AppOptions::viewport_breakpoints` owns the two increasing width boundaries. `Runtime::SetWindowMetrics()` updates that entry only when the resolved viewport class changes, and normal dependency tracking invalidates the exact application or layer scopes that observed it. Exact viewport and safe-area dimensions do not become raw Environment values: measurement receives them through `Constraints` and the layout-time safe-area context, while repeated changes inside one class remain incremental layout work rather than composition dependencies.

## Theme

Theme is a transparent subtree provider built on Environment:

```cpp
return Theme {
  std::move(definition),
  Column {
    Text("Settings"),
    SettingsContent(),
  },
};
```

The Theme node stores one ordinary child declaration and does not create a RecomposeScope.
Built-in primitives retain raw semantic inputs and resolve final component values when Runtime reconciles them under the mounted Theme Environment.
A component whose implementation directly calls `UseTheme()` or `UseEnvironment()` is composable because the read belongs to its own composition lifetime.

### Theme systems

Core semantic theme values include:

```cpp
struct ThemeSpec {
  ColorScheme colors;
  TypographyScheme typography;
  ShapeScheme shapes;
  SpacingScheme spacing;
  ElevationScheme elevation;
  MotionScheme motion;
  InteractionScheme interactions;
};
```

Component styles are typed Environment values:

```text
TextStyle
ButtonStyle
IconButtonStyle
ChipStyle
SegmentedButtonStyle
SelectStyle
DividerStyle
CheckboxStyle
RadioButtonStyle
SwitchStyle
ProgressCircleStyle
ProgressBarStyle
SliderStyle
TooltipStyle
DialogStyle
BottomSheetStyle
MenuStyle
ToastStyle
SnackBarStyle
ScrollBarStyle
```

Third-party components can define their own style keys without extending a single global style registry.

Material, flat, and third-party themes are ordinary View declarations, not Runtime types or Runtime subclasses.

The built-in Flat and Material systems provide complete light and dark boundaries:

```cpp
FlatTheme {Content()}
FlatDarkTheme {Content()}
MaterialTheme {Content()}
MaterialDarkTheme {Content()}
```

`FlatLightThemeSpec()` and `FlatDarkThemeSpec()` return mutable token values that applications can use as the starting point for a branded flat theme. `MaterialLightThemeSpec()` and `MaterialDarkThemeSpec()` provide the corresponding Material tokens. Flat and Material Theme definitions explicitly register their complete Dialog, BottomSheet, Menu, Toast, and SnackBar styles rather than relying on presentation services to infer a style from ThemeSpec. Passing customized tokens as the first constructor argument to `FlatTheme` or `MaterialTheme` rebuilds that system's component styles from those tokens.

### Theme syntax

Pass ordinary View content directly:

```cpp
return MaterialTheme {
  AppContent(),
};
```

A reusable component that reads Theme values for composition logic keeps its own scope:

```cpp
[[huxerui::composable]]
View BrandContent(UserId user_id) {
  const ThemeSpec& theme = UseTheme();
  return Text(UserLabel(user_id)).With(Foreground{theme.colors.primary});
}

return MaterialTheme {
  BrandContent(user_id),
};
```

Theme does not retain a content factory and there is no separate Theme macro.

### Theme resolution

Final component resolution follows a fixed order:

```text
generic initial values
  -> component Theme/default projection using component declaration configuration
  -> ordered property modifiers
```

`LayoutValue` remains a separate parent-child metadata and component-layout configuration channel rather than a ViewProperties precedence layer.
Component-owned layout configuration may be completed with resolved Theme values during its default projection.

A complete Theme establishes a design system boundary. A Theme override inherits unspecified values from its parent. Runtime does not branch on Material, flat, liquid, or third-party theme identity.

`ThemeDefinition{ThemeSpec}` establishes a complete boundary. `ThemeDefinition{}` only contributes its typed component values, so a nested style override does not replace the parent `ThemeSpec`. Text, Button, Dialog, Toast, SnackBar, ScrollBar, and default indications derive their semantic defaults from the nearest complete `ThemeSpec`. Component style lookup stops at that complete boundary, while a component-only `ThemeDefinition` continues to inherit from its parent.

Built-in component modules supply one internal defaults operation on their ViewSpec declarations rather than adding concrete component branches to Runtime or inserting synthetic defaults into the user modifier sequence. A third-party composed component defines a typed style value, registers it with `ThemeDefinition::Set()`, and reads it with `UseEnvironment<CustomStyle>()` inside its composable function. It does not register a global NodeKind or extend a Runtime style table.

Built-in elevation styles keep `Shadow::offset` and `Shadow::spread` at zero so elevation remains a platform-neutral ambient effect. Custom drawing and explicit `Shadow` modifiers retain directional offset and spread when a design calls for a drop shadow rather than semantic elevation.

Material theme definitions map stable semantic roles into typed component styles. Surface-container colors, typography roles, and shape roles remain in `ThemeSpec`; control geometry, component-specific disabled colors, interaction target sizes, presentation motion, and surface composition remain in their owning styles. Runtime and platform renderers receive only the resolved View and PaintCommand data and never branch on Material identity.

Text uses `TextRole::Body`, `TextRole::Label`, and `TextRole::Title` to select the corresponding typography token. A component `TextStyle` value can still replace the complete Text style for a local subtree.

Theme switching initially updates values directly. Per-frame animated Theme interpolation is intentionally deferred.

## RuntimeRoot and the layer stack

`RuntimeRoot` owns the application content and one shared layer stack. This stack is the only global presentation container.

### LayerStack ownership and ordering

`RuntimeRoot` keeps three fixed internal children so full-window paint, application-safe layout, and presentation invalidation remain independent.
The application content boundary contains at most the one composed application root and is not a public host, slot, scope, or portal abstraction:

```text
RuntimeRoot
|-- window system-bar backplane
|-- application content boundary
|   `-- application root view
`-- LayerStack
    |-- Presentation entries
    |-- Notification entries
    `-- System entries
```

The backplane and LayerStack always use the complete viewport.
The application content boundary consumes all safe-area edges in the default `SafeArea` mode and leaves them available to application layouts in `EdgeToEdge` mode.
See [Window Insets and System Bars Design](window-insets.md) for the exact propagation and appearance contract.

Layer ordering describes broad drawing levels rather than concrete presentation components:

```cpp
enum class LayerLevel {
  Presentation,
  Notification,
  System,
};

enum class LayerPointerPolicy {
  PassThrough,
  Content,
  Barrier,
};

enum class LayerCancelPolicy {
  PassThrough,
  Consume,
  Dismiss,
};
```

`Presentation` contains Dialog, BottomSheet, Popup, and Menu entries. Entries at the same level follow attachment order, so a Menu opened from a Dialog appears above that Dialog. `Notification` contains transient feedback such as Toast and SnackBar. `System` contains ordinary HuxerUI diagnostic UI such as the debug ribbon and performance panel. Runtime-owned `FrameworkOverlay` content, including text-selection handles and the editing toolbar, remains outside the public layer stack and is painted after it.

Layer options separate stacking, pointer behavior, focus containment, and dismissal:

```cpp
struct LayerOptions {
  LayerLevel level = LayerLevel::Presentation;
  LayerPointerPolicy pointer_policy = LayerPointerPolicy::Content;
  bool trap_focus = false;
  bool dismiss_on_outside_press = false;
  LayerCancelPolicy cancel_policy = LayerCancelPolicy::PassThrough;
  std::function<void()> on_dismiss_request;
  std::optional<Color> barrier_color;
};
```

Pointer `PassThrough` never participates in hit testing. `Content` allows uncovered areas to reach lower layers. `Barrier` consumes input outside the presented content and optionally requests dismissal. A dismissible or colored barrier requires `Barrier`.

Back routing checks the framework-owned text-selection overlay first and then visits public layers from top to bottom. `LayerCancelPolicy::PassThrough` continues to a lower entry, `Consume` stops without dismissal, and `Dismiss` invokes `on_dismiss_request` or removes the entry when no callback is present. Dialog, BottomSheet, Popup, and Menu map `dismiss_on_cancel = false` to `Consume`, so a visible interactive presentation never lets Back close content behind it or leave the system window. Toast, SnackBar, and passive diagnostic content pass through. [Navigation](navigation.md) extends this Runtime-owned chain after layers with application Back handlers, nested page stacks, and a captured predictive Back transaction. Only a completely unhandled request reaches the platform fallback.

Desktop adapters map Escape through key dispatch. Android's full-screen `HuxerUIActivity` owns one lifecycle-bound Back callback, maps API 34 predictive phases to `BackEvent`, and asks Runtime before invoking its platform fallback. API 23 through 33 and an embedded `HuxerUIView` retain the Commit-only `handleBack()` entry point. Runtime never pushes Back-handler state into a platform adapter.

Focus follows actual paint order rather than raw insertion order. Layer options and application-owned modal surfaces such as a presented Drawer project onto the same internal node-level focus trap. A closing modal retains its trap until its exit animation finishes. The topmost enabled trap excludes lower entries and application content from focus traversal while still allowing higher System content to interact. Dismissing Menu over Dialog restores Dialog focus; dismissing Dialog or Drawer then restores the previous application focus when that node is still valid.

`LayerController::State` owns layer entries, identifiers, and attachment sequence. `LayerController` mutates that shared state directly and asks Runtime to invalidate the layer stack. Runtime owns the corresponding mounted nodes, layout, interaction tree, and RenderScene state. Disconnecting the controller clears retained factories and makes copies that outlive Runtime fail safely.

Application and layer invalidation remain separate:

```text
application_dirty -> compose the application root factory
layers_dirty      -> reconcile ordered LayerStack entries
dirty scope       -> recompose only that mounted scope
```

Attaching, updating, or dismissing a LayerEntry must not execute the application root factory. Each entry owns an independent `RecomposeScope`. Application composition may attach an entry that is included later in the same frame. Mutations after the layer snapshot schedule another frame instead of recursively composing layers. The mounted Layer entry records the id, exit participation, and semantic modal-group identity from that snapshot, so geometry and semantics in one FrameCommit never mix the mounted tree with newer controller state.

Concrete presentation policy remains outside Runtime. Typed per-window services build entries on the common controller:

```text
UseToast()       -> Notification, pass-through, timed bottom placement
UseSnackBar()    -> Notification, interactive content, one active bottom placement
Tooltip          -> Notification, anchored modifier-owned plain text
UseDialog()      -> Presentation, modal barrier, theme-controlled vertical placement
UseBottomSheet() -> Presentation, modal barrier, bottom content
UsePopup()       -> Presentation, anchored arbitrary content
UseMenu()        -> Presentation, anchored menu semantics and focus
```

These typed handles are the primary public interaction model for command-oriented presentation. Tooltip uses the same LayerController through a retained target modifier and private per-window service because it is target-owned behavior rather than an imperative action. Having several discoverable `UseXxx()` functions does not create several layer systems; each service shares LayerController, ordering, Environment capture, focus, input, and invalidation. The design does not add a generic `UsePresentation()`, public `UseModal()`, `UseLayers()`, or declarative portal solely to reduce the number of typed entry points.

`UseXxx()` captures the current Environment while composing and returns a lightweight handle that can be retained by an event callback. Showing content later uses that captured Theme, Locale, resources, and third-party values. Services installed through RootHook use the root Environment unless their typed handle captures a narrower one.

Popup and Menu handles expose a retained anchor modifier and point-based presentation:

```cpp
auto menu = UseMenu();

return Button("More")
    .With(menu.Anchor())
    .OnClick([menu] {
      menu.Show({
          MenuItem("Rename", [] {}),
          MenuSection{},
          MenuItem("Delete", [] {}),
      });
    });
```

The anchor modifier records final presentation geometry without creating a layer. `Show()` attaches the entry and follows the complete node bounds, `ShowAtAnchor()` follows a validated node-local rectangle, and `ShowAt()` uses a fixed window point. The retained anchor stores only the local rectangle and latest resolved transform values; `PopupHandle::UpdateAnchor()` updates a LocalRect entry without changing its layer id or content instance and rejects stale identifiers or other anchor modes. `PopupHandle::Update()` replaces content and its captured Environment without changing the active layer id, anchor placement, or dismissal options. `retain_anchor_focus` snapshots the mounted anchor identity into the layer entry; pointer focus resolution keeps that anchor only when popup content has no focusable target and the anchor still owns focus. Each Popup or Menu handle retains at most one active entry; presenting through it again dismisses the previous entry before attaching the replacement. `PopupContext` dismisses arbitrary popup content directly, while Menu leaf actions dismiss the complete open menu chain automatically. Anchor movement invalidates only the corresponding layer entry placement, settles that layout path before the current frame commit, and damages the old and new bounds; anchor removal dismisses node-bound and local-rectangle entries while fixed-window entries remain independent. Placement combines a preferred side, cross-axis alignment, gap, offset, viewport margin, opposite-side fallback, and final clamping without introducing a general cross-tree layout dependency.

Menu is structurally distinct from Popup. Its public input is a recursive sequence of `MenuEntry` values created implicitly from `MenuItem` and `MenuSection`. Menu items directly contain either an action or another entry sequence, while `MenuSection{}` is a non-interactive logical boundary whose visual treatment belongs to the theme. Items retain resource identifiers and image assets as semantic values; the presentation service resolves resources from the captured Environment and composes themed surfaces and interaction. The root menu owns the transparent outside-press barrier. Submenus are content-only anchored layers, so their parent menu remains interactive; Back closes the deepest open level, the default outside-press behavior closes the complete chain, and opening another submenu replaces only that level and its descendants. Arbitrary custom anchored content remains a Popup responsibility.

Dialog and BottomSheet use their own typed handles rather than a shared public Modal mode. They share private barrier, focus, Cancel, dismissal, Environment, and retained Layer transition machinery, while their layout, surface, motion, and options remain component-specific. Dialog resolves placement and motion from `DialogStyle`, while BottomSheet owns an adaptive-width bottom surface that translates from the window edge. When its style exposes a drag handle, a retained handle extension captures the pointer and shares its downward offset with the surface motion extension; cancellation or a short release settles to the edge, while a release beyond the bounded distance threshold follows the layer's `on_dismiss_request` contract and settles if that request leaves the layer visible. The command-oriented `UseDialog()` path remains the primary ergonomic model.

The built-in debug overlay attaches one persistent System entry after root hooks have installed application services and global components. Its dark-red top-right `DEBUG` ribbon toggles an upper-left metrics panel within the entry's own state. Both are composed from ordinary Views against the complete viewport without applying safe-area or title-bar insets; the ribbon is one rotated component clipped by the viewport rather than separately positioned background and label geometry. Toggling or sampling the panel must not reconcile the application root or damage the full viewport. Runtime records painted-frame count, frame-commit time, and damage ratio in a dedicated debug metrics state. PlatformAdapter optionally supplies cumulative process CPU time, a platform-preferred process-memory footprint, and logical processor count so interval utilization can be derived without platform state leaking into LayerController.

The sampling modifier is mounted only with the expanded panel. It wakes once per second and updates the panel's local scope. That update is an ordinary painted frame, keeping the metric tied to actual work without coupling Runtime accounting to the overlay's reconciliation timing. Collapsing the panel removes the modifier and its deadline, so a static application does not animate merely because the debug ribbon is enabled.

LayerController entries without a transition are removed immediately. Dialog, BottomSheet, Menu, Toast, and SnackBar entries with configured motion first become non-interactive, retain their presentation state through the exit animation, and are removed after completion. Modal barriers remain until actual removal, so focus cannot be restored and content behind a visually exiting modal cannot be activated early.

## RootHook

A RootHook installs per-window services or persistent global components before the first application composition:

```cpp
using RootHook = std::function<void(RootContext&)>;
```

`RootContext` exposes root services, layers, and the narrow platform registration operations:

```cpp
class RootContext {
public:
  template <class Service>
  void Provide(std::shared_ptr<Service> service);

  LayerController& Layers();

  template <class Module, class Factory>
  void RegisterPlatformModule(std::string name, Factory factory);

  template <class Module, class Options, class Factory>
  void RegisterPlatformModule(std::string name, Factory factory);

  template <class Properties, class Factory>
  void RegisterPlatformView(std::string name, Factory factory);

  template <class Properties, class Controller, class Factory>
  void RegisterPlatformView(std::string name, Factory factory);

  template <class Module>
  Module OpenPlatformModule(std::string name);

  template <class Module, class Options>
  Module OpenPlatformModule(std::string name, Options options);
};
```

These operations forward to the surface-owned internal `PlatformRegistry` and expose no generic lookup or registry accessor.
They do not discover compile-time libraries, download dependencies, expose system handles, or provide an application-facing string service lookup.

Installation uses `AppOptions`:

```cpp
const Application application{
    App,
    {
        .root_hooks = {
            InstallXxxToast(),
        },
    },
};
```

A service hook can be a function:

```cpp
RootHook InstallXxxToast(XxxToastOptions options = {})
{
  return [options](RootContext& root) {
    root.Provide(
        std::make_shared<XxxToastService>(
            root.Layers(),
            options));
  };
}
```

A persistent global component can attach through `LayerController`:

```cpp
RootHook InstallGlobalBanner()
{
  return [](RootContext& root) {
    root.Layers().Attach(
        LayerOptions{
            .level = LayerLevel::System,
            .pointer_policy = LayerPointerPolicy::PassThrough,
        },
        GlobalBanner);
  };
}
```

Services are stored in the root Environment and retrieved through a typed helper:

```cpp
auto service = UseService<XxxToastService>();
```

Duplicate service types are rejected rather than silently replaced.

Root hooks run once in declaration order. Runtime owns the provided services and attached entries. On window destruction, Runtime removes content and layers before destroying services in reverse registration order. A service uses its destructor to release external subscriptions.

HuxerUI installs its built-in Toast, SnackBar, Tooltip, Dialog, BottomSheet, Popup, and Menu services for every Runtime before application root hooks run. Applications use command-oriented services through their typed `UseXxx()` handles, while Tooltip remains an ordinary retained modifier; root hooks remain the extension mechanism for third-party services and global components. When `AppOptions::show_debug_overlay` is enabled, Runtime installs the built-in DebugOverlay after all root hooks so its System entry remains above other global layers. The option defaults to enabled in Debug builds and disabled in Release builds.

RootHook does not provide:

- Direct Runtime access.
- Direct MountedNode insertion.
- Per-frame callbacks.
- Root replacement.
- Dynamic installation and removal.

## Theme-driven presentation policy

The shared LayerStack foundation owns presentation lifetime, ordering, focus, barriers, Cancel routing, outside-press handling, Environment capture, and removal. It must not also define a single visual structure for every Theme.

Presentation is divided into three contracts:

```text
semantic request
    Dialog title, message, and actions
    Menu items, sections, and submenus
    Tooltip message and target bounds
    Toast message
    SnackBar message and optional action
        -> theme presentation policy
    structure, surface, geometry, placement, and motion
        -> window presentation lifetime
    Layer entry, focus, barrier, dismissal, and scheduling
```

Runtime and LayerController only implement the final contract. Typed services resolve the captured Theme, compose the themed content, and attach it to the shared LayerStack. No layer or Runtime code checks whether a Theme is Material, Flat, iOS, MIUI, or third-party.

`ThemeDefinition` continues to carry typed component styles. A presentation style is a complete value description rather than only a color bundle. Depending on the component, it may describe:

- Surface background, shape, shadow, and size constraints.
- Typography, padding, spacing, alignment, and action arrangement.
- Separator policy and item treatment.
- Default window or anchor placement and viewport margins.
- Enter and exit motion.

The framework composes these semantic values through ordinary HuxerUI Views. A Theme does not receive arbitrary Layer access, own dismissal callbacks, or replace a presentation service. Theme values also do not contain application actions.

`PresentationMotion` is a public Theme value shared by presentation styles, while motion execution remains private to presentation. An absent optional motion disables the transition; otherwise neutral scale and slide values express a fade, and non-neutral values add scale or placement-relative translation without a second motion-kind hierarchy. The implementation interpolates opacity, scale, translation, and transform origin through `AnimationSpec`, retained Layer transition state, and presentation properties. Dialog, Menu, Toast, and SnackBar derive motion from their styles; BottomSheet maps its component-specific motion values into the same private executor.

Menu motion direction and transform origin derive from the requested anchor placement. The origin does not follow a runtime fallback to the opposite side because the resolved side belongs to LayerStack layout rather than the semantic Menu request.

Theme policy does not erase semantic component identity:

- Dialog remains modal content with focus containment and a barrier.
- BottomSheet remains an edge-attached modal surface.
- Menu remains an anchored semantic item hierarchy.
- Popup remains arbitrary anchored content.
- Toast remains a transient notification.
- SnackBar remains actionable transient feedback.

Custom View factories are escape hatches for application-specific content. They still receive themed outer placement, scrim, and motion where appropriate, but they do not implicitly receive the standard component's surface, padding, or internal layout.

### Standard Dialog model

Dialog supports standard title-and-message requests in addition to custom View factories. A standard request has one positive action and may have one negative action. Empty callbacks retain the normal dismissal behavior without adding application work.

The standard model allows Theme to select a platform-appropriate arrangement without inspecting application content:

- Material can use a centered surface with trailing horizontal actions.
- Flat can use a compact desktop surface and its own button treatment.
- iOS can center content blocks, stretch actions, and place separators between them.
- MIUI can use a different viewport position and slide or scale motion.

Positive and negative semantics provide styling and arrangement information without exposing a general action model. Either action requests dismissal through the normal retained exit path and then invokes its non-empty callback; actual Layer removal still completes after the exit animation. A custom interaction that must keep the Dialog open uses the custom `DialogFactory` form.

The command-oriented API provides compact overloads for the common single-action case:

```cpp
auto dialog = UseDialog();

dialog.Show("Network unavailable", "Check your connection and try again.");

dialog.Show(
    "Save changes?",
    "The current document has unsaved changes.",
    "Save",
    [] {
      SaveDocument();
    });
```

Both overloads construct the same internal standard Dialog request. They do not bypass Theme resolution or create another service path. Public parameter naming follows `positive`, `negative`, `on_positive_click`, and `on_negative_click`.

`Show(title, message)` creates one default positive action whose only behavior is dismissal. Supplying a positive label and callback adds application behavior to that same action. The two-action overload requires both labels so it cannot be ambiguous with the compact form.

An empty positive label resolves `dialog_ok` from the built-in `huxerui` resource namespace, while an empty negative label in the two-action overload resolves `dialog_cancel`. Direct Runtime integrations provide the merged resource package required by ordinary StringResource values. Explicit `StringResource` inputs resolve through the same resource context as other deferred presentation content.

The two-action form remains compact:

```cpp
dialog.Show(
    "Delete item?",
    "This action cannot be undone.",
    "Delete",
    "Cancel",
    DeleteItem
);
```

`StringVariant` is the shared deferred display-string representation for component, validation, semantics, and presentation APIs that accept either direct text or a `StringResource` plus positional arguments. Shared resource resolution resolves it under the effective mounted Environment, while `UseString` is the explicit composition adapter for application logic that needs immediate UTF-8.

`DialogStyle` is the complete standard Dialog presentation policy. It covers the modal scrim, default placement, viewport margins, enter and exit motion, surface appearance and width constraints, content padding and alignment, title and message styles, action direction and alignment, positive and negative action appearance and indication, and action separator policy.

The existing custom factory remains available:

```cpp
dialog.Show([](DialogContext dialog) {
  return CustomDialogContent(dialog);
});
```

Typed arguments follow the factory:

```cpp
dialog.Show(CustomDialogContent, document_id);
```

When custom presentation options and bound arguments are both required, a lambda keeps the existing `(factory, options)` ordering explicit without adding another overload family.

This path uses themed modal placement, scrim, and motion but leaves the custom content's own surface and internal layout untouched.

Declarative custom Dialog presentation and command-created Dialogs share style resolution, Layer entry, and the retained dismissal path. The declarative modifier accepts custom content; its visibility remains controlled state, so outside press and Cancel request a source-state update rather than directly overriding it.

### Menu presentation policy

Menu already receives semantic `MenuItem` and `MenuSection` values. `MenuSection` remains a logical boundary: Theme may render it as a separator, spacing, or no visible element.

`MenuStyle` controls menu surface and item treatment, including foreground and background colors, item indication, shape, shadow, icon geometry, padding, minimum metrics, separator policy, and root or submenu motion. The menu surface clips descendants to its rounded bounds so edge-to-edge item feedback cannot escape the shape. `MenuOptions` owns call-specific anchor placement, gap, viewport margin, offset, and width decisions.

The service retains ownership of submenu chains, focus, outside press, Cancel routing, action dispatch, and automatic chain dismissal. Theme cannot change those behavioral guarantees.

### Toast presentation policy

`ToastStyle` controls surface background, text style, padding, shape, shadow, maximum width, viewport margins, top or bottom placement, and enter and exit motion. Toast duration, timed dismissal, queueing, and deduplication remain service policy rather than Theme values.

Toast stays passive and message-only. Actionable feedback uses SnackBar rather than adding a second interaction mode to Toast.

### SnackBar presentation policy

`SnackBarStyle` controls the surface, message typography, action appearance, adaptive spacing, size constraints, viewport margins, and enter and exit motion. `SnackBarOptions` owns request duration, including an indefinite presentation when duration is absent.

The SnackBar service owns one active request per Runtime. A new request creates a new Layer id and atomically replaces the previous Layer entry, so no frame exposes two SnackBars and stale ids, timers, or action callbacks cannot affect the replacement. An action first dismisses its owning request and then invokes the application callback, which makes a callback that immediately shows another SnackBar well-defined.

Timed dismissal uses retained frame timing rather than an application thread. It pauses while the surface or action is hovered, while the action is focused or pressed, and while the application is not active. SnackBar remains non-modal, does not trap or steal focus, and passes Back to lower presentation and navigation handlers.

## Toast

Toast is naturally command-oriented:

```cpp
auto toast = UseToast();

return Button("Save")
    .OnClick([toast] {
      toast.Show("Saved");
    });
```

`UseToast()` returns a lightweight handle bound to the current window and captures the current Environment. A Toast shown from a nested Theme uses that Theme by default.

The Toast service creates one LayerEntry per call and manages its duration. The Runtime layer stack owns composition, input behavior, and removal. Queueing and deduplication are deferred policies.

There is no process-global `Toast::Show()` because it would be ambiguous in multi-window and multi-Runtime applications.

## SnackBar

SnackBar is a separate command-oriented service for a message with at most one action:

```cpp
auto snack_bar = UseSnackBar();

snack_bar.Show("Item deleted", "Undo", [snack_bar] {
  RestoreItem();
  snack_bar.Show("Item restored");
});
```

`UseSnackBar()` captures the current Environment and returns a lightweight handle bound to the current window. `Dismiss` accepts only the id of the currently active request. Replacement and action dismissal begin the normal retained exit path, while replacement exchanges the visual Layer entry atomically instead of waiting for the old exit animation.

## Dialog

Dialog supports both declarative and command-oriented usage.

Declarative presentation is a modifier:

```cpp
return Content().With(
    Dialog {
        .visible = show_dialog,
        .content = ConfirmDialog,
        .dismiss_on_outside_press = true,
        .on_dismiss_request = [show_dialog] {
          show_dialog = false;
        },
    });
```

`DialogExtension` owns a LayerEntry handle. Updating the modifier updates the entry and can reverse an in-progress exit from its current presentation value. Destroying the source modifier requests the same retained dismissal used by command-created dialogs.

An outside press requests dismissal instead of directly removing a declarative Dialog layer. The callback updates the source State, preserving one source of truth for both the component and layer stack. A dismissible declarative Dialog must provide `on_dismiss_request`.

Command-oriented presentation uses a per-window service:

```cpp
auto dialog = UseDialog();

return Button("Delete")
    .OnClick([dialog] {
      dialog.Show([](DialogContext dialog) {
        return Column {
          Text("Delete item?"),
          Button("Cancel").OnClick([dialog] {
            dialog.Dismiss();
          }),
        };
      });
    });
```

`DialogContext` identifies the presented instance and lets command-created content dismiss itself without capturing a `LayerId` before `Show()` returns.

Both forms use the same modal LayerEntry implementation:

- Modal barrier.
- Focus capture and restoration.
- Outside-press dismissal policy.
- Captured Environment and Theme.

Dialog does not own a separate Runtime or presentation host.

## Theme and global presentation

Root services are installed before application composition and inherited through nested Environments.

A global presentation handle obtained inside themed content captures the caller Environment:

```cpp
[[huxerui::composable]]
View AppContent()
{
  auto toast = UseToast();

  return Button("Save")
      .OnClick([toast] {
        toast.Show("Saved");
      });
}

View App()
{
  return MaterialTheme {
    AppContent(),
  };
}
```

The resulting Toast entry receives the Material Theme frame even though it is mounted in the window layer stack outside the normal content layout hierarchy.

A presentation API may explicitly request the root Theme for application-wide alerts, but caller Theme is the default.

## Extension map

The current extension points are:

| Requirement | Extension mechanism |
| --- | --- |
| Custom layout | `Layout<Derived>`, `LayoutContext`, `LayoutResult` |
| Custom virtual container | `VirtualLayout<Derived>` and `VirtualLayoutContext` |
| Custom event | `Event<Result(Arguments...)>`, `On<Key>()`, `UseEvents()`, and `Emit<Key>()` |
| Component external resource lifetime | `Lifecycle(setup, dependencies...)` |
| Composition-owned asynchronous work | `Task<T>`, `UseTaskScope()`, `TaskScope::Launch()`, `RunWorker()`, and `WorkerSequence` |
| External-thread UI handoff | Lifecycle-bound `TaskScope::Post()` |
| Custom View effect | Modifier value and `NodeExtension` |
| Custom animation | `AnimationSpec` or animated modifier value |
| Custom interaction visual | `Indication` and `NodeExtension::OnInteraction` |
| Custom text input or selection | `TextInputClient`, `TextSelectionClient`, and `NodeExtension` |
| Custom theme | Typed values in `ThemeDefinition` and a direct Theme View boundary |
| Per-window service | RootHook and `RootContext::Provide()` |
| Platform nonvisual session | Registered PlatformModule opened by Lifecycle setup or a RootHook owner |
| Global component | RootHook and `LayerController` |
| Typed presentation library | A service backed by the Runtime LayerStack |
| Platform interactive hierarchy | PlatformView factory, PlacePlatformViewCommand, and internal RenderComposition |
| Live camera or video content | ExternalTexture and DrawExternalTextureCommand |

Built-in and third-party implementations use the same lifecycle and storage models.
The semantics extension keeps the same model: a reusable `Semantics` property modifier supplies author declarations, while a semantic-capable NodeExtension may contribute dynamic properties, stable virtual semantic children, and semantic-only action handling.
Runtime resolves both into one committed tree instead of adding a separate accessibility plugin host.
See [Semantics and Accessibility Design](semantics.md).

## Performance rules

The architecture follows these rules:

- Animation advances mounted state and does not recompose components every frame.
- Node extension frame traversal skips subtrees that contain no retained extensions after the extension-tree cache is rebuilt.
- Delayed animation work schedules one wake-up instead of polling.
- Mounted nodes and layers retain their effective shared Environment, while RecomposeScopes subscribe to the exact typed entries consumed during ViewSpec compilation.
- Layer entries use independent scopes.
- ScrollBar state exists only on Views that install the modifier.
- Pointer interaction state is stored per pointer ID.
- Explicit style values override Theme without mutating Theme.
- A service belongs to one window root.
- External texture frames invalidate visible destinations without recomposition or PaintSequence recording.
- PlatformView updates diff committed identities and property revisions instead of recreating PlatformView instances every frame.
- PlatformView composition reuses compatible platform slice representations, avoids per-slice surfaces on single-surface adapters, and does not split a scene that contains no PlatformViews.

Incremental layout and retained rendering are specified separately in [Incremental Layout and Rendering Design](incremental-rendering.md).
The implemented pipeline coordinates mounted geometry, extension painting, Runtime frame output, and platform renderers under that contract.

## Deliberately omitted abstractions

The current design does not introduce:

- `ModifierHost`.
- A Lifecycle modifier or node-bound component effect.
- A context class for every modifier lifecycle phase.
- Runtime branches for ScrollBar, Ripple, Dialog, or concrete animations.
- `OverlayBehavior`.
- Separate Overlay and Presentation runtime trees.
- A Host type for every global component.
- `AppFeature` or `MountedRootFeature`.
- `RootRegistration`.
- A public parallel ServiceRegistry.
- A public Library base class or runtime plugin registry.
- A generic platform-handle variant shared across platforms.
- Per-frame pixel callbacks through Runtime.
- Theme class inheritance.
- Runtime checks for Material, flat, liquid, or third-party themes.
- Process-global Toast or Dialog singletons.
- Dynamic RootHook installation and removal.
- Arbitrary numeric layer z-index.
- Automatic interpolation of an entire Theme.
