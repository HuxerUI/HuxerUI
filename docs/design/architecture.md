# Architecture Design

Status: implemented foundation with deferred follow-up work

This document describes the implemented modifier, animation, interaction, theme, presentation, and root extension foundation, followed by explicitly identified follow-up work. Code examples in implemented sections match the current public API.

Current implementation status:

- Generic View modifiers, node extension reconciliation, frame callbacks, pointer observation, foreground painting, and third-party descriptors are implemented.
- ScrollBar animation, hit testing, dragging, and painting are implemented as a node extension without Runtime feature branches.
- Typed Environment, direct Theme providers, nested Theme propagation, and reduced-motion animation resolution are implemented.
- RuntimeRoot, LayerStack ordering, independent application and layer invalidation, RootHook services, and typed presentation handles are implemented.
- Dialog, BottomSheet, Popup, Menu, and Toast share that LayerStack foundation. Standard Dialog structure and Dialog, BottomSheet, Menu, and Toast visual policy resolve from Theme, and a visible BottomSheet handle owns shared drag-to-dismiss interaction.
- Tween and spring animated Offset, Opacity, Scale, and Rotation values, state-overlay indication, and multi-pointer ripple indication are implemented.
- Node-local PaintSequence recording and reuse, stable RenderNode ownership and revisions, retained group opacity, RenderScene publication, damage calculation, and renderer traversal are implemented.
- Platform-neutral semantic declarations, immutable `SemanticFrame` publication, basic component defaults and action routing, NodeExtension virtual semantic children, and native accessibility bridges on Android, iOS, macOS, and Windows are implemented. Complete component semantics and the remaining native adapters are follow-up work.
- Compile-time module acquisition, ordered resource merging, `PlatformPayload`, the low-level PlatformView leaf, `PlacePlatformViewCommand`, shared `RenderComposition` derivation, per-surface factory registration, and the nonvisual `PlatformInstance` Call, Result, Event, Cancel, and Dispose protocol are implemented. `ExternalTexture`, its closed PlatformPayload capability, Image composition, retained frame scheduling, revision damage, and explicit renderer command boundary are implemented. macOS and iOS provide independent native `CVPixelBuffer` sources and Core Image frame import, Android provides a native `Bitmap` source and Canvas frame import, and Linux provides copied RGBA/BGRA pixel sources and Cairo frame import; Windows and Web native producer and renderer paths remain proposed. Android, macOS, iOS, Web, and Windows provide owning-thread dispatch and native PlatformView hosting with shared ordering and focus synchronization; Linux provides owning-thread dispatch for nonvisual modules. Android, iOS, macOS, and Windows also attach native PlatformView accessibility beneath semantic anchors, while the Web accessibility bridge remains proposed. Android, iOS, Linux, macOS, Windows, and Web provide nonvisual timer reference integrations behind one typed Root Service. Applications install module RootHooks explicitly. Production nonvisual modules, native PlatformView hosting and matching bridges on the remaining platforms, remaining ExternalTexture phases, and native dependency projection preserve one shared Runtime and keep native objects inside platform adapters and module implementations.
- General View exit transitions, keyframes, decay animation, advanced Toast queue policy, and profiler timelines remain follow-up work. Dialog, BottomSheet, Menu, and Toast already retain their Layer entries through component-specific exit motion when their active style enables it.

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

Modifiers are processed from left to right, but the current property modifiers do not form wrapper nodes. `Padding`, `Frame`, `Background`, `Foreground`, `FontSize`, alignment, spacing, and similar values apply directly to `ViewSpec`. A later modifier that writes the same property wins. `Frame` merges only explicitly supplied width, height, minimum, and maximum fields so independent declarations can constrain separate axes.

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

Flow uses the same public `Layout<Derived>` protocol as Row, Column, and Stack. It first measures children at their natural widths to form horizontal lines, then distributes finite remaining width among Grow children within each line. Main alignment is resolved separately per line, cross alignment applies within the line height, and the common Spacing value is used for both item and line gaps. An unbounded width produces one intrinsic-width line without Grow expansion.

Retained modifiers such as `ScrollBar`, `Indication`, animated `Opacity`, and third-party modifiers with an extension preserve their relative order. Compatible retained entries reconcile by descriptor and position. Frame and foreground paint callbacks run in declaration order, while extension hit testing runs in reverse order.

## Modifier descriptions and node extensions

There are two modifier categories:

- A property modifier applies its value directly to `ViewSpec` and is not retained afterward.
- A retained modifier stores a type-erased `ModifierSpec` in `ViewSpec` and a persistent `NodeExtension` in `MountedNode`.

Conceptually:

```text
Padding / Background ── apply ──▶ ViewSpec properties

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

## NodeExtension lifecycle

`NodeExtension` operates directly on a controlled public `MountedNode`. There is no separate `ModifierHost` and no context object for every phase.

The current public lifecycle is:

```cpp
class NodeExtension {
public:
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

  virtual bool PrepareGeometry(MountedNode& node);

  virtual void OnScrollActivity(MountedNode& node);
  virtual void OnScrollGesture(MountedNode& node, bool active);

  virtual bool HitTest(
      MountedNode& node,
      Point position) const;

  virtual bool HoverHitTest(
      MountedNode& node,
      Point position) const;

  virtual bool HoverWhenDisabled() const noexcept;

  virtual void OnHoverChanged(MountedNode& node, bool hovered);
  virtual void OnFocusChanged(MountedNode& node, bool focused);
  virtual void OnKey(MountedNode& node, const KeyEvent& event);

  virtual PointerResult OnPointer(
      MountedNode& node,
      const PointerEvent& event);

  virtual void Paint(
      const MountedNode& node,
      PaintContext& context) const;

protected:
  void InvalidatePaint();
};
```

`Paint()` is currently a foreground pass after the View content and children. `NodeExtension` does not wrap measure, layout, or paint, and it has no `Next` continuations. Custom child measurement and placement belong to `Layout<Derived>` or `VirtualLayout<Derived>`.

`PrepareGeometry()` runs after final presentation transforms are resolved and before text services and paint consume geometry. It returns true only when the extension's foreground paint inputs changed. This phase lets geometry-dependent extensions retain value snapshots without storing raw mounted-node references or forcing clean PaintSequences to rerecord.

During `Paint()`, extensions append node-local PaintCommands through `PaintContext`. Runtime stores the resulting foreground PaintSequence on the node's RenderNode, and platform renderers apply the inherited layout and presentation transform while traversing RenderScene. Paint may extend beyond `Bounds()` unless an explicit clip limits it, and Runtime derives render visibility from recorded PaintSequence bounds and visible descendants. `PresentationBounds()` is the transformed axis-aligned host-view logical layout bounds. Pointer positions delivered to `NodeExtension::HitTest()` and `OnPointer()` are mapped back into the node's local coordinate space.

An extension whose `HitTest()` returns true keeps its node on the topmost pointer route and prevents lower visual branches from receiving that pointer. Runtime may query `HitTest()` while constructing the route and again before dispatch, so implementations keep it deterministic and free of side effects.

Every matching hover extension on the deepest hit node receives `OnHoverChanged()` rather than competing for one exclusive hover slot.
`HoverWhenDisabled()` opts a hover-only affordance into disabled targets without enabling focus, touch, Click, or other pointer behavior.
An extension that returned `Observe` on pointer down continues receiving the pointer sequence without owning it; returning `CancelTarget` after recognizing a competing gesture sends PointerCancel to the active target and suppresses its activation.

Clean content and foreground PaintSequences remain attached to their stable RenderNode. An extension calls `InvalidatePaint()` after changing paint-visible retained state; the operation invalidates only its owner's foreground sequence and schedules a frame when called outside frame construction.
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

`Bounds()` has a zero origin and the node's layout size. `LayoutOffset()` is parent-relative. `PresentationBounds()` is derived from the committed ancestor transform chain for native-boundary queries and diagnostics.

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
return FrameCommit with RenderFrame and an optional absolute deadline
invalidate and present native damage
schedule the returned deadline
```

The current Runtime reuses clean measurement and placement results, retains clean content and foreground PaintSequences, and changes a RenderNode revision only when its commands or scene properties change. PaintSequence revisions and lightweight committed-scene snapshots produce conservative DamageRegion rectangles. Node-extension frame traversal caches whether a subtree contains any extensions and skips extension-free subtrees. A modifier that is waiting for a delayed transition schedules one wake-up rather than running empty frames.
Runtime may request a platform frame when state changes outside frame construction, but never calls the platform scheduler from inside `BuildFrame()`.
Continuous animation and delayed extension work are returned in `FrameCommit::next_frame_deadline`, allowing each host to present the current commit before arming the next frame.

Runtime calls fixed node and modifier lifecycle functions. It does not contain branches for concrete features such as ScrollBar, Ripple, Dialog, or a particular animation.

The retained scene and incremental invalidation architecture are defined in [Incremental Layout and Rendering Design](incremental-rendering.md).
Local geometry, the scene boundary, PaintSequence reuse, transform and opacity presentation updates, retained ScrollView movement, layout and virtual-realization caching, equality-aware modifier and layout-value diffs, and precise shared-runtime damage are implemented.
macOS and Windows consume shared DamageRegion output for native partial redraw.
Android retains the same shared damage calculation and committed-scene path but currently invalidates its complete native View.

The semantics pipeline is a parallel Runtime output rather than a RenderScene branch.
After reconciliation and final presentation geometry, Runtime resolves component declarations, NodeExtension contributions, application overrides, focus, visibility, and secure-data policy into one immutable owning `SemanticFrame`.
`FrameCommit` publishes a shared pointer to that frame beside `RenderFrame`, allowing native accessibility objects to retain committed data without retaining MountedNode pointers.
The complete declaration, frame, action, identity, virtualization, security, and platform mapping contract is defined in [Semantics and Accessibility Design](semantics.md).

## Platform content integration

Status: shared payload, PlatformView composition, nonvisual instance protocol, and ExternalTexture rendering implemented; Android, iOS, Linux, and macOS native ExternalTexture production and consumption implemented; production modules and remaining native adapters proposed

Native modules produce three integration forms:

| Requirement | Integration |
| --- | --- |
| Permission, Audio, Camera control, or another nonvisual capability | Strongly typed Root Service backed by a registered platform module instance |
| WebView, map, document preview, or another native interactive hierarchy | Registered PlatformView factory and a real leaf View |
| Camera preview, video decode, or another high-frequency visual stream | ExternalTexture composed by the HuxerUI renderer |

The categories may coexist in one module.
A Camera module normally installs a Camera service and returns an ExternalTexture for preview, while a WebView module installs a PlatformView factory.
PlatformView and nonvisual modules share the same registry namespace, payload model, event naming, and lifecycle rules without forcing their different update and request models through one factory type.
This does not introduce a Runtime subclass, a public Module base class, native platform types in shared headers, or an application-visible generic module lookup.

### Platform payload and instance protocol

Direct registration from Objective-C, Swift, Java, Kotlin, JavaScript, C++, and future platform languages requires one value model that every boundary can represent.
`PlatformPayload` is an immutable equality-comparable tree containing null, boolean, signed 64-bit integer, double, UTF-8 string, bytes, list, string-keyed object, and one closed framework capability kind, `ExternalTexture`.
The capability kind does not admit arbitrary objects.
Objects require unique keys and compare independently of insertion order; encoders preserve the distinction between integers, doubles, strings, and bytes rather than routing through JSON.
Strings and object keys must be valid UTF-8, doubles must be finite, and positive and negative zero compare equal.
Construction rejects excessive nesting, while platform decoders enforce input-size limits before allocating containers from untrusted data.
`PlatformPayload` is an in-process platform boundary value rather than a persistence, network, or general serialization format.
It never contains a callback, open-ended C++ type-erased object, native handle, pointer, platform View, or executable closure.
`ExternalTexture` is the only framework-owned capability kind and remains an opaque value rather than opening a generic native-object alternative.

Boundary bridges preserve these kinds directly rather than relying on implicit coercion:

| Boundary | Scalars | Bytes and containers |
| --- | --- | --- |
| C++, Windows, and Linux | `bool`, `std::int64_t`, `double`, and UTF-8 string alternatives | Owned bytes, list, and object alternatives |
| Objective-C and Swift | Kind-preserving NSNumber values and NSString | NSData, NSArray, and NSDictionary with NSString keys |
| Java and Kotlin | Boolean, Long, Double, and String | byte array, List, and String-keyed Map |
| JavaScript and future JS-hosted adapters | Boolean, BigInt, Number, and string | Uint8Array, Array, and a prototype-free string-keyed object |

Decoders require the declared kind and range instead of converting a string to a number, truncating a double to an integer, or treating bytes as text.
Native ExternalTexture bridge phases represent the value with an unforgeable framework wrapper retaining the same opaque source state; they never expose or reconstruct it from a numeric identity.

The shared public surface stays focused as the phases land:

- `<huxerui/platform_module.h>` owns `PlatformPayload`, `PlatformError`, `PlatformModuleFactory`, `UIThreadDispatcher`, the move-only `PlatformInstance`, and the per-surface `PlatformModules` registry.
- `<huxerui/platform_view.h>` owns the low-level `PlatformView` leaf and its event-key declaration API.
- `<huxerui/external_texture.h>` owns the platform-neutral `ExternalTexture` consumer value; platform-specific headers own native frame producers.
- `<huxerui/android/jni.h>` owns move-only JNI local references plus strict UTF-8, Java String, and byte-array conversion for Android module sources; platform-specific `platform_view.h` headers own the Android, Apple, Web, and Windows visual factory contracts.

There is no public PlatformView type tag, declaration wrapper, property base class, callback wrapper, platform-object base class, or parallel dynamic value type.
Platform-neutral implemented headers are re-exported through `<huxerui/huxerui.h>`, while platform-specific factory and producer headers are included directly by native module sources; ordinary applications normally see only a module's typed component and service headers.

The same payload type carries controlled PlatformView properties, nonvisual module creation options, method arguments, method results, event data, structured error details, and opaque ExternalTexture references.
Concrete module APIs remain strongly typed and own their codecs at the module boundary:

```cpp
View WebView(const WebViewOptions& options) {
  return PlatformView(
      "web/WebView",
      EncodeWebViewOptions(options)
  ).Events<
      WebViewEvents::NavigationChanged,
      WebViewEvents::LoadFailed
  >();
}
```

Application code consumes `WebViewOptions`, `NavigationState`, `AudioSource`, and other module types rather than assembling `PlatformPayload` objects or spelling platform type and method names.
The dynamic representation exists only where shared C++ crosses into a platform implementation.
Large or continuous media frames do not travel through PlatformPayload; only an `ExternalTexture` capability may cross that boundary, while its platform-owned streaming path retains all native frame data.

Platform adapters own a per-surface registry with one case-sensitive UTF-8 type namespace.
Platform sources register visual and nonvisual factories explicitly by stable string, for example `web/WebView` or `audio/Player`.
`PlatformModules::Register(type, registration)` stores the platform-specific registration by its concrete C++ type, and the owning adapter retrieves it through `Find<Registration>(type)`.
Visual registrations use the platform-specific `PlatformViewFactory` in the `android`, `ios`, `macos`, `web`, or `windows` namespace. Nonvisual C++ and Apple implementations use the platform-neutral `PlatformModuleFactory`, while Java-backed Android implementations use `android::PlatformModuleFactory` so the owning adapter can inject its retained Context and current UI-thread `JNIEnv`; another registry or registration-kind enum is unnecessary.
Registration callbacks remain in the platform adapter or platform module source and may use native types there; they are not stored in `PlatformPayload` or exposed to shared Runtime code.
`PlatformModules::Open()` owns instance protocol setup and delegates registration-specific creation to its `PlatformAdapter`. The default adapter path creates a platform-neutral factory, while an adapter override may recognize its own registration type before falling back. This keeps native host dependencies in the adapter instead of adding a Context service, hidden opener, thread-local state, or process-global registry.
The registry rejects an empty type, duplicate registration across registration kinds, and retrieving a registered type through an incompatible registration type.
Type strings are module contract rather than application configuration, and modules normally expose them only through their concrete C++ component or service.
Registration is not a process-global static side effect and does not choose a composition mode.
An explicitly selected RootHook connects its platform registrations before opening a nonvisual instance or composing the first PlatformView, and a root cannot replace a registration while an instance of that type is alive.

The host gives each created instance narrow sinks for emitting an Event and completing or failing a Call; PlatformView independently retains its existing presentation invalidation path.
Sink closures route to their owning instance state without exposing Runtime, MountedNode, EventBindings, or HuxerUI application state.
The owning bridge validates payloads at Create, Update, Call, Result, and Event boundaries and converts native failures to `PlatformError`.

The two integration forms use the same message vocabulary while retaining distinct factory contracts:

```text
Create(type, initial payload, event sink) -> native instance
Update(native instance, payload)
Call(native instance, method, payload, result sink) -> optional cancellation operation
Result(result sink, payload or PlatformError)
Event(event sink, event, payload)
Dispose(native instance)
```

PlatformView uses Create, Update, Event, and Dispose, while `PlatformInstance` uses Create, Call, Result, Event, Cancel, and Dispose.
Application callback objects never cross the platform boundary; the host transfers only protocol results and event envelopes.
Calls are asynchronous even when the native implementation answers synchronously, complete at most once, and return a structured `PlatformError` with a stable code, English diagnostic message, and optional PlatformPayload details.
Framework error codes use the reserved `huxerui/` prefix, while modules namespace their own codes; neither side requires an enum that would prevent third-party extension.
C++ exceptions, Objective-C exceptions, Java exceptions, and JavaScript exceptions are converted at their owning boundary and never propagate through another language runtime.

The platform adapter receives an optional `UIThreadDispatcher` during construction; HuxerUI defines its UI thread as the thread owning that adapter, its Runtime, and its event loop.
The dispatcher must enqueue without invoking inline and delivers events in emission order per instance outside frame construction, reconciliation, native drawing, and the initiating native call stack.
Module services call `PlatformInstance::Call`, `On`, `Cancel`, and `Close` only from that UI thread; native Result and Event sinks may be invoked from other threads and cross through the dispatcher.
Call results use request identity and may complete out of call order, but their delivery obeys the same thread and reentrancy boundary.
Events produced while creating a visual candidate remain queued until the candidate enters the committed `RenderComposition`; a failed candidate expires without publishing events.
Compatible Update mutates an existing native instance in place and is not a cross-instance rollback boundary.
A nonvisual instance begins delivering events only after Create succeeds.
`Cancel(request)` removes the completion before invoking the optional native cancellation operation, and a result already queued for that request is subsequently ignored.
Dispose first rejects new calls, cancels pending requests, detaches event delivery, and then releases native state.
Results and events carrying an obsolete instance or request identity are ignored safely.

Platform events retain the existing HuxerUI typed-event model.
A module event key supplies its wire event name and a PlatformPayload decoder in addition to its ordinary `Signature`:

```cpp
struct NavigationChanged : Event<NavigationState> {
  static constexpr std::string_view Name = "navigationChanged";
  static NavigationState Decode(const PlatformPayload& payload);
};
```

The concrete component declares its supported keys through the rvalue-qualified `PlatformView::Events<Key...>()` fluent API, while application code continues to use `.On<WebViewEvents::NavigationChanged>(handler)`.
Runtime resolves an incoming event name only against descriptors attached to that mounted declaration, decodes it, and emits the existing Event Key through the node's EventBindings.
Changing an application callback reconciles EventBindings only; it does not update or recreate the native instance.
An unbound declared event is ignored without decoding.
A duplicate declared event name is rejected as invalid component configuration, while an undeclared incoming event, malformed subscribed payload, or decoder failure is dropped without invoking application code.

Nonvisual method keys follow the same pattern without becoming Event Keys.
A method key declares its request type, result type, stable wire name, encoder, and decoder; a typed service calls `PlatformInstance::Call<Method>(request, completion)` internally and exposes an application-facing asynchronous result in its own API.
The request and result must be object types, the result must be move-constructible and distinct from `PlatformError`, `Encode` returns `PlatformPayload` exactly, and `Decode` may return a type implicitly convertible to the declared result.
Call completions and event handlers must be constructible as the declared typed callback before the template participates in overload resolution.
`PlatformInstance` is the move-only module-author handle returned by `PlatformModules::Open()` and owns the native instance, monotonically assigned request identities, pending calls, and typed event subscriptions.
Its `On<Key>(handler)` operation registers the Key's wire-name and decoder descriptor together with one service-owned handler; it does not expose a raw payload callback to application code.
It is not a generic application service surface.

Nonvisual modules use the same event descriptors and payload codecs behind their typed Root Services.
Runtime exposes the platform-neutral `PlatformModules` capability to RootHook through `RootContext::Modules()`.
A module installer may open a registered native instance and provide a typed service directly:

```cpp
void InstallAudio(RootContext& root) {
  root.Provide(std::make_shared<AudioService>(
      root.Modules().Open("audio/Player")
  ));
}
```

`PlatformModules` is a module-author capability rather than an application service locator.
The resulting service owns the `PlatformInstance`, encodes typed calls, decodes results and events, and closes the instance from its destructor.
An application-wide native engine may remain shared behind several per-window instances, but each Runtime retains only its own identities, subscriptions, and typed services.
The shared protocol and deterministic dispatcher fixture are implemented and tested.
The macOS and iOS adapters configure asynchronous main-queue delivery, Android dispatches through its owning `HuxerUIView`, Linux wakes its X11 event loop through `eventfd`, Windows posts a private message to its owning application HWND, and Web queues work through the browser event loop.
The Windows dispatcher accepts work before that HWND exists because Runtime installs RootHooks before the adapter creates its window, then schedules the queued batch when the window attaches; shutdown drops late native callbacks without retaining the destroyed HWND.
`example_platform_module` registers a Foundation timer on Apple platforms, a Java scheduled timer on Android, a `timerfd` timer on Linux, a thread-pool timer on Windows, and an Emscripten interval on Web behind one typed Root Service to exercise Call, Result, Event, Cancel, and Dispose end to end.
Other production adapters and concrete product modules remain proposed.

### PlatformView

PlatformView is a real built-in leaf View rather than a modifier.
Runtime owns its mounted identity, compatible reconciliation, measurement, final geometry, visibility, hit-testing boundary, focus participation, semantic anchor, and unmount timing.
The platform adapter owns the corresponding `NSView`, `UIView`, Android `View`, `HWND`, DOM element, or future platform object.

The low-level declaration has only the registered type, complete controlled properties, and supported event keys:

```cpp
class PlatformView final : public View {
public:
  explicit PlatformView(std::string type, PlatformPayload properties = {});

  template <class... Keys>
  PlatformView&& Events() &&;
};
```

Default-constructed `PlatformPayload` is null, so `PlatformView("module/Type")` is valid for a factory with no creation properties.
`Events<Key...>()` stores only wire-name and decoder descriptors; application callbacks remain ordinary EventBindings added later through `View::On<Key>()`.
PlatformView participates in shared focus traversal by default, while the ordinary `Focusable(false)` modifier removes a non-focusable native surface from that order.

A module exposes a concrete component such as `WebView()` and internally constructs `PlatformView(type, properties)` with a stable registered type string and immutable PlatformPayload properties.
Compatible recomposition retains the mounted and native instance when the type string and key remain compatible.
A changed type string or incompatible key replaces it, while changed properties update the retained instance after the next successful commit.

PlatformView measurement remains platform-neutral and never creates or synchronously measures a native object during shared layout.
PlatformView has zero intrinsic logical size under loose constraints; ordinary parent constraints and size modifiers such as `Frame` produce its final axis-aligned layout bounds.
A concrete module may require dimensions or derive a Frame from controlled application data.
Native intrinsic-content changes do not mutate mounted geometry behind Runtime or start an adapter-to-layout feedback loop.

PlatformView follows final RenderScene paint order rather than a separate native plane or a component-tree depth number.
Content, children, foreground painting, sibling order, and LayerStack entries therefore determine native composition in exactly the same order as ordinary HuxerUI drawing.
Registration does not select a behind, above, overlay, or texture composition mode, and applications do not move content into LayerStack merely to cover a PlatformView.

The PlatformView leaf records one `PlacePlatformViewCommand` in its retained PaintSequence.
The immutable command carries the stable mounted identity, registered type string, immutable PlatformPayload properties and revision, and the final local axis-aligned destination rectangle.
It carries no native handle and performs no raster drawing.
Only the built-in PlatformView leaf records it; Canvas and public PaintContext do not expose an operation for placing an arbitrary native identity.
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
Adapters reuse the platform-appropriate representation of compatible slices across frames, diff PlatformView identity and property revisions, and mutate the native hierarchy only while applying a committed RenderComposition on the platform UI thread.
Native objects are never inserted, reordered, or removed from `drawRect:`, `onDraw`, `dispatchDraw`, a paint callback, or renderer command replay.
A changed composition creates, updates, positions, clips, shows, hides, reorders, or destroys only the affected native instances and adapter-owned slice representations.
Moving or removing an item damages its previous and current visible bounds without rerecording unrelated clean PaintSequences.
An offscreen or temporarily hidden PlatformView remains mounted and preserves native state; committed removal detaches input and accessibility references before native destruction.
On Windows, a visible removed HWND remains retained behind the previous transparent aperture until the replacement HuxerUI surface is successfully presented, then it is destroyed before the next frame; this prevents the root background from flashing between frame commit and presentation.

The adapter creates a native instance when its identity first enters a committed RenderComposition and applies controlled properties before making it visible.
A property revision sends the complete controlled properties to the compatible instance through an idempotent Update, while bounds-only changes update placement without resending an unchanged property payload.
Factories should validate an Update before mutating observable native state and apply the complete controlled payload idempotently.
Replacement prepares the new instance before retiring the old one.
A factory exception is a module integration error that aborts the platform commit; adapters contain native exceptions at their boundary but do not attempt to roll back arbitrary module-owned native state.
Runtime shutdown detaches input, focus, and accessibility bridges, destroys PlatformViews and adapter-owned composition resources, and only then releases module Root Services in their existing reverse installation order.

Initial PlatformView presentation supports translation, axis-aligned layout, rectangular clipping, visibility, and deterministic ordering among PlatformViews.
Arbitrary rotation, path clipping, group opacity spanning a PlatformView, backdrop filters, and offscreen effects are unsupported until every platform can preserve their semantics.
The framework rejects unsupported declarations instead of approximating them silently.
Exact z-order does not imply support for an otherwise unsupported visual effect.

Native pointer, keyboard, IME, and internal gesture handling remain inside the native hierarchy when the PlatformView is the active hit target.
Runtime hit testing and the committed `RenderComposition` use the same front-to-back order, so visually higher HuxerUI content wins before a covered PlatformView receives native input.
Native slice hosts remain hit-test transparent outside HuxerUI interactive regions instead of blocking the complete PlatformView rectangle below them.
Once the PlatformView wins hit testing, its native hierarchy owns pointer sequences and gestures until completion or cancellation; the host does not duplicate those events into Runtime.
Focus traversal treats the PlatformView as one HuxerUI leaf, and platform focus changes synchronize that leaf without exposing native responder objects.
A focused native text editor owns its native text service; Runtime suspends any HuxerUI text-input session until focus returns.

The semantic tree contains one PlatformView anchor at the mounted position.
The native accessibility adapter attaches the platform object's native accessibility root beneath that anchor, preserves its position among HuxerUI semantic siblings, and excludes duplicate HuxerUI descendants.
Removing or replacing the PlatformView invalidates the bridge before native destruction so retained accessibility references fail safely.

Platform adapters preserve the same contract through platform-specific composition machinery:

| Platform | Composition strategy |
| --- | --- |
| Android | The host is a ViewGroup that alternates HuxerUI slice replay with ordinary child drawing in committed order. A `TextureView` participates as a regular child, while any `SurfaceView` subtree is rejected because its system composition cannot preserve this Canvas order. |
| iOS | Transparent HuxerUI slice views or layers and native UIViews are retained as ordered siblings under one host UIView. CoreGraphics replay targets only damaged slices. |
| macOS | Transparent HuxerUI slice views or layers and native NSViews are retained as ordered siblings under one host NSView. AppKit hierarchy changes occur outside `drawRect:`. |
| Windows | One transparent DirectComposition surface replays every HuxerUI slice, while native child HWNDs remain beneath it. Each placement clears a rectangular aperture in command order, and later HuxerUI drawing may cover that aperture without allocating a surface per slice. |
| Linux/X11 | The adapter uses ordered child windows with suitable ARGB composition or redirects native child content through XComposite. A server without the required composition capability cannot host that PlatformView. |
| Web | HuxerUI Canvas slices and DOM PlatformViews are ordered siblings in one isolated CSS stacking context. The adapter coordinates DOM event targeting with Runtime hit testing. |

These strategies are conformance requirements, not application-selectable composition modes.
A factory whose native object cannot preserve exact ordering on the current platform fails with a diagnostic identifying the PlatformView type or unavailable adapter capability at the layer that detects it.
It must not silently flatten the declaration into a global foreground or background plane, capture an interactive native hierarchy as stale pixels, or discard covering HuxerUI content.
Modules may choose a different native implementation internally, while high-frequency visual content without native interaction remains an ExternalTexture.

Current shared tests cover payload invariants, per-surface registration, leaf layout, identity and property revisions, typed event delivery, frontmost hit testing, basic paint order, adjacent PlatformViews, keyed movement, replacement, and unsupported transforms and opacity.
The macOS integration fixture covers native creation, property update, HuxerUI slice ordering, retained identity, unchanged placement, focus synchronization, accessibility identity resolution, removal, stale-event rejection, and disposal.
Android focused tests cover semantic-anchor encoding, while the Android PlatformView example and device validation cover native creation, controlled updates, slice ordering, hit testing, focus and IME transfer, native accessibility attachment, removal, and recreation.
The Windows integration fixture covers native creation and update, retained identity across hiding, nested HWND focus and UI Automation resolution, stale-event rejection, presentation-delayed retirement, remount, and deterministic disposal.
Later composition phases add content-child-foreground order, nested rectangular clips, visibility, and equivalent adapter coverage as those behaviors land.
Each available platform adds an integration fixture with HuxerUI content below and above one native control, verifies frontmost pointer ownership, native focus and IME transfer, accessibility traversal through the anchor, retained native state across recomposition and temporary hiding, and deterministic teardown.
Future surface-specific tests cover Android `SurfaceView` rejection and X11 child-window composition. Web integration coverage still needs automated DOM stacking, native event, focus-boundary, and disposal checks across supported browsers.

#### Windows PlatformView composition

Status: implemented

The default Windows 10 adapter uses one premultiplied-alpha DirectComposition surface as the complete HuxerUI composition plane whenever the committed scene contains PlatformViews.
It does not create one HWND, swap chain, bitmap, or DirectComposition surface per HuxerUI slice.
The existing non-PlatformView presentation path remains unchanged until this composition mode is committed for the window.

The coordinator derives and consumes the ordered `RenderComposition` for native identity, placement, and z-order without deleting or replacing its slice boundaries.
The single-surface renderer traverses the committed scene once; ordinary HuxerUI drawing remains on that surface, each `PlacePlatformViewCommand` clears its resolved visible rectangle to transparent, and later HuxerUI drawing continues over the aperture.
This is observably equivalent to replaying the ordered slice ranges while avoiding a surface or replay pass per slice, and the retained slice contract remains available if Windows later needs a multi-surface mapping.
Native child HWND z-order follows committed PlatformView order, so a later placement replaces earlier HuxerUI drawing within its aperture while drawing after that placement can cover the native content.
This preserves the shared observable paint order for axis-aligned opaque native islands without pretending that HWND pixels participate directly in Direct2D blending.

Each mounted PlatformView owns one framework-private clipping container HWND.
The factory creates its native root HWND as a child of that container, while the adapter positions the container at the visible rectangle and offsets the native root by the clip origin.
The container performs no drawing, never takes focus, contributes no semantic node, and is not a public composition concept.
It provides a stable ownership and rectangular-clipping boundary without modifying the factory-owned control through `SetWindowRgn`.
Placement and native z-order changes are applied on the window UI thread.

The Windows-specific factory contract lives in `<huxerui/windows/platform_view.h>`:

```cpp
namespace huxerui::windows {

struct PlatformViewFactory {
  std::function<HWND(HWND parent, const PlatformPayload&, PlatformEventSink)> create;
  std::function<void(HWND, const PlatformPayload&)> update;
  std::function<void(HWND)> dispose;
};

} // namespace huxerui::windows
```

The factory must return a same-process, same-UI-thread `WS_CHILD` HWND whose parent is the supplied clipping container.
After successful creation, HuxerUI owns destruction of that root HWND.
The adapter calls `dispose` once before `DestroyWindow`; `dispose` releases module callbacks, subclass state, and other owned resources but does not destroy the HWND.
Creation, update, disposal, placement, and focus changes run on the owning UI thread, while `PlatformEventSink` delivery uses the existing Windows UI-thread dispatcher and drops events from an inactive route.

A single framework-private transparent input-shield HWND covers the client area while PlatformViews are present.
It asks Runtime for the committed frontmost hit target.
When a PlatformView wins, the shield returns `HTTRANSPARENT` so the same-thread child HWND receives ordinary pointer input; when HuxerUI content wins, the shield routes input through the existing adapter path and prevents the covered native control from receiving it.
Pointer capture remains with the side that won the initial sequence.

Runtime-directed PlatformView focus selects the native root or its first focusable descendant.
After native message dispatch, the adapter maps `GetFocus()` back through each hosted root and its descendants and synchronizes the matching mounted identity.
Tab traversal remains within standard native descendants while a next tab stop exists, then crosses the PlatformView boundary through Runtime focus order.
A focused native editor owns Win32 text services and suspends the HuxerUI text-input session until focus returns to a HuxerUI editor.
The UI Automation adapter exposes the hosted HWND provider below the existing PlatformView semantic anchor rather than publishing both as sibling roots.

Windows PlatformViews are opaque rectangular native islands.
A factory that returns a foreign-process HWND, a different-thread HWND, a non-child root, or native content requiring per-pixel composition outside its aperture fails explicitly.
Rotation, path clipping, cross-boundary group opacity, backdrop effects, and other unsupported declarations remain rejected by the shared composition builder before platform mutation.
The Windows 7 compatibility renderer does not silently flatten PlatformViews into a global foreground or background plane; a Windows 7 session that requests this DirectComposition capability fails with an explicit unavailable-capability diagnostic.

### ExternalTexture

Status: shared value, payload, Image, rendering command, scheduling, and damage implemented; Android, iOS, Linux, and macOS native sources and frame import implemented; remaining native sources phased below

`ExternalTexture` is a copyable platform-neutral consumer value representing one live visual source.
It exposes fixed logical intrinsic size, stable identity equality, and validity, while its shared opaque state retains platform-owned frame production and lifetime data.
The public value exposes no frame revision, native texture, buffer, view, device pointer, registry identity, or mutation operation.
Application code cannot construct a valid texture from an integer or native handle; only a platform-specific source creates one.

The platform-neutral public surface remains a value type:

```cpp
class ExternalTexture final {
public:
  ExternalTexture() noexcept = default;

  [[nodiscard]] Size IntrinsicSize() const noexcept;
  [[nodiscard]] bool HasValue() const noexcept;

  bool operator==(const ExternalTexture& other) const noexcept;
};
```

Default construction produces an empty value for optional storage.
PlatformPayload and Image reject that empty value.

A platform source is move-only and may be created before a Runtime or native surface exists.
Its `Texture()` operation returns the copyable consumer value, `Publish()` replaces the pending native frame, and `Finish()` rejects later frames while preserving the last published frame for drawing.
Source destruction performs the same terminal cleanup and is safe even when the texture was never bound to a surface.
The implemented producer surfaces are `<huxerui/android/external_texture.h>`, `<huxerui/ios/external_texture.h>`, `<huxerui/linux/external_texture.h>`, and `<huxerui/macos/external_texture.h>`.
Android accepts retained `android.graphics.Bitmap` frames through the publishing thread's `JNIEnv`, macOS and iOS accept `CVPixelBufferRef` frames, and Linux copies borrowed RGBA8888 or BGRA8888 pixels with explicit dimensions and row stride.
Each platform retains independent source state and renderer integration without widening the platform-neutral consumer representation.
Future Windows and Web producers join the same contract through their own platform headers.

An unbound texture binds exactly once when it first enters a surface-owned PlatformAdapter boundary.
A Result or Event binds before delivery to shared C++, a Call argument or PlatformView property binds or validates against the receiving adapter, and a texture created directly by native module code binds when its first committed render use is collected.
All paths use the same internal surface-binding invariant.
Re-entering the same surface is valid, while using the texture with another surface fails at the owning boundary with a HuxerUI diagnostic rather than rendering an empty result.
There is no public or module-visible texture registry: the source state is the capability, and each renderer keeps only the private cache needed to consume sources already bound to its surface.

`PlatformPayloadKind::ExternalTexture` transports the consumer value through module options, Calls, Results, Events, and PlatformView properties, including nested lists and objects.
Constructing a payload from an empty texture is invalid, and `AsExternalTexture()` requires the exact kind.
Payload equality delegates to texture identity equality; frame publication never changes payload equality.
Platform bridges carry an opaque framework wrapper retaining the source state instead of encoding a raw numeric identifier.
This closed capability does not make PlatformPayload a generic object transport, and PlatformPayload remains explicitly in-process and non-serializable.

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
Intrinsic size is immutable because asynchronously changing it would mutate layout without controlled application state; a structural size or format change produces a new ExternalTexture value.

Painting records a distinct `DrawExternalTextureCommand` in PaintCommand.
The command owns an ExternalTexture value plus source and destination rectangles, sampling, and opacity.
It remains distinct from DrawImageCommand because immutable encoded images use decode caches while an external texture resolves the latest platform-owned frame through its opaque source state.
The command contains no frame revision, so publishing a frame does not make a clean PaintSequence unequal or require rerecording it.
Adding the command requires explicit handling in every renderer; a backend must not silently draw an empty rectangle.

Frame production does not write application State, recompose a scope, or rerecord an otherwise clean PaintSequence.
The source accepts frame publication on the producer thread, atomically advances its private revision, replaces the pending frame, and requests at most one platform frame through the weak scheduler installed during surface binding.
Runtime records visible texture uses while publishing the RenderScene and retains a committed snapshot of each identity, revision, and transformed destination.
On the next BuildFrame it compares the source revisions with that snapshot, damages every changed visible destination, and advances the RenderFrame revision while retaining the PaintSequence and RenderNode structure.
The same texture may appear in several nodes; each visible destination participates independently in damage.

ExternalTexture uses a latest-wins mailbox rather than an unbounded frame queue.
The capture or decoder thread never waits for Runtime, intermediate frames may be dropped, and a renderer acquires the newest frame available when processing damaged content.
The source mailbox and renderer cache together retain at most the currently acquired frame and one newer pending frame.
If no newer frame is ready during an unrelated redraw, the renderer retains the last successfully acquired frame.
Before the first frame, Image contributes transparent visual content without treating the valid texture as an error.
After `Finish()`, rendering freezes on the last acquired or pending frame until the final consumer value and renderer cache release the source state.

Committed visibility controls scheduling rather than production ownership.
When no committed visible command references the texture, publication updates the mailbox but does not continuously wake the UI.
The source may receive an activity callback when its committed visibility changes so a camera or decoder can throttle, but Runtime does not own or pause the producer automatically.
Becoming visible through an ordinary application frame schedules the newest published revision without requiring a new Publish call.

Frame acquisition and synchronization remain platform-specific because a safe common return type cannot represent `CVPixelBuffer`, `IOSurface`, `AHardwareBuffer`, `SurfaceTexture`, DXGI resources, DMA-BUF, `VideoFrame`, and future native handles.
The shared command retains the opaque consumer value and immutable drawing data, while the source state supplies a platform-private mailbox interface only to the matching renderer.
Each backend chooses a native zero-copy path when its renderer and producer share a compatible graphics API and otherwise uses a bounded platform-owned conversion path.
The API promises no copy through shared Runtime; it does not claim universal zero-copy on the current CoreGraphics, Android Canvas, or Cairo backends.
The Apple implementations accept `CVPixelBufferRef` and use Core Image conversion compatible with their existing renderers.
The Android API 23 path accepts a retained `Bitmap`, keeps one acquired frame per active source, and draws it through the existing Canvas backend.
Software and hardware-backed Bitmaps share that source contract, but direct `AHardwareBuffer` import, synchronization fences, and a zero-copy graphics path remain future renderer work.
The Linux implementation copies borrowed straight-alpha RGBA8888 or BGRA8888 rows into native premultiplied ARGB32 storage, keeps one acquired Cairo surface per active source, and leaves DMA-BUF import and explicit synchronization as future renderer work.

The source, payloads, and retained PaintCommands share the opaque source-state lifetime without a registration record.
Unmount first removes committed drawing references and visibility callbacks, then renderer-cache eviction releases its acquired frame; native mailbox resources are released when the source state loses its final owner.
Runtime destruction releases RenderScene and platform-content frames before Root Services are destroyed in reverse registration order.

ExternalTexture is visual content, not a native interaction or accessibility subtree.
Image semantics apply unless the module supplies a more specific HuxerUI semantic declaration, and controls layered over a Camera preview remain ordinary HuxerUI nodes.

Implementation proceeds through reviewable stages:

- The shared protocol has added `ExternalTexture`, the closed PlatformPayload kind, public-header coverage, and focused value and payload tests without adding a registry.
- Shared rendering has added the Image input, DrawExternalTextureCommand, Runtime dependency snapshots, coalesced frame scheduling, damage invalidation, and explicit renderer command handling.
- Android, iOS, Linux, and macOS supply independent platform sources, latest-frame mailboxes, renderer-owned caches, and platform module examples.
- Windows and Web retain explicit unsupported diagnostics until their native frame and renderer paths are implemented; each later backend preserves the same public contract.

Every stage ends with focused tests, the affected current-host build, `git diff --check`, and owner review before the next stage begins.

## Animation model

Animation is separated into motion parameters and animated modifier values. Visibility transitions are deferred.

### AnimationSpec

`AnimationSpec` describes how a value moves:

```cpp
using AnimationSpec = std::variant<
    SnapSpec,
    TweenSpec,
    SpringSpec>;
```

Keyframes, decay animation, and visibility transitions remain follow-up work.

Examples:

```cpp
TweenSpec{
    .duration = 0.2,
    .easing = Easing::EaseOut,
};
```

```cpp
SpringSpec{
    .stiffness = 320.0F,
    .damping_ratio = 0.82F,
};
```

`AnimationSpec` is a value. It is not a modifier and does not own runtime state.

`TweenSpec` supports linear interpolation, cubic ease-in, and cubic ease-out. Ease-in begins at zero speed, ease-out ends at zero speed, and their endpoint slopes can be matched to a linear segment when a motion needs a sustained cruising phase.

### Animated modifier values

`AnimateTo()` combines a target with an animation description:

```cpp
template <class T>
struct Animated {
  T target;
  AnimationSpec animation;
};
```

Modifiers can accept either immediate or animated values:

```cpp
return Panel().With(
    Offset{
        AnimateTo(
            target_offset,
            SpringSpec{
                .stiffness = 320.0F,
                .damping_ratio = 0.82F,
            }),
    },
    Opacity{
        AnimateTo(
            visible ? 1.0F : 0.0F,
            TweenSpec{
                .duration = 0.2,
            }),
    },
    Scale{
        AnimateTo(
            visible ? 1.0F : 0.92F,
            SpringSpec{}),
    },
    Rotation{
        AnimateTo(
            selected ? 8.0F : 0.0F,
            TweenSpec{.duration = 0.2}),
    });
```

The current value, velocity, start time, and target are stored in the compatible `NodeExtension`. Retargeting starts from the current presentation value. Advancing an animation does not recompose the component. Scale and Rotation default to the View center, use a normalized `TransformOrigin`, and share their transform with descendant drawing, clipping, foreground extensions, and pointer hit testing without changing Measure or Layout.

### Deferred transition model

`TransitionSpec` is a proposed insertion and removal model, not a current public API:

```cpp
TransitionSpec{
    .enter = {
        FadeTransition{
            TweenSpec{.duration = 0.18},
        },
        ScaleTransition{
            .from = 0.96F,
        },
    },
    .exit = {
        FadeTransition{
            TweenSpec{.duration = 0.14},
        },
    },
};
```

When a node with an exit transition disappears from the incoming tree, it enters a retained exit state:

```text
remove from the logical composition
    ↓
stop normal input delivery
    ↓
retain mounted presentation state
    ↓
run the exit transition
    ↓
unmount after completion
```

Dialog, BottomSheet, Menu, and Toast apply this lifecycle to Layer entries through a shared internal transition state when their active style enables motion. They reuse `AnimationSpec`, `AnimatedValue`, frame scheduling, reduced-motion resolution, and retained presentation properties rather than introducing a second animation engine. General View insertion and removal transitions remain proposed.

### Reduced motion

Accessibility and platform preferences enter through Environment. Theme motion resolution can replace animations with `SnapSpec` or shorter motion without changing each component.

## Interaction and indication

Pointer input follows one shared pipeline:

```text
PointerEvent
    ↓
hit testing and gesture arbitration
    ↓
clickable or gesture modifier
    ↓
InteractionState
    ↓
IndicationSpec
    ↓
mounted animation and paint
```

Interaction state is tracked per pointer ID:

```text
Press
Release
Cancel
```

A Press records the pointer ID and local press position. Release and Cancel refer to the corresponding Press. This supports multiple simultaneous pointers and multiple active ripple instances.

`OnClick()` and `.On<ViewEvents::Click>()` register the same typed event. Adding a Click handler makes the View participate in click interaction. Flat themes use a state-overlay indication, while Material themes select a ripple with a hover state layer. Default controls resolve their colors from `InteractionScheme` and their transition durations from `MotionScheme`; a typed component style can provide an explicit `IndicationSpec` when its foreground differs from the theme-wide state-layer color. Reduced-motion themes snap those transitions.

`Enabled` is a semantic modifier. Effective enabled state is resolved from the root toward its descendants, so a child cannot re-enable itself beneath a disabled parent. Disabled controls remain hit-test barriers without receiving pointer, scroll, focus, or Click interaction. A control that directly establishes the disabled boundary uses its component-specific disabled state colors. A non-control boundary applies disabled group opacity once; inherited descendants keep their enabled paint colors so the subtree is not dimmed again.

`Focusable` lets a custom View participate in the window focus order. Button is focusable by default. Runtime owns one focused mounted-node identity, dispatches `FocusChanged`, `KeyDown`, and `KeyUp`, and moves focus for Tab or Shift+Tab. Enter activates a focused Button on key down; Space shows pressed indication and activates on key up. Meaningful keyboard input, including an unmapped key reported as `Key::Unknown`, makes focus visible; the explicit Shift, Control, Alt, and Meta keys do not reveal a pointer-focused ring by themselves. Focus ring color, width, disabled opacity, and key indication timing resolve from Theme.

The topmost modal Layer is the active focus traversal root. Opening a nested modal captures the current focus, and dismissing it restores the previously focused mounted node when that node still exists and remains enabled.

When a pointer drag crosses the scroll threshold, the selected scroll container wins gesture arbitration. The original click target receives PointerCancel, Click is suppressed, and its indication runs the cancellation animation.

### IndicationSpec

Indications describe interaction visuals:

```cpp
using IndicationSpec = std::variant<
    NoIndication,
    StateOverlayIndication,
    RippleIndication>;
```

The default indication comes from Theme. A View can override it:

```cpp
return Button("Save").With(
    Indication{
        RippleIndication{
            .color = Color::White(),
        },
    });
```

A ripple is one mounted instance per Press. It continues expanding and fading after Release or Cancel until its configured transition finishes. Its PaintSequence clip uses the resolved component corner radius.

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
};

const GreetingLocale& locale = UseEnvironment<GreetingLocale>();
return ProvideEnvironment(GreetingLocale{"fr"}, Content);
```

Use a semantic wrapper when two ambient values share the same underlying representation. Primitive or third-party representation types are not separate Environment keys by themselves.

Each Environment stores only local values and points to its parent:

```cpp
class Environment {
  std::shared_ptr<const Environment> parent_;
  std::unordered_map<std::type_index, std::any> local_values_;
};
```

Each composed subtree captures its current Environment. A nested provider shadows only the value type it supplies and inherits every other value through the shared parent chain.

Environment carries:

- Theme values.
- Platform and accessibility values.
- The runtime-managed viewport width class.
- Per-window services.
- Other typed third-party values.

Theme and services reuse Environment rather than introducing parallel tree propagation systems.

The public `UseViewportClass()` read resolves an internal Environment value with Compact, Medium, and Expanded states. `AppOptions::viewport_breakpoints` owns the two increasing width boundaries. `Runtime::SetWindowMetrics()` updates that value and invalidates the application root and layers only when the resolved viewport class changes. Exact viewport and safe-area dimensions do not become raw Environment values: measurement receives them through `Constraints` and the layout-time safe-area context, while repeated changes inside one class remain incremental layout work rather than composition dependencies.

## Theme

Theme is a direct, deferred subtree provider built on Environment:

```cpp
template <class Factory, class... Arguments>
View Theme(
    ThemeDefinition definition,
    Factory&& content,
    Arguments&&... arguments);
```

The content factory is stored and invoked only after the Theme Environment is active. This allows `UseTheme()` inside child component composition.

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
ScrollBarStyle
```

Third-party components can define their own style keys without extending a single global style registry.

Material, flat, liquid, and third-party themes are theme provider functions, not Runtime types and not subclasses:

```cpp
template <class Factory, class... Arguments>
View MaterialTheme(Factory&& content, Arguments&&... arguments)
{
  return Theme(
      MaterialThemeDefinition(),
      std::forward<Factory>(content),
      std::forward<Arguments>(arguments)...);
}
```

```cpp
template <class Factory>
View XxxTheme(Factory&& content)
{
  return Theme(
      BuildXxxTheme(),
      std::forward<Factory>(content));
}
```

The built-in Flat and Material systems provide complete light and dark boundaries:

```cpp
FlatTheme(Content)
FlatDarkTheme(Content)
MaterialTheme(Content)
MaterialDarkTheme(Content)
```

`FlatLightThemeSpec()` and `FlatDarkThemeSpec()` return mutable token values that applications can use as the starting point for a branded flat theme. `MaterialLightThemeSpec()` and `MaterialDarkThemeSpec()` provide the corresponding Material tokens. Flat and Material Theme definitions explicitly register their complete Dialog, BottomSheet, Menu, and Toast styles rather than relying on presentation services to infer a style from ThemeSpec. Passing customized tokens to `FlatTheme(theme, factory)` or `MaterialTheme(theme, factory)` rebuilds that system's component styles from those tokens.

### Theme syntax

Pass a component function directly in the common case:

```cpp
return MaterialTheme(AppContent);
```

A component function can receive typed arguments directly:

```cpp
return MaterialTheme(AppContent, user_id);
```

Scope, Environment, Theme, Navigation, Layer, Dialog, BottomSheet, and Popup factories share this binding contract.
Bound arguments are decayed and retained by value because a content factory may execute repeatedly during recomposition.
A lambda remains appropriate when argument derivation or capture behavior is more complex than direct function invocation.

`HUXERUI_THEME` is optional syntax sugar for an inline View expression or a component call with arguments:

```cpp
#define HUXERUI_THEME(ThemeProvider, ...)                              \
  (ThemeProvider)([=]() -> ::huxerui::View { return (__VA_ARGS__); })
```

Root usage:

```cpp
View App()
{
  return MaterialTheme(AppContent);
}
```

Nested usage:

```cpp
return HUXERUI_THEME(
    MaterialTheme,
    Column {
        Header(),
        HUXERUI_THEME(
            LiquidTheme,
            LiquidPanel()),
        Footer(),
    });
```

### Theme resolution

Theme resolution follows a fixed order:

```text
explicit View modifier
    ↓
nearest component style
    ↓
nearest Theme override
    ↓
nearest complete Theme
    ↓
root Theme
    ↓
platform defaults
```

A complete Theme establishes a design system boundary. A Theme override inherits unspecified values from its parent. Runtime does not branch on Material, flat, liquid, or third-party theme identity.

`ThemeDefinition{ThemeSpec}` establishes a complete boundary. `ThemeDefinition{}` only contributes its typed component values, so a nested style override does not replace the parent `ThemeSpec`. Text, Button, Dialog, Toast, ScrollBar, and default indications derive their semantic defaults from the nearest complete `ThemeSpec`. Component style lookup stops at that complete boundary, while a component-only `ThemeDefinition` continues to inherit from its parent. Explicit View modifiers run after semantic style resolution and win without a separate runtime style branch.

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

`Presentation` contains Dialog, BottomSheet, Popup, and Menu entries. Entries at the same level follow attachment order, so a Menu opened from a Dialog appears above that Dialog. `Notification` contains transient messages such as Toast. `System` contains ordinary HuxerUI diagnostic UI such as the debug ribbon and performance panel. Runtime-owned `FrameworkOverlay` content, including text-selection handles and the editing toolbar, remains outside the public layer stack and is painted after it.

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

Back routing checks the framework-owned text-selection overlay first and then visits public layers from top to bottom. `LayerCancelPolicy::PassThrough` continues to a lower entry, `Consume` stops without dismissal, and `Dismiss` invokes `on_dismiss_request` or removes the entry when no callback is present. Dialog, BottomSheet, Popup, and Menu map `dismiss_on_cancel = false` to `Consume`, so a visible interactive presentation never lets Back close content behind it or leave the native window. Toast and passive diagnostic content pass through. [Navigation](navigation.md) extends this Runtime-owned chain after layers with application Back handlers, nested page stacks, and a captured predictive Back transaction. Only a completely unhandled request reaches the platform fallback.

Desktop adapters map Escape through key dispatch. Android's full-screen `HuxerUIActivity` owns one lifecycle-bound Back callback, maps API 34 predictive phases to `BackEvent`, and asks Runtime before invoking its native fallback. API 23 through 33 and an embedded `HuxerUIView` retain the Commit-only `handleBack()` entry point. Runtime never pushes Back-handler state into a platform adapter.

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

The anchor modifier records final PresentationBounds without creating a layer. `Show()` attaches the entry and follows those bounds. `ShowAt()` supports context menus and pointer-position popups. Each Popup or Menu handle retains at most one active entry; presenting through it again dismisses the previous entry before attaching the replacement. `PopupContext` dismisses arbitrary popup content directly, while Menu leaf actions dismiss the complete open menu chain automatically. Anchor movement invalidates only the corresponding layer entry placement, settles that layout path before the current frame commit, and damages the old and new bounds; anchor removal dismisses the entry. Placement combines a preferred side, cross-axis alignment, gap, offset, viewport margin, opposite-side fallback, and final clamping without introducing a general cross-tree layout dependency.

Menu is structurally distinct from Popup. Its public input is a recursive sequence of `MenuEntry` values created implicitly from `MenuItem` and `MenuSection`. Menu items directly contain either an action or another entry sequence, while `MenuSection{}` is a non-interactive logical boundary whose visual treatment belongs to the theme. Items retain resource identifiers and image assets as semantic values; the presentation service resolves resources from the captured Environment and composes themed surfaces and interaction. The root menu owns the transparent outside-press barrier. Submenus are content-only anchored layers, so their parent menu remains interactive; Back closes the deepest open level, the default outside-press behavior closes the complete chain, and opening another submenu replaces only that level and its descendants. Arbitrary custom anchored content remains a Popup responsibility.

Dialog and BottomSheet use their own typed handles rather than a shared public Modal mode. They share private barrier, focus, Cancel, dismissal, Environment, and retained Layer transition machinery, while their layout, surface, motion, and options remain component-specific. Dialog resolves placement and motion from `DialogStyle`, while BottomSheet owns an adaptive-width bottom surface that translates from the window edge. When its style exposes a drag handle, a retained handle extension captures the pointer and shares its downward offset with the surface motion extension; cancellation or a short release settles to the edge, while a release beyond the bounded distance threshold follows the layer's `on_dismiss_request` contract and settles if that request leaves the layer visible. The command-oriented `UseDialog()` path remains the primary ergonomic model.

The built-in debug overlay attaches one persistent System entry after root hooks have installed application services and global components. Its dark-red top-right `DEBUG` ribbon toggles an upper-left metrics panel within the entry's own state. Both are composed from ordinary Views against the complete viewport without applying safe-area or title-bar insets; the ribbon is one rotated component clipped by the viewport rather than separately positioned background and label geometry. Toggling or sampling the panel must not reconcile the application root or damage the full viewport. Runtime records painted-frame count, frame-commit time, and damage ratio in a dedicated debug metrics state. PlatformAdapter optionally supplies cumulative process CPU time, a platform-preferred process-memory footprint, and logical processor count so interval utilization can be derived without platform state leaking into LayerController.

The sampling modifier is mounted only with the expanded panel. It wakes once per second and updates the panel's local scope. That update is an ordinary painted frame, keeping the metric tied to actual work without coupling Runtime accounting to the overlay's reconciliation timing. Collapsing the panel removes the modifier and its deadline, so a static application does not animate merely because the debug ribbon is enabled.

LayerController entries without a transition are removed immediately. Dialog, BottomSheet, Menu, and Toast entries with configured motion first become non-interactive, retain their presentation state through the exit animation, and are removed after completion. Modal barriers remain until actual removal, so focus cannot be restored and content behind a visually exiting modal cannot be activated early.

## RootHook

A RootHook installs per-window services or persistent global components before the first application composition:

```cpp
using RootHook = std::function<void(RootContext&)>;
```

The implemented `RootContext` has two capabilities:

```cpp
class RootContext {
public:
  template <class Service>
  void Provide(std::shared_ptr<Service> service);

  LayerController& Layers();
};
```

The proposed platform-module phase adds one narrow module-author capability without exposing Runtime or PlatformAdapter:

```cpp
class RootContext {
public:
  PlatformModules& Modules();
};
```

`Modules()` opens only factories already registered by the current platform integration.
It does not discover compile-time modules, download dependencies, expose native handles, or provide an application-facing string service lookup.

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

HuxerUI installs its built-in Toast, Tooltip, Dialog, BottomSheet, Popup, and Menu services for every Runtime before application root hooks run. Applications use command-oriented services through their typed `UseXxx()` handles, while Tooltip remains an ordinary retained modifier; root hooks remain the extension mechanism for third-party services and global components. When `AppOptions::show_debug_overlay` is enabled, Runtime installs the built-in DebugOverlay after all root hooks so its System entry remains above other global layers. The option defaults to enabled in Debug builds and disabled in Release builds.

RootHook does not provide:

- Direct Runtime access.
- Direct MountedNode insertion.
- Per-frame callbacks.
- Root replacement.
- Dynamic installation and removal.

## Theme-driven presentation policy

Status: implemented for standard Dialog and theme-owned Tooltip, Dialog, BottomSheet, Menu, and Toast presentation policy

The shared LayerStack foundation owns presentation lifetime, ordering, focus, barriers, Cancel routing, outside-press handling, Environment capture, and removal. It must not also define a single visual structure for every Theme.

Presentation is divided into three contracts:

```text
semantic request
    Dialog title, message, and actions
    Menu items, sections, and submenus
    Tooltip message and target bounds
    Toast message
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

`PresentationMotion` is a public Theme value shared by presentation styles, while motion execution remains private to presentation. An absent optional motion disables the transition; otherwise neutral scale and slide values express a fade, and non-neutral values add scale or placement-relative translation without a second motion-kind hierarchy. The implementation interpolates opacity, scale, translation, and transform origin through `AnimationSpec`, retained Layer transition state, and presentation properties. Dialog, Menu, and Toast derive motion from their styles; BottomSheet maps its component-specific motion values into the same private executor.

Menu motion direction and transform origin derive from the requested anchor placement. Making the origin follow a runtime fallback to the opposite side remains follow-up work because the resolved side currently belongs to LayerStack layout rather than the semantic Menu request.

Theme policy does not erase semantic component identity:

- Dialog remains modal content with focus containment and a barrier.
- BottomSheet remains an edge-attached modal surface.
- Menu remains an anchored semantic item hierarchy.
- Popup remains arbitrary anchored content.
- Toast remains a transient notification.

Custom View factories are escape hatches for application-specific content. They still receive themed outer placement, scrim, and motion where appropriate, but they do not implicitly receive the standard component's surface, padding, or internal layout.

### Standard Dialog model

Dialog supports standard title-and-message requests in addition to custom View factories. A standard request has one positive action and may have one negative action. Empty callbacks retain the normal dismissal behavior without adding application work.

The standard model allows Theme to select a native-feeling arrangement without inspecting application content:

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

`StringVariant` is the shared deferred display-string representation for component, validation, semantics, and presentation APIs that accept either direct text or a `StringResource` plus positional arguments. `UseString` is the single composition-time resolution operation for both StringResource and StringVariant; Runtime and components do not introduce parallel resolver APIs.

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

The message-only API remains the common entry point. Future semantic actions or icons extend the Toast request model rather than requiring callers to construct the Theme's internal View layout.

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
[[huxerui::scope]]
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
  return MaterialTheme(AppContent);
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
| Custom event | `Event<Arguments...>`, `On<Key>()`, `UseEvents()`, and `Emit<Key>()` |
| Custom View effect | Modifier value and `NodeExtension` |
| Custom animation | `AnimationSpec` or animated modifier value |
| Custom interaction visual | `IndicationSpec` and `NodeExtension` |
| Custom text input or selection | `TextInputClient`, `TextSelectionClient`, and `NodeExtension` |
| Custom theme | `XxxTheme(factory)` wrapping `Theme()` |
| Per-window service | RootHook and `RootContext::Provide()` |
| Global component | RootHook and `LayerController` |
| Typed presentation library | A service backed by the Runtime LayerStack |
| Native interactive hierarchy | PlatformView factory, PlacePlatformViewCommand, and internal RenderComposition |
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
- Environment values are captured during composition; the current runtime does not maintain per-key Environment dependency subscriptions.
- Layer entries use independent scopes.
- ScrollBar state exists only on Views that install the modifier.
- Pointer interaction state is stored per pointer ID.
- Explicit style values override Theme without mutating Theme.
- A service belongs to one window root.
- External texture frames invalidate visible destinations without recomposition or PaintSequence recording.
- PlatformView updates diff committed identities and property revisions instead of recreating native instances every frame.
- PlatformView composition reuses compatible platform slice representations, avoids per-slice surfaces on single-surface adapters, and does not split a scene that contains no PlatformViews.

Incremental layout and retained rendering are specified separately in [Incremental Layout and Rendering Design](incremental-rendering.md).
The implemented pipeline coordinates mounted geometry, extension painting, Runtime frame output, and platform renderers under that contract.

## Deliberately omitted abstractions

The current design does not introduce:

- `ModifierHost`.
- A context class for every modifier lifecycle phase.
- Runtime branches for ScrollBar, Ripple, Dialog, or concrete animations.
- `OverlayBehavior`.
- Separate Overlay and Presentation runtime trees.
- A Host type for every global component.
- `AppFeature` or `MountedRootFeature`.
- `RootRegistration`.
- A public parallel ServiceRegistry.
- A public Module base class or runtime plugin registry.
- A native-handle variant shared across platforms.
- Per-frame pixel callbacks through Runtime.
- Theme class inheritance.
- Runtime checks for Material, flat, liquid, or third-party themes.
- Process-global Toast or Dialog singletons.
- Dynamic RootHook installation and removal.
- Arbitrary numeric layer z-index.
- Animated Theme interpolation in the initial implementation.

## Implemented adoption sequence

The foundation was introduced through the following sequence:

- Add the generic modifier descriptor and node extension reconciliation.
- Move ScrollBar frame, pointer, and paint state into a node extension.
- Add generic invalidation flags and prune inactive frame subtrees.
- Add typed hierarchical Environment values and direct Theme providers.
- Add the synthetic RuntimeRoot and shared layer stack.
- Add RootHook service installation.
- Build Dialog and Toast on the layer stack.
- Separate application and LayerStack composition, add level ordering, and build BottomSheet, Popup, Menu, and DebugOverlay on the shared controller.
- Add interaction indications and public animation values.
- Migrate common View styling to `With()` modifier values.
