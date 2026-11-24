# Architecture Design

HuxerUI separates the application lifetime, each mounted window lifetime, and the platform host. Shared code owns declarations, composition, layout, interaction, and scene construction. Platform-derived Runtime and UiWindow classes implement the host operations needed to drive that shared model.

## Runtime and UiWindow ownership

Runtime owns application services, the platform factory catalog, application dispatch, and ordinary timers. Each attached UiWindow owns a root Environment, window services, the mounted application tree, the layer stack, and its frame transaction. One application thread runs shared UI work. Each window schedules and commits its own frames.

| Owner | Responsibility |
| --- | --- |
| Runtime | Application installation, service and factory lifetime, dispatcher, and shutdown |
| UiWindow | Window installation, composition, mounted identity, layout, focus, input routing, frame commit, and retirement |
| PointerInteraction | Physical pointer ownership, gesture arbitration, hover, and typed drag and drop |
| TextInteraction | IME session identity, editing commands, selection interaction, and the editing overlay |
| FileDropReceiver | External hover and asynchronously prepared file deliveries |
| SemanticTree | Committed semantic identities, hierarchy, and action dispatch |
| SceneTransitionService | Retained visual transition state and frozen scenes |

The window collaborators are private parts of UiWindow coordination. `UiWindow::State` supplies the shared mounted-tree context while each collaborator retains its own transient state. Operations that change focus, request a frame, or invalidate content go through UiWindow. Callbacks preserve stable identities and re-resolve mounted references after calling application code.

## Application and window installation

The static `Application` declaration stores a root factory and immutable `AppOptions`. Runtime invokes `application_hooks` once, in declaration order, after its platform facilities and built-in services are ready. Hooks share the installing service table and factory catalog. Successful installation freezes both before queued application work or window initialization. Installation can run without any attached window.

An installation failure retires partial state and propagates the original failure. A failed window installation unwinds that attachment. A failed application installation prevents later hooks and window initialization.

Each UiWindow installs a fresh root Environment and built-in window services, then runs its `window_hooks` before first composition. The application and window contexts are borrowed during their respective hooks. Attaching another window repeats window installation and uses the existing Runtime and frozen catalog.

Application services belong to Runtime. Window services and layers belong to their original UiWindow. Duplicate service types and factory names are rejected. Retained handles preserve the provenance of their originating attachment. See [Application Activation and Lifecycle](application.md#application-hooks-and-applicationhandle) and [Window Presentation](window-presentation.md).

## Top-level window ownership and dispatch

The platform application runner or host owns the event loop. Desktop `OpenWindow()` asks Runtime to create and initialize a top-level window before presenting it. Native facilities are ready before shared initialization admits callbacks. `OpenWindowAsync()` requests an iOS scene or a HarmonyOS child window in the existing application Runtime. An externally attached window remains owned by its host.

Accepting closure of an owned desktop window stops new callbacks. Runtime defers teardown until active UI calls, including native callback tails and nested loops, have returned. Window retirement retires mounted work, layers, and services before the host releases its facilities. An iOS scene may temporarily lose its attachment while its window session remains open. [Multiple windows](../guide/core-concepts.md#multiple-windows) describes the public handle behavior.

After removing the last owned window, Runtime follows `quit_on_last_window_closed`. Explicit Quit uses the same deferred retirement boundary and bypasses window close interception. Attached windows and their work retire before application tasks and services.

## Composition and mounted behavior

`View` and its copy-on-write declaration contain transient input. Reconciliation maps compatible declarations onto mounted nodes and retained extensions. A RecomposeScope owns its State and Lifecycle identity. Keyed movement preserves that identity where reconciliation is compatible. The mounted tree and its owners supply layout, hit testing, paint, and semantics.

`With()` applies ordered modifiers, `On<EventKey>()` binds typed events, `LayoutValue<Key>()` supplies parent-child metadata, and `Key()` supplies sibling identity. Property modifiers change one declaration in order. Retained modifiers reconcile per-node extensions. Each extension uses the controlled `ViewNode` interface for its owning node. Third-party layouts, gestures, input clients, and visuals use the same mechanisms as built-in components.

Environment values are typed and inherited. A Theme is an Environment boundary, and each scope observes the exact values it reads. Window metrics feed layout constraints, while observable viewport classes invalidate only subscribers when a breakpoint is crossed.

The [composition design](view-composition.md) describes scope reconciliation, local property resolution, and Environment tracking. [Task ownership](tasks.md#scope-ownership) describes asynchronous work and cancellation.

## Frame lifecycle

A frame updates dirty composition, reconciles mounted state, measures and places affected subtrees, resolves presentation geometry, and records the changed paint sequences. Hit testing, carets, layers, and semantic bounds use the resolved window geometry. Layout and paint reuse clean retained results.

A `FrameCommit` publishes one `RenderFrame` and `SemanticFrame`. Renderers consume their committed data at the platform boundary. A platform-presented frame can isolate a composition failure in the built-in error page through `BuildFrameOrErrorPage()`. `BuildFrame()` preserves the original exception for callers that own the error policy.

See [Incremental Layout and Rendering](incremental-rendering.md) for the frame transaction, invalidation, geometry, and damage rules. See [Semantics](semantics.md) for committed accessibility and action ownership.

## Platform content integration

Application hooks register typed factories. PlatformModule instances retain library-owned nonvisual lifetimes. A PlatformView places an interactive host object in scene order. ExternalTexture supplies live visual content through the renderer. The [platform content design](platform-content.md) defines the catalog, bridge, scene placement, and frame lifetime contracts.

## Source boundaries

Public declarations live under `include/huxerui`. Shared implementations live in `src/runtime`, `src/application`, `src/components`, `src/text`, `src/graphics`, `src/resources`, and `src/io`. Concrete platform implementations live under `platform`. Shared runtime contracts carry platform-neutral data.

Private dependencies run from UiWindow coordination to mounted-node operations to View declaration support. `ui_window_internal.h` may include `mounted_node_internal.h`, which may include `view_internal.h`. Shared private access to public types uses `detail::InternalAccess` for operations that need friendship. It owns no state or dispatch.

## Subsystem contracts

- [Layout and virtualization](layout.md) and [scrolling](scrolling.md) describe measurement, virtual item lifetime, and input dispatch.
- [Gestures](gestures.md) and [interaction indication](interaction-indication.md) describe input arbitration and retained visual feedback.
- [Text layout](text.md) and [text input](text-input.md) describe glyph geometry, editable clients, and IME integration.
- [Window presentation](window-presentation.md) and [navigation](navigation.md) describe layer lifetime, focus, Back routing, and page transitions.
- [Animation](animation.md) describes timing, motion, and frozen rendering.
- [SDK and CLI](sdk-cli.md) describes builds, package ownership, and library integration.
