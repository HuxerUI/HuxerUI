# Platform Content Integration

Platform integration uses three independent lifetime models.

| Content | Integration |
| --- | --- |
| Nonvisual platform operation or session | A PlatformModule instance opened by the library |
| Interactive host View | A PlatformView declaration and a committed platform object |
| Live visual stream | An ExternalTexture composed by the renderer |

## PlatformRegistry contract

Each Runtime owns one private catalog. Application hooks register both factory kinds in one case-sensitive UTF-8 name space. The catalog validates the kind and exact C++ types, rejects duplicate names across kinds, and freezes after the last application hook. See [application and window installation](architecture.md#application-and-window-installation).

A module registration binds its handle and optional Options type. A View registration binds its Properties and optional Controller type. A factory may be a direct callable, retained factory object, or library adapter. A library's installation hook supplies the factory, and its typed public API hides the name and host details from applications.

See [Extending HuxerUI](../guide/extending.md#platformmodule) for registration and usage examples.

## Strongly typed C++ path

Direct C++ factories receive concrete values. A Module factory takes one host reference, `Runtime&` or `UiWindow&`, followed by `const Options&` if registered. A View factory prepares for the mounting UiWindow and receives its defined platform object types. `PlatformValue` preserves C++ type identity and optional equality through in-process type erasure for retained storage.

A PlatformView describes complete controlled Properties. Properties are movable and equality-comparable so compatible reconciliation can skip unchanged updates. The direct factory receives `const Properties&`. The optional Controller and typed EventBindings travel separately from Properties. The propertyless declaration uses the registered void form.

## PlatformModule ownership

`OpenPlatformModule()` opens a new instance in the original Runtime or live UiWindow context. Call it outside composition in application or window hooks, in committed `Lifecycle` setup, or in application-thread work. A retired window cannot be substituted with another one.

The returned value is the library's concrete handle. An application or window service can retain it, or a component can release it through its Lifecycle cleanup. The handle owns the native instance or its language bridge according to the library's RAII and cancellation contract.

## PlatformView controllers

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

The registration binds the exact Controller type. Equal values identify the same command target across recomposition. The internal typed binding connects the Controller only to the committed instance, with the factory defining attach and detach operations. Properties remain a separate declaration.

A Controller may be declared before its PlatformView mounts and becomes connected only after the candidate platform instance commits.
One Controller may be connected to at most one committed PlatformView at a time; a second simultaneous attachment is a framework usage error.
Compatible reconciliation preserves an unchanged binding, replacing a Controller detaches the old binding and attaches the new one without updating Properties or recreating the platform object, and unmount disconnects the Controller before invalidating calls and disposing the platform instance.
Disconnected calls are not queued or replayed; the library maps them to its own synchronous, callback, Future, Task, or error convention.
Initial URL, options, and other initial facts remain Properties rather than imperative commands issued before attachment.

A direct C++ factory may attach its platform instance to the Controller without an intermediate proxy.
A cross-language implementation may compose a reusable framework call channel that owns payload conversion, result delivery, thread transfer, cancellation, disposal invalidation, and late-result rejection.
Using that channel does not require the Controller to inherit a proxy base, and a library may instead implement its complete JNI, Objective-C++, Emscripten, or other bridge directly.
Both forms attach through the internal typed Controller binding and therefore share the same mount, replacement, and teardown semantics.

## Typed events

Platform events remain ordinary HuxerUI Event Keys and use `Event<Result(Arguments...)>`.
An event with several fields carries one owning, non-reference structured `T` rather than a multi-argument signature, giving every platform boundary one value to validate and decode.
Every `.On<Key>(handler)` call creates the typed EventBinding and, when the Key provides platform-boundary metadata, records its stable event name and concrete argument type in the same binding.
This behavior belongs to generic event binding construction rather than a PlatformView-specific UiWindow branch, so fluent calls before or after ordinary View modifiers cannot lose the descriptor.
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
Synchronous emission before commit, after detachment, from a disabled mounted View, or from another thread produces no result and invokes no handler.
Disposal invalidates the emitter before releasing platform state, so later emissions are harmless no-ops.

Method results and events remain distinct.
A per-invocation `PlatformResult` completes or fails one cross-language call at most once, while an instance-level `PlatformEventEmitter` may publish any number of unsolicited notifications or synchronous PlatformView decisions.
Neither endpoint dictates the library's public callback or return type.
A PlatformChannel accepts only void event subscriptions; request results remain on `Invoke` rather than becoming result-returning Module events.
A direct C++ Module may expose ordinary callbacks or state without either endpoint, while a cross-language implementation may adapt Result and notification delivery into the library's chosen API.

## PlatformPayload boundary

`PlatformPayload` is the value model used by HuxerUI's common bridge when data crosses between C++ and another platform language.
It does not appear in ordinary PlatformView construction, direct C++ factory signatures, direct C++ method handlers, typed service APIs, or Windows and Linux parameter flow.
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

`PlatformPayload` remains a structurally immutable equality-comparable tree containing null, boolean, signed 64-bit integer, double, UTF-8 string, `Bytes`, list, string-keyed object, and the closed framework capabilities `ExternalTexture`, `FileReference`, and `BufferReference`.
It is an in-process boundary value rather than a persistence, network, or general serialization format.
Its value tree never accepts callbacks, arbitrary C++ objects, platform Views, or executable closures, and its binary form never embeds system handles or pointers.
Media bytes are not implicitly serialized: an ExternalTexture payload retains the same shared platform-owned texture object used by rendering, while a BufferReference capability retains a CPU-readable memory range without copying it.
A FileReference payload retains the same access state and captured metadata used by shared file operations so a platform library can consume an Android `Uri`, Apple `NSURL`, or browser `File` without reducing the original grant to a local path or detached platform value.

### Platform-language value API

The Android Java SDK, Web JavaScript bridge, and Apple Objective-C/Swift adapters expose one structurally immutable `PlatformPayload` value type rather than separate Reader, Writer, Builder, or Codec abstractions.
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
fileReference(value)
```

An adapter with an implemented ExternalTexture capability bridge additionally provides `externalTexture(value)`.
Android and Apple also provide `bufferReference(value)` and the corresponding exact accessor; Web explicitly rejects BufferReference.

Bytes and collection constructors copy or safely freeze their inputs; BufferReference retains external storage without freezing its contents.
Byte reads return a defensive copy or an immutable platform view and never expose mutable backing storage.
Java maps Int64 to `long` and Bytes to copied `byte[]`.
The Apple adapters map them to Swift `Int64` and `Data`, while Web maps them to JavaScript `bigint` and copied `Uint8Array`.
The Web contract never constructs Int64 from Number and currently rejects every ExternalTexture payload; every adapter rejects non-finite Double values, invalid Unicode, and unsupported or closed capability values.

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
requireFileReference()

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
The SDK exposes no public JSON conversion, numeric coercion, or raw capability slot.

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
The one-byte tags are Null `0`, Boolean `1`, Integer `2`, Double `3`, String `4`, Bytes `5`, List `6`, Object `7`, ExternalTexture `8`, FileReference `9`, and BufferReference `10`.
Integer values use signed 64-bit little-endian representation, Double values use their IEEE 754 binary64 bits in little-endian order, and all byte lengths and container counts use unsigned 32-bit little-endian values.
Strings contain a byte length followed by UTF-8 bytes, lists contain a count followed by values, and objects contain a count followed by length-prefixed UTF-8 keys and values.
Object keys are serialized in ascending UTF-8 byte order so one payload has one canonical encoding.
ExternalTexture contains capability kind `1` as one byte followed by an unsigned 32-bit envelope-local slot.
FileReference uses the same slot representation with capability kind `2`; BufferReference uses capability kind `3`.

The binary format preserves all payload kinds without implicit coercion.
Decoders require the declared tag and range instead of converting strings to numbers, truncating doubles to integers, or treating bytes as text.
Strings and object keys must be valid UTF-8, doubles must be finite, and containers enforce unique object keys plus nesting and allocation limits.
The maximum nesting depth is 64, matching the in-process payload contract.
All bridge implementations use the same framework constants for maximum envelope bytes, scalar bytes, and container entries, plus capability slots where supported, and validate them before allocation; a platform must not substitute looser local limits.
Unknown versions, flags, tags, or capability kinds, duplicate object keys, invalid UTF-8, non-finite doubles, integer or length overflow, truncated input, excessive allocation, and trailing bytes are malformed payloads.

`ExternalTexture`, `FileReference`, and `BufferReference` are opaque capabilities and therefore travel beside the binary data in separate bridge-private, strongly typed companion tables.
The binary stream contains only an envelope-local typed slot, while JNI references, Objective-C objects, JavaScript handles, or retained C++ objects remain in the owning bridge for that crossing.
Slots are unique only within one envelope and are not public identifiers.
Repeated references to the same slot preserve the same capability within that decode.
A bridge without transport for a capability rejects the value before encoding, without a byte-copy fallback.
A bridge that supports a capability rejects a missing slot, duplicate capability-table entry, unreferenced table entry, or kind mismatch while decoding.
On successful decode, the resulting shared reference or language wrapper retains the capability before the temporary table is released; on failure, the bridge releases the complete table.
Library code cannot forge, retain, or reuse a slot, and the closed table cannot carry arbitrary platform objects.

FileReference table equality and deduplication use the retained C++ access state, not its display metadata, projected path, URI text, or wrapper-object address.
Android and Web wrappers carry a process-local capability key derived from that state address solely for equality and table deduplication; the separate native handle owns a `FileReference` copy, while the key is never dereferenced or persisted and remains distinct from a provider entry key.
The companion entry carries the complete C++ value, including `CanWrite()` and other captured metadata, without serializing those fields or exposing a second authorization model to platform code.
Android exposes one `HuxerUIFileReference` class whose bridge-created instances retain the C++ capability and return the underlying document `Uri`; its separate state-owned instances continue to perform provider I/O and never point back to their owning C++ state.
The Apple adapters retain the C++ value in a lightweight `HUXFileReference` wrapper and expose its path-backed `NSURL`; ARC releases the capability with the wrapper.
Web exposes a retained `FileReference` wrapper whose asynchronous `getFile()` resolves the current browser handle or captured `File`, and whose `close()` releases its C++ share.
These projections do not start a new security scope, duplicate `CanWrite()`, attenuate the original grant, or make the capability persistent.

`HuxerUIFileReference.fromUri()` constructs a read-only payload-compatible capability from existing URI access without acquiring or releasing persistent grants.
On iOS, `+[HUXFileReference copyFromFileURL:error:]` creates a read-only SDK-owned copy before returning from a provider callback; the existing shared file state retains its temporary-file owner through reference I/O.

BufferReference lives in `data.h` and owns one shared backing state containing a fixed byte span and a retained owner; slices add only an offset and length.
Construction rejects nonempty storage without an owner, and slicing validates bounds using subtraction to avoid overflow.
Equality and table deduplication use backing-state identity plus range, not contents or the raw address alone; independently wrapping the same address creates a distinct capability.
Default and moved-from references are valid empty values, distinct from payload Null; even an empty slice may retain an owner.
Referenced memory is outside the inline scalar-byte limit, while the total capability-slot limit still applies; Android additionally requires lengths representable by ByteBuffer.

Retaining the owner guarantees neither immutable contents nor synchronization.
The producer must keep memory mapped and address-stable, finish its writes before reading begins, and wait for every reader to complete before reuse.
The framework adds no buffer pool, lock, lease, frame scheduler, or implicit copy; dimensions, row strides, pixel formats, and sequence numbers belong to a library's frame type.
Reusing a reference does not change payload equality or automatically invalidate UI state.
Only CPU-readable memory belongs here; GPU resources continue to use ExternalTexture.

Android retains global references to the direct ByteBuffer slice and optional final-release Runnable, never to the wrapper that owns the native handle.
The wrapper captures position-to-limit at construction, and payload construction and extraction retain independent native shares.
A scoped read keeps a temporary native share through its callback, even when the wrapper closes reentrantly; the read-only ByteBuffer must not escape.
The last backing-state release invokes the optional Runnable on the releasing thread before dropping the global references; the callback must handle thread affinity and must not throw.
Java finalization is a fallback, not a prompt-release guarantee for payload-owned shares.
Apple holds the C++ reference in an ARC wrapper and retains the supplied storage owner, whose release can likewise occur on the final reader's thread.
Pointers from temporary Swift Data borrows and direct views into externally closed images are not made safe by retaining a wrapper.
Local-notification persistence rejects nested buffer capabilities rather than snapshotting them; Web has no supported shared-memory bridge.

| Platform boundary | Binary data | Capability table |
| --- | --- | --- |
| Android | `byte[]` | Separate `HuxerUIExternalTexture`, `HuxerUIFileReference`, and `HuxerUIBufferReference` lists |
| Apple Objective-C/Swift adapters | Internal owned bytes | Separate `HUXExternalTexture`, `HUXFileReference`, and `HUXBufferReference` arrays |
| Web common adapter | `Uint8Array` | `FileReference` list; ExternalTexture and BufferReference remain unsupported |

Application callback objects never enter the envelope, and Objective-C, Java, JavaScript, or C++ exceptions are contained by the bridge that owns them.
`PlatformError` has a stable UTF-8 `code`, an English `message`, and optional structured `details` carried by the same payload envelope.
Framework codes reserve the `huxerui/` prefix and library codes use a library-owned prefix.
A bridge failure that prevents a library result from being represented becomes a stable framework error rather than escaping through another language runtime.

## Host factories and call lifetime

Platform factories receive the concrete owner appropriate to the registration:

| Host | PlatformView owner | Window-owned PlatformModule owner |
| --- | --- | --- |
| Windows | Parent `HWND` | Owning `HWND` |
| macOS | Owning `NSWindow` | Owning `NSWindow` |
| Linux | Adapter-owned parent `GtkWidget` | Owning `GtkWindow` |
| Web | The factory returns a detached `HTMLElement` | Browser host |
| Android | Owning `Context` | Originating `HuxerUIView` for a window factory |
| iOS | Owning `UIViewController` | Owning `UIViewController` |
| HarmonyOS | Originating ArkUI `UIContext` | Originating ArkUI `UIContext` and `Window` |

The Java class adapter resolves stable names through the host ClassLoader and retains the View factory for the mounting UiWindow. Application Module factories resolve lazily and receive the application Context. Keep rules protect classes referenced by runtime name. The Apple adapters accept actual Objective-C or Swift factories and maintain independent UIKit and AppKit lifetimes. Their `HuxerUIPlatform` Clang modules expose the corresponding platform types. Web receives actual JavaScript factory objects before mount. HarmonyOS resolves ArkTS factories through the original ability, passes application modules its `UIAbilityContext`, and retains each returned `ComponentContent` until its node is detached. Direct C++ factories remain available alongside the common adapters.

View creation, update, placement, and disposal run on the owning UI thread. The platform View root is stable and initially unparented. The adapter owns attachment, and updates apply complete Properties values idempotently. Creation-time notifications supported by a language bridge are withheld until the instance commits. A candidate cannot answer a synchronous mounted event, and a failed candidate delivers no notifications.

The common call bridge owns request identity, thread transfer, results, and cancellation. Module results and notifications may come from another thread and are delivered through the owning dispatcher. Even a result completed inline is delivered after invocation returns. Disposal rejects new work, invalidates the emitter and completion endpoints, cancels pending work where possible, and drops late results. See [Platform Support](../guide/platforms.md) for the platform APIs.

## PlatformView

PlatformView is a real built-in leaf View rather than a modifier.
UiWindow owns its mounted identity, compatible reconciliation, measurement, final geometry, visibility, hit-testing boundary, focus participation, semantic anchor, and unmount timing.
The native backend owns the corresponding `NSView`, `UIView`, Android `View`, `HWND`, DOM element, or equivalent platform object.

The [PlatformView declaration](../../include/huxerui/view.h) carries its registration name and complete controlled Properties. `PlatformView("WebView")` selects the no-properties form. Ordinary `.On<Key>()` bindings attach typed handlers and any bridge event metadata. Use `Focusable(false)` to omit the view from shared focus traversal.

A library component constructs `PlatformView(name, properties)` with its registration name and controlled Properties. Reconciliation preserves the mounted instance when the declaration is compatible. A changed name, incompatible Properties type, or incompatible key replaces the instance. A changed value updates the retained instance at commit. Imperative calls use the [separate Controller binding](#platformview-controllers).

PlatformView measurement remains platform-neutral and never creates or synchronously measures a platform object during shared layout.
PlatformView has zero intrinsic logical size under loose constraints; ordinary parent constraints and size modifiers such as `Frame` produce its final axis-aligned layout bounds.
A concrete library may require dimensions or derive a Frame from controlled application data.
Platform intrinsic-content changes do not mutate mounted geometry behind UiWindow or start a native-to-layout feedback loop.

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
Before raster presentation, a shared internal builder traverses the committed RenderScene for the native backend and derives an immutable `RenderComposition`.
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
The [rendering boundary](incremental-rendering.md#platformview-composition-foundation) specifies damage handling and the Windows aperture retirement timing.

The adapter creates a PlatformView instance when its identity first enters a committed RenderComposition and applies controlled properties before making it visible.
A property revision sends the complete controlled Properties to the compatible instance through an idempotent Update, while bounds-only changes update placement without resending unchanged Properties.
Factories should validate an Update before mutating observable platform state and apply the complete controlled value idempotently.
Replacement prepares the new instance before retiring the old one.
A factory exception is a library integration error that aborts the platform commit; adapters contain platform exceptions at their boundary but do not attempt to roll back arbitrary library-owned platform state.
UiWindow retirement detaches input, focus, and accessibility bridges, destroys PlatformViews and native composition resources, and then releases its window services. Application services remain owned by Runtime until application retirement.

PlatformView presentation supports translation, axis-aligned layout, rectangular clipping, visibility, and deterministic ordering among PlatformViews.
Arbitrary rotation, path clipping, group opacity spanning a PlatformView, backdrop filters, and offscreen effects are unsupported until every platform can preserve their semantics.
The framework rejects unsupported declarations instead of approximating them silently.
Exact z-order does not imply support for an otherwise unsupported visual effect.

Platform pointer, keyboard, IME, and internal gesture handling remain inside the PlatformView hierarchy when the PlatformView is the active hit target.
UiWindow hit testing and the committed `RenderComposition` use the same front-to-back order, so visually higher HuxerUI content wins before a covered PlatformView receives platform input.
Platform slice hosts remain hit-test transparent outside HuxerUI interactive regions instead of blocking the complete PlatformView rectangle below them.
Once the PlatformView wins hit testing, its platform hierarchy owns pointer sequences and gestures until completion or cancellation; the host does not duplicate those events into UiWindow.
Focus traversal treats the PlatformView as one HuxerUI leaf, and platform focus changes synchronize that leaf without exposing platform responder objects.
A focused platform text editor owns its platform text service. See [PlatformView focus](text-input.md#platformview-focus) for session retirement and recreation.

The semantic tree contains one PlatformView anchor at the mounted position.
On hosts that bridge the semantic tree, the platform accessibility adapter attaches the platform object's accessibility root beneath that anchor, preserves its position among HuxerUI semantic siblings, and excludes duplicate HuxerUI descendants.
Removing or replacing the PlatformView invalidates the bridge before destroying the platform object so retained accessibility references fail safely.

Platform adapters preserve the same contract through platform-specific composition machinery:

| Platform | Composition strategy |
| --- | --- |
| Windows | One transparent DirectComposition surface replays every HuxerUI slice, while child HWNDs remain beneath it. Each placement clears a rectangular aperture in command order, and later HuxerUI drawing may cover that aperture without allocating a surface per slice. |
| macOS | Transparent HuxerUI slice views or layers and NSViews are retained as ordered siblings under one host NSView. AppKit hierarchy changes occur outside `drawRect:`. |
| Linux | Retained GtkWidget children are clipped and inserted between Cairo and GSK drawing in one GtkSnapshot. A transparent input widget uses UiWindow hit testing to admit GTK child picks only where platform content owns input. |
| Web | HuxerUI Canvas slices and DOM PlatformViews are ordered siblings in one isolated CSS stacking context. The Web host coordinates DOM event targeting with UiWindow hit testing. |
| Android | The host is a ViewGroup that alternates HuxerUI slice replay with ordinary child drawing in committed order. A `TextureView` participates as a regular child, while any `SurfaceView` subtree is rejected because its system composition cannot preserve this Canvas order. |
| iOS | Transparent HuxerUI slice views or layers and UIViews are retained as ordered siblings under one host UIView. CoreGraphics replay targets only damaged slices. |
| HarmonyOS | The XComponent draws the base slice. Clipped ArkUI children and custom drawing nodes retain the order of the remaining placements and slices. ArkUI navigates inside each hosted focus scope and returns Tab navigation to UiWindow when the scope loses focus. |

These strategies are conformance requirements, not application-selectable composition modes.
A factory whose platform object cannot preserve exact ordering on the current platform fails with a diagnostic identifying the PlatformView registration name or unavailable adapter capability at the layer that detects it.
It must not silently flatten the declaration into a global foreground or background plane, capture an interactive platform hierarchy as stale pixels, or discard covering HuxerUI content.
Libraries may choose a different platform implementation internally, while high-frequency visual content without platform interaction remains an ExternalTexture.

Current shared tests cover payload invariants, per-surface registration, leaf layout, identity and property revisions, typed event delivery, frontmost hit testing, basic paint order, adjacent PlatformViews, keyed movement, replacement, and unsupported transforms and opacity.
The macOS integration fixture covers PlatformView creation, property update, HuxerUI slice ordering, retained identity, unchanged placement, focus synchronization, accessibility identity resolution, removal, stale-event rejection, and disposal.
Android focused tests cover semantic-anchor encoding, while the Android PlatformView example and device validation cover PlatformView creation, controlled updates, slice ordering, hit testing, focus and IME transfer, platform accessibility attachment, removal, and recreation.
The Windows integration fixture covers PlatformView creation and update, retained identity across hiding, nested HWND focus and UI Automation resolution, stale-event rejection, presentation-delayed retirement, remount, and deterministic disposal.
The Linux integration fixture covers GTK picking, controlled properties and events, clipping during scrolling, retained identity, and disposal. GTK can expose its own widget accessibility when enabled. The shared semantic anchor is not projected into GTK.
Each available platform adds an integration fixture with HuxerUI content below and above one PlatformView, verifies frontmost pointer ownership, platform focus and IME transfer, accessibility traversal through the anchor where bridged, retained platform state across recomposition and temporary hiding, and deterministic teardown.
Android integration rejects `SurfaceView` where system composition cannot preserve the shared order.
Web integration validation covers DOM stacking, platform events, focus boundaries, and disposal across supported browsers.

### Windows PlatformView composition

The default Windows 10 adapter uses one premultiplied-alpha DirectComposition surface as the complete HuxerUI composition plane whenever the committed scene contains PlatformViews.
It does not create one HWND, swap chain, bitmap, or DirectComposition surface per HuxerUI slice.
The renderer enables this surface on the first PlatformView and retains it for the window lifetime.

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

The Windows convenience factory is declared in [windows/platform_registry.h](../../include/huxerui/windows/platform_registry.h).

`Instance` is an arbitrary library type and does not inherit a HuxerUI base class.
For a controlled registration, the factory adapter connects `Instance` to the exact registered Controller type; for an uncontrolled registration it needs no controller-related API.
The factory's `view` operation must return a same-process, same-UI-thread `WS_CHILD` HWND whose parent is the supplied clipping container.
After successful creation, HuxerUI owns destruction of that root HWND.
The adapter calls `dispose` once before `DestroyWindow`; `dispose` releases library callbacks, subclass state, and other owned resources but does not destroy the HWND, then the retained `Instance` is released.
Creation, update, disposal, placement, focus changes, and `PlatformEventEmitter` delivery run on the owning UI thread.
The emitter dispatches through the active mounted event route synchronously and returns no result from an inactive route.

A single framework-private transparent input-shield HWND covers the client area while PlatformViews are present.
The shield excludes rectangular regions where a committed PlatformView is the frontmost input target and the frontmost hosted HWND. This lets Windows hit-test its hierarchy, including descendants hosted on another thread.
Potential HuxerUI hit bounds, higher hosted HWNDs, and custom chrome constrain these openings. Extension hit shapes use conservative bounds.
At points still covered by the shield, UiWindow resolves the exact hit target. The shield returns `HTTRANSPARENT` for a winning PlatformView on the normal same-thread path, and routes input to HuxerUI when it wins.
Pointer capture remains with the side that won the initial sequence.

UiWindow-directed PlatformView focus selects the root HWND or its first focusable descendant.
After Win32 message dispatch, the adapter maps `GetFocus()` back through each hosted root and its descendants and synchronizes the matching mounted identity.
Tab traversal remains within standard child HWNDs while a next tab stop exists, then crosses the PlatformView boundary through UiWindow focus order.
A focused HWND editor owns Win32 text services and suspends the HuxerUI text-input session until focus returns to a HuxerUI editor.
The UI Automation adapter exposes the hosted HWND provider below the existing PlatformView semantic anchor rather than publishing both as sibling roots.

Windows PlatformViews are opaque rectangular HWND islands.
A factory that returns a foreign-process HWND, a different-thread HWND, a non-child root, or platform content requiring per-pixel composition outside its aperture fails explicitly.
Rotation, path clipping, cross-boundary group opacity, backdrop effects, and other unsupported declarations remain rejected by the shared composition builder before platform mutation.
The Windows 7 compatibility renderer does not silently flatten PlatformViews into a global foreground or background plane; a Windows 7 session that requests this DirectComposition capability fails with an explicit unavailable-capability diagnostic.

## ExternalTexture

[ExternalTexture](../../include/huxerui/external_texture.h) identifies one live visual producer and retains its immutable logical size, frame revision, and visibility subscriptions. Each platform subclass owns its mailbox and publication API. Applications and renderers share the same object through `std::shared_ptr<ExternalTexture>`.

The concrete producers and their native call requirements are listed in [Platform Support](../guide/platforms.md#reading-externaltexture-frames). Retained Bitmap, pixel-buffer, and advanced GDK inputs stay immutable while a renderer can access them. A publisher can reuse copied or cloned input when publication returns.

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
The concrete texture accepts frame publication from its supported producer context, atomically advances the shared revision, replaces its latest immutable frame snapshot, and requests at most one platform frame from each committed UiWindow that currently displays it.
UiWindow records visible texture uses while publishing the RenderScene and retains a committed snapshot of each identity, revision, and transformed destination.
On the next BuildFrame it compares revisions with that snapshot, damages every changed visible destination, and advances the RenderFrame revision while retaining the PaintSequence and RenderNode structure.
The same texture may appear in several nodes; each visible destination participates independently in damage.

ExternalTexture uses a latest-wins mailbox rather than an unbounded frame queue.
The capture or decoder thread never waits for UiWindow, intermediate frames may be dropped, and a renderer acquires the newest frame available when processing damaged content.
The mailbox retains one immutable latest frame and each active renderer may retain the snapshot it imported.
Acquisition does not remove the mailbox value, so several renderers or windows can consume the same publication independently.
If no newer frame is ready during an unrelated redraw, the renderer reuses its last imported resource.
Before the first frame, Image contributes transparent visual content without treating the valid texture as an error.
After `Finish()`, rendering freezes on the last published frame until the final shared texture and renderer caches release it.

`SnapshotAsync()` acquires this same immutable frame at the call site and retains it until conversion to a `Bitmap`. The conversion and any GPU readback run only when the task executes. An empty mailbox yields `std::nullopt`, while a failed or unsupported readback raises an error through the task.

Committed visibility controls scheduling rather than production ownership.
When no committed visible command references the texture, publication updates the mailbox but does not continuously wake the UI.
`IsActive()` reports whether at least one live UiWindow currently displays the texture, but UiWindow does not own or pause the producer automatically.
Becoming visible through an ordinary application frame schedules the newest published revision without requiring a new Publish call.
Visibility is a private weak subscription from each UiWindow, not a single application binding.
The same texture may be displayed by several UiWindows; unmounting one removes only that UiWindow's subscription.

Frame acquisition and synchronization remain platform-specific because a safe common return type cannot represent `CVPixelBuffer`, `IOSurface`, `AHardwareBuffer`, `SurfaceTexture`, DXGI resources, DMA-BUF, `VideoFrame`, and future native handles.
The shared command retains the texture identity and immutable drawing data, while the renderer casts to its matching platform texture and takes the latest frame at draw time.
Concrete textures with a public `AcquireFrame()` expose the same completed frame representation used by their renderer.
The mailbox hands out independently retained frames, or independent `VideoFrame` clones on Web, so replacing or finishing the source does not invalidate a frame already acquired.
For thread-safe mailboxes, publication and acquisition exchange ownership under the frame lock and GPU work runs outside that lock.
A consumer retains the frame or its imported resource through GPU use and writes processing results to its own output.
Each backend chooses a direct import or a GPU copy into compatible resources and reports unsupported imports.
The API promises no copy through shared UiWindow code; it does not claim universal zero-copy on CoreGraphics, Android Canvas, or a GSK renderer that must convert an incompatible GDK texture.
The Apple implementations accept `CVPixelBufferRef` and use Core Image conversion compatible with their existing renderers.
The Android API 23 path keeps Canvas as the primary renderer and draws retained `Bitmap` frames directly.
`GlTexture::PublishCurrent()` reads level-zero `GL_TEXTURE_2D` content from the current EGL context, duplicates an optional native acquire fence, and synchronously converts it once into a compositor-owned premultiplied 2D texture without CPU readback.
Without an acquire fence the call waits for producer GL work; after `PublishCurrent()` returns, the source texture storage is no longer retained and may be reused or deleted.
`SurfaceStreamTexture` creates a producer-facing `Surface`, latches its SurfaceTexture on the private EGL thread, applies the SurfaceTexture transform, and converts each ready OES image once into the same immutable internal representation.
Revision advances only after that import completes, so UiWindow never observes a revision whose frame is not drawable.
Both Android GPU mailboxes retain the same `GlFrame` type. Its `ImportCurrent()` creates one `EGLImage` lazily on the compositor worker and uses it to create independent 2D texture objects in compatible caller contexts on the same display.
The consumer owns the imported texture name and its sampling parameters, and retains the shared pixels as read-only input.
Each GPU draw occurrence owns an internal non-interactive TextureView output while it remains in the committed scene.
The Android renderer encounters that ordinary `DrawExternalTextureCommand` during Canvas replay, renders its selected source rectangle into the TextureView surface, and invokes `drawChild()` under the current Canvas transform, clip, and save-layer stack.
This platform-private child does not become a PlatformView, public View, accessibility node, or shared RenderComposition layer.
Frames containing GPU texture children move the base Canvas slice from `onDraw()` into `dispatchDraw()` so all Canvas, GPU texture, and PlatformView content retains command order.
Direct `AHardwareBuffer` import remains future work.
HarmonyOS `GlTexture::PublishCurrent()` blits from the producer's current OpenGL ES context into a new `OH_NativeBuffer` and waits there for GPU completion before publishing. `GlTexture` and `NativeBufferTexture` publish the same `GlFrame` for Native Drawing replay and external acquisition. Producers can render directly into a buffer and publish it through `NativeBufferTexture`; published buffers remain immutable even after a new publication or `Finish()`. `GlFrame::ImportCurrent()` creates an EGLImage from the retained buffer on the consumer's display and binds it to a read-only texture in the current context. Texture coordinate zero is the logical top edge, and the frame remains retained through GPU use.
The Linux `PixelTexture` implementation copies borrowed straight-alpha RGBA8888 or BGRA8888 rows into premultiplied storage and wraps each acquired frame in a `GdkMemoryTexture` for GTK Snapshot composition.
`linux::GlTexture::PublishCurrent()` validates level-zero `GL_RGBA8` storage in the current producer `GdkGLContext`, then completes a GPU copy into a new immutable texture managed through a private HuxerUI context in the same GL share group.
HuxerUI constructs the `GdkGLTexture` with its completion fence and retains the same GL resource owner in GDK and the acquired `GdkFrame`, so either can keep the copied texture alive.
A `GlTexture` consumer can borrow the frame's texture name only in a compatible current `GdkGLContext` and must retain the frame through GPU use.
The synchronous copy commits before revision publication, restores the producer context before returning, and allows the producer to update or delete its mutable source immediately.
The Linux `GdkTexture` implementation remains the advanced path that retains one already immutable GDK frame in its latest-wins mailbox, so producers may publish `GdkDmabufTexture` or another GDK-owned texture without HuxerUI downloading pixels.
The custom GTK widget snapshots ordinary PaintCommands as ordered Cairo nodes and ExternalTexture commands as ordered GDK texture nodes under the same transform, clip, path-fill, opacity, content, child, and foreground structure.
GTK 4.14 is the minimum because path clips use the 4.14 Snapshot fill API; the GDK GL texture builder used for GPU frames is available from GTK 4.12.
GDK owns final GL and DMA-BUF presentation import and format negotiation; the concrete Linux ExternalTexture owns its appropriate synchronization and native resource lifetime.
HuxerUI does not expose file descriptors, GL names, EGL objects, Vulkan objects, or display-server handles in UiWindow or PaintCommand.
The Windows `PixelTexture` implementation copies the same borrowed formats into premultiplied BGRA storage, updates a retained Direct2D bitmap once per physical frame, and preserves the last CPU frame across D3D device recreation.
It reuses the bitmap allocation while pixel dimensions remain unchanged.
`D3D11Texture::Publish()` accepts a completed one-mip, one-slice, single-sampled BGRA8 default-usage texture, copies it once into a new HuxerUI-owned NT-handle shared resource, waits for that copy, and publishes only the immutable snapshot.
The producer may reuse or release its source after publication returns, but it must submit writes first and externally serialize access to its immediate context.
The renderer opens the snapshot on its own same-adapter D3D11 device and creates a Direct2D bitmap from that DXGI surface without a renderer-local texture copy or CPU readback.
Every renderer precollects the snapshots used by a frame, acquires their keyed mutexes in stable resource order, submits all Direct2D reads with `EndDraw()`, and releases the mutexes in one cleanup path.
A zero-timeout contention result abandons that render attempt and schedules a retry instead of blocking the UI thread; abandoned synchronization and adapter mismatch fail explicitly.
`D3D11Frame::TryCopyTo()` uses the same open and synchronization protocol to submit one copy into a consumer-owned texture, so processing can proceed on that destination after releasing the shared frame's mutex.
Renderer-local device resource recreation discards only the opened resource, mutex, and Direct2D wrapper, then reopens the retained snapshot while its producer device remains valid.
A removed producer device invalidates snapshots owned by that device, so the application must publish a replacement from a valid device before another render.
The modern NT-handle path is unavailable in Windows 7 compatibility builds.
The Web implementation accepts only open WebCodecs `VideoFrame` values, clones each publication synchronously, and leaves the caller responsible for closing its original value.
Because `emscripten::val` is thread-affine, texture construction, publication, finish, and destruction remain on the browser main thread.
The Canvas renderer clones the latest mailbox frame at most once per physical frame, shares that renderer-owned clone across every Canvas slice, maps logical crop coordinates through `displayWidth` and `displayHeight`, and closes replaced or inactive clones.
WebCodecs may share the clone's underlying media resource, but Canvas drawing and browser color conversion may still copy, so this backend does not claim zero-copy.

GPU-native expansion remains backend-owned rather than copying Android's TextureView mechanism into shared UiWindow code.
Each platform preserves its primary renderer and introduces native composition only when that renderer cannot import the producer inline.

Payloads and retained PaintCommands share the same opaque texture lifetime without a registration record.
Unmount first removes committed drawing references and visibility subscriptions.
Renderer caches are weakly keyed and release imported frames when the owning renderer prunes the entry or is destroyed; platform mailbox resources are released when the texture loses its final owner.
UiWindow retirement releases RenderScene and platform-content frames before its window services; Runtime releases application services when the application retires.

ExternalTexture is visual content, not a PlatformView interaction or accessibility subtree.
Image semantics apply unless the library supplies a more specific HuxerUI semantic declaration, and controls layered over a Camera preview remain ordinary HuxerUI nodes.
