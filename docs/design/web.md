# Web Platform Design

Status: technical preview

This document defines the implemented HuxerUI Web preview and its target contract, including application startup, Emscripten integration, Canvas-session ownership, rendering, text layout, input, resources, accessibility, and delivery boundaries.

## Goals

- Reuse the existing Runtime, View model, layout, input state machines, animation, text editing protocol, RenderScene, and PaintCommand types without a Web-specific Runtime.
- Keep the static `Application` declaration and shared application source unchanged across native and Web targets.
- Produce an ES module and WebAssembly application that can be mounted into a browser-owned Canvas.
- Preserve logical coordinates, retained PaintSequences, Runtime damage, controlled text editing, typed resources, and platform-owned native services.
- Support mouse, touch, pen, wheel, keyboard, browser IME, high-density displays, resizing, and asynchronous image decoding.
- Leave room for future accessibility, DOM-backed PlatformView, worker rendering, and alternative renderers without exposing a second public UI surface.

## Non-goals

The initial backend does not provide DOM rendering for ordinary Views, server-side rendering, hydration, CSS layout, WebGPU, pthreads, OffscreenCanvas workers, browser navigation integration, PWA packaging, or DOM-backed PlatformView.

The initial backend targets a Canvas-owned application surface. Embedding HuxerUI inside a page that must conditionally return wheel, keyboard, or touch gestures to surrounding DOM content is deferred until Runtime exposes an explicit input-consumption result.

The initial backend may run before the platform-neutral semantics tree is available, but it remains a technical preview rather than a complete accessible Web backend until equivalent browser semantics are emitted.

## Current preview

The current implementation includes Emscripten platform selection, automatic `Application` registration, ES module mounting and disposal, Canvas sizing, frame scheduling, asynchronous PlatformModule result and event dispatch, Canvas 2D replay for every current PaintCommand variant, Pointer Events, wheel and keyboard conversion, synchronous Canvas-backed text layout, controlled browser text input and composition events, preloaded resources, and asynchronous ImageBitmap decoding.

Repository examples generate directly runnable HTML, ES module, WebAssembly, and optional resource data artifacts.
The preview has been exercised with stateful pointer interaction, wheel scrolling, secure single-line input, multiline input, packaged localized resources, and asynchronous image repaint in a Chromium-based browser.

The current text layout is intentionally conservative and still requires broader complex-script, bidirectional, grapheme, and browser-consistency validation.
Exact offscreen group-opacity compositing, clipboard operations outside native browser editing events, semantics and accessibility, embedded-page gesture arbitration, production serving and packaging, multi-browser automation, and real mobile-browser IME validation remain deferred.

## Ownership

The Web backend follows the same Runtime and PlatformAdapter boundary as native platforms:

| Layer | Responsibility |
|---|---|
| Shared Runtime | Composition, reconciliation, layout, focus, interaction, scrolling, text editing behavior, animation, PaintSequence recording, RenderScene construction, and damage |
| WebSession | Internal ownership of one WebPlatformAdapter and one Runtime for a mounted Canvas |
| WebPlatformAdapter | Monotonic time, frame requests, UI-thread dispatch, viewport coordination, and browser service capabilities |
| WebRenderer | RenderScene traversal, Canvas state, damage replay, path conversion, image decode entries, and renderer caches |
| WebTextLayout | Browser font resolution, measurements, line records, UTF-16 caret movement, hit testing, and range geometry |
| WebTextInput | Native input-element lifecycle and conversion of browser editing events into TextInputCommandBatch values |
| WebResources | Synchronous reads from resources loaded before Runtime creation and current locale and display scale |
| ES module integration | Canvas lookup, asynchronous startup, resize observation, browser events, DOM objects, and exported mount and disposal operations |

There is one internal `WebSession` containing one `Runtime` and one `WebPlatformAdapter` per mounted Canvas. Multiple sessions may use the same registered `Application` without sharing Runtime state, focus, layout, resources, or input sessions.

Browser objects never enter shared headers, ViewSpec, MountedNode, PaintCommand, or application state. JavaScript identifies sessions with validated numeric IDs rather than retaining raw C++ object pointers as application-visible values.

## Application startup

Web compilation constructs the same static `Application` used by native targets. Its constructor registers the immutable root and `AppOptions`; Web does not create or require a native desktop `main()`.

The generated ES module exposes a synchronous mount operation that accepts a Canvas selector after module and font loading have completed. Broader startup configuration such as a resource base URL, containing element, and page-integration policy remains deferred and does not belong in `AppOptions`.

Startup occurs in this order:

- Instantiate the Emscripten module and complete packaged-resource preload into the virtual filesystem.
- Await `document.fonts.ready` in the JavaScript entry point.
- Resolve the target Canvas and create WebPlatformAdapter and Runtime from the uniquely registered Application.
- Create the JavaScript session record and install browser observers and event listeners.
- Apply the initial CSS-pixel viewport and ResourceConfiguration.
- Request the initial frame.

The returned session ID owns explicit disposal. Disposal cancels timers and animation-frame callbacks, removes DOM listeners and hidden input elements, releases renderer caches, ends active text input, destroys Runtime, and invalidates the session ID.

The initial distribution contains an HTML entry point when requested by SDK tooling, an ES module, a WebAssembly module, and optional preloaded resource data. The framework library remains a static or object dependency of the final WebAssembly application; ordinary Emscripten shared-library and dynamic-module modes are not part of the backend contract.

## CMake and SDK integration

Emscripten is selected through `EMSCRIPTEN` before native platform branches and configures `cmake/platform/Web.cmake`.

The Web platform configuration supplies Web sources, platform-owned application registration, required Emscripten link settings, JavaScript bridge code, and exported lifecycle operations. Web targets do not build the ordinary HuxerUI shared library by default.

Direct consumers continue to create an executable, link the canonical `HuxerUI::huxerui` target, enable scope code generation, and attach resources. Tool resolution continues to use the development host, so a macOS, Windows, or Linux prebuilt code generator runs while the C++ target is WebAssembly.

The CLI wraps the same CMake path for project creation, diagnostics, incremental builds, and local serving through `emrun`.
The project-owned Web shell keeps its HTML and Canvas mount code under `platform/web`, while the backend continues to avoid a parallel JavaScript component build system.
Release packaging remains separate future work.

## Viewport and display scale

Runtime logical coordinates map to CSS pixels. The Canvas CSS size defines the Runtime viewport, while its backing bitmap width and height are the rounded CSS dimensions multiplied by `devicePixelRatio`.

The renderer applies the display scale at the Canvas boundary, so layout, pointer positions, text geometry, PaintCommands, and damage remain in logical coordinates. `ResourceConfiguration::display_scale` uses the same value for image variant resolution.

`ResizeObserver` detects Canvas geometry changes. A resolution media query and window resize listener detect display-scale changes, rebuild the backing bitmap, update Runtime resource configuration, and request full repaint. Setting Canvas backing dimensions resets Canvas state, so the renderer restores its baseline transform and clip state before replay.

## Frame scheduling

`WebPlatformAdapter::Now()` uses the browser monotonic performance clock expressed in seconds.

`RequestFrameAt()` coalesces immediate requests into one `requestAnimationFrame` callback. A future absolute deadline arms one browser timer; when the deadline becomes eligible, the timer requests an animation frame rather than building and presenting outside the browser rendering phase.

The animation-frame callback builds one `FrameCommit`, presents its `RenderFrame`, then schedules `next_frame_deadline`. Re-entry is prevented with the same pending-build and pending-paint invariants used by native adapters.

When a document becomes visible after animation frames were suspended, the Web integration requests a new frame. Runtime time remains monotonic, so retained animations advance to the current time rather than replaying every missed frame.

## Canvas rendering

Canvas 2D is the initial rendering backend because it directly represents the current rectangles, text, images, paths, transforms, clips, opacity, and shadows without adding a graphics dependency or a second public drawing model.

WebRenderer traverses `RenderScene` in C++, maintains balanced Canvas save and restore operations, and translates every PaintCommand explicitly. Node offset, presentation transform, group opacity, child clip, children transform, content, descendants, and foreground preserve the ordering defined by the retained-rendering contract.

The Canvas bitmap retains pixels between frames. For partial damage, WebRenderer rounds each region outward to device pixels, uses those same bounds for clearing, background fill, and one compound device-pixel clip, then traverses the committed scene once in logical coordinates. A full-damage frame clears and replays the complete surface. Runtime remains responsible for conservative transformed bounds and damage expansion.

The initial implementation should use a narrow bridge without defining another serialized render model. A compact replay packet, cached JavaScript sequence, or other batching representation is added only if profiling identifies WebAssembly-to-JavaScript calls as a material bottleneck, and remains private to WebRenderer.

Paths map to browser `Path2D` or direct Canvas path operations. Renderer-owned caches may retain converted paths by stable PaintSequence revision when profiling justifies it.

Decoded images are asynchronous browser values. WebRenderer assigns an internal loading entry to an ImageAsset, copies encoded bytes before an asynchronous decoder can observe movable WebAssembly memory, and creates an ImageBitmap. A pending image is skipped for the current replay. Completion validates the session, stores the decoded value in a 64 MiB renderer-owned LRU cache, and requests another frame through WebPlatformAdapter. Decode failure retries at bounded delays and becomes terminal after three attempts for that session, while the negative cache remains count-bounded.

WebGPU, Skia, or another renderer may later implement the same RenderScene contract. None requires changes to View, Canvas, PaintCommand, Runtime, or application source.

## Text layout and drawing

WebTextLayout implements the existing synchronous TextLayout contract and uses UTF-16 offsets, matching browser string and selection coordinates.

Each WebRenderer owns one Canvas measurement context rather than a DOM element for every HuxerUI Text node. Canvas TextMetrics supplies font metrics, advances, baselines, and painted bounds.

Creating a WebTextLayout materializes an immutable C++ snapshot containing measured size, baselines, line records, grapheme caret boundaries from `Intl.Segmenter`, and measured caret advances. Hit testing, caret lookup, and range geometry query that snapshot without returning to Canvas.

Non-editable paragraph measurement and drawing share a bounded renderer-owned WebTextLayout cache keyed by text, font, layout options, and logical width. Editable layouts returned through `CreateTextLayout()` remain independently owned so active text geometry does not borrow cache lifetime.

Drawing uses the same resolved CSS font description, direction, locale, alignment, wrapping decisions, and line records as measurement. The generated example entry waits for `document.fonts.ready` before mounting; direct integrations must do the same after loading application fonts. Runtime font-generation invalidation is not implemented in the preview.

The preview resolves paragraph direction from the first strong character and handles whole-paragraph left-to-right or right-to-left placement. Mixed-direction caret geometry and full Unicode line breaking still need cross-browser validation. If Canvas metrics cannot meet the required consistency, an alternative text engine may replace WebTextLayout and WebRenderer internals without changing the public Text or TextInput APIs.

## Pointer, wheel, and keyboard input

The Web integration uses Pointer Events as the single mouse, touch, and pen input source. Pointer identifiers, type, position, and click count are converted at the Canvas boundary. Pressure, buttons, timestamp, and modifiers are not represented by the current shared PointerEvent contract.

An accepted pointer down captures that browser pointer to the Canvas so move, up, and cancel remain ordered when contact leaves the element. Browser pointer cancellation maps to the shared Cancel path and must release pressed, drag, focus-candidate, and retained-extension state.

Wheel deltas are normalized from pixel, line, or page units into logical pixels before producing a ScrollEvent. The initial Canvas-owned integration prevents page scrolling for gestures directed at the application surface. An embedded-page mode requires a future Runtime consumption result instead of guessing from concrete components.

The Canvas is focusable and receives key down and key up events when HuxerUI owns browser focus. Browser key values, repeat, modifiers, and platform conventions map to the existing KeyEvent model. An active hidden text control forwards Tab to the same Runtime key path instead of letting DOM focus leave the Canvas-owned application surface. Text insertion is never inferred from printable key events while a browser text-input session is active.

CSS `touch-action` belongs to browser integration policy. A full application surface initially owns direct-manipulation gestures and uses a restrictive value. A future embedded mode may expose an explicit page-gesture policy without adding a C++ View modifier.

## Browser text input and IME

WebTextInput creates one native `input` element and one `textarea` when the first editing session starts, then reuses them across single-line and multiline sessions. These elements are visually hidden from application rendering but remain focusable, editable browser controls.

Input type, capitalization, action, multiline, secure entry, and autocorrection map to the closest browser attributes such as `type`, `inputmode`, `enterkeyhint`, `autocapitalize`, and `autocomplete`. Runtime does not start an editable native session for a read-only TextField.

The active element is positioned near Runtime's current caret geometry so native IME candidate UI and mobile keyboard behavior have a meaningful anchor. Position and selection are refreshed after Runtime state or geometry changes.

`input`, selection, and composition events are converted into ordered TextInputCommandBatch values. Browser mutations are observed, converted, and resynchronized against the shared controlled value without double-applying text.

WebTextInput retains the active text-input session identifier and a restart token. Every asynchronous or deferred event reads the current token before calling Runtime, while disposal and configuration restart invalidate stale events.

Touch scrolling over a TextField does not focus the hidden input merely because a pointer started over editable content. Runtime first resolves the gesture as a tap and starts or requests the native session while still processing the trusted browser pointer event, allowing mobile keyboards to open without a second tap.

## Clipboard

Browser clipboard events provide synchronous data during trusted copy, cut, and paste dispatch, while the asynchronous Clipboard API may require focus, user activation, and permission.

The preview does not expose `PlatformClipboard`, because its synchronous contract cannot truthfully represent the asynchronous browser Clipboard API. Native editing events on the hidden input continue to provide browser-managed copy, cut, and paste while the input is active.

An application-triggered asynchronous clipboard operation cannot be represented as a completed synchronous read. The initial backend must not report unavailable pasted data or a rejected promise as synchronous success. A future clipboard expansion should add one shared asynchronous capability or an explicit Runtime command path only when TextField Cut, Paste, and application clipboard APIs require it.

## Resources and locale

Packaged resources follow [App Resources, Images, and Localization Design](resources.md). The ES module completes asynchronous transport before Runtime creation, then WebResources provides synchronous immutable reads from WebAssembly memory or the virtual filesystem.

Browser URLs are not ResourceIds. Network code fetches bytes asynchronously, constructs an ImageAsset or RawAsset, updates controlled application state, and uses ordinary recomposition.

The initial locale derives from `navigator.language` and is normalized through the existing Locale model. Dynamic browser-language updates are not implemented in the preview; display-scale updates use `Runtime::UpdateResourceConfiguration()`.

## PlatformModule

Web uses the platform-neutral `PlatformModuleFactory`, `PlatformInstance`, typed method, typed event, cancellation, and disposal contracts without adding a JavaScript factory registry or a second message protocol.
Module-owned Web sources are ordinary C++ and Emscripten glue selected by the module's CMake target when `EMSCRIPTEN` is active.
They call browser APIs through Emscripten, register the existing factory from an explicit RootHook, and keep JavaScript values, callbacks, promises, and DOM objects outside `PlatformPayload` and shared application code.
Modules that require JavaScript libraries express those link inputs through their own Emscripten target configuration; the HuxerUI module graph does not parse or reproduce JavaScript package metadata.

The WebPlatformAdapter supplies the shared `UIThreadDispatcher` through the browser event loop.
Native result and event sinks may be invoked during a browser callback, but typed application callbacks always run asynchronously after the initiating stack has unwound.
Closing an instance invalidates its pending calls and event routes before a queued task can observe application state, while cancellation remains owned by the module instance.

The Web `example_platform_module` uses an Emscripten interval to exercise factory creation, typed calls, first-result completion, recurring events, cancellation, and disposal through the same Timer Root Service as the native examples.

## PlatformView composition

DOM-backed PlatformView remains proposed, but its composition contract follows final RenderScene paint order rather than one DOM overlay above the complete Canvas.
`PlacePlatformViewCommand` divides the scene into nonempty HuxerUI Canvas slices and DOM PlatformView placements.
The WebPlatformAdapter will consume the shared internal `RenderComposition` before drawing and retain compatible Canvas elements and DOM objects across frames.

Web PlatformView factories will register explicitly under the same stable UTF-8 type strings as native platforms and keep DOM values in platform-owned factory state rather than `PlatformPayload`.

A PlatformView-capable session owns one isolated CSS stacking context around the browser-supplied Canvas.
The original Canvas serves as the first HuxerUI slice when applicable, while additional transparent Canvas elements and PlatformView elements become absolutely positioned ordered siblings in the same composition root.
DOM child order represents `RenderComposition` order; application z-index values do not participate in or escape that root.
Every Canvas slice shares the logical viewport, backing-store scale, clipping root, and resize transaction, while its renderer replays only the retained scene content assigned to that slice.
A scene without PlatformViews keeps the current single-Canvas structure and does not allocate a wrapper surface per RenderNode.

Composition-root and DOM hierarchy changes occur while applying a committed `RenderComposition`, never during Canvas command replay.
Disposal removes adapter-owned slices and PlatformViews, releases native listeners and accessibility bridges, and leaves no hidden interactive DOM objects behind.
Moving or resizing a PlatformView updates its CSS geometry and old and new damage, while unchanged slices retain their Canvas and rendering caches.

The composition root observes pointer events during capture and asks Runtime for the topmost committed HuxerUI or PlatformView hit target.
When HuxerUI wins, the adapter prevents native activation and routes the pointer sequence to Runtime.
When a PlatformView wins, its DOM subtree receives the ordinary browser event and owns browser pointer capture, focus, selection, and editing until the sequence ends.
Transparent Canvas slices never become full-viewport input blockers merely because they are visually above a PlatformView.

The first Web contract supports axis-aligned CSS bounds, translation, rectangular overflow clipping, visibility, and sibling ordering.
CSS transforms, filters, blend modes, or stacking contexts created inside a PlatformView cannot alter ordering outside its assigned placement.
An element that requires top-layer presentation, a separate browsing context with incompatible policy, or another mechanism that cannot remain in the composition root is rejected rather than moved above the complete HuxerUI scene.

## Accessibility and semantics

Canvas pixels alone do not provide the browser accessibility tree required by interactive applications. Stable Web support therefore depends on mapping the implemented platform-neutral semantics foundation into the browser.

Runtime publishes the immutable owning `SemanticFrame` defined by [Semantics and Accessibility Design](semantics.md) independently of rendering; collection, live-region, modal, and complete visibility behavior remain part of its staged implementation.
The planned WebPlatformAdapter mapping retains that frame and maps only meaningful semantic nodes to minimal browser elements associated with the Canvas.
It does not reconstruct semantics from PaintCommands and does not mirror every View or layout node into DOM.

Semantic DOM is not a second visual renderer.
It remains visually unobtrusive, participates in browser focus and assistive technology, forwards typed semantic actions to Runtime, maintains live regions and collection metadata, and follows committed order, visibility, transforms, and clipping where geometry is exposed.
Browser accessibility focus remains separate from Runtime input focus, and the semantic DOM coordinates focus with the hidden input and textarea so an active TextField does not create duplicate keyboard focus targets.

DOM-backed PlatformView is a separate future leaf-node capability.
Its visual DOM element occupies the `RenderComposition` position, while its semantic anchor occupies the corresponding SemanticFrame position and exposes the native accessible subtree without duplicating it in semantic DOM.
It must not use the semantics overlay as a general visual DOM container.

## Threading

The initial Web backend is single-threaded on the browser main thread. Runtime, Canvas, DOM input, text measurement, resource access, and frame scheduling therefore preserve ordinary synchronous ownership.

Emscripten pthreads, SharedArrayBuffer deployment headers, worker proxies, and OffscreenCanvas introduce synchronization and hosting requirements without evidence that the retained renderer needs them. They are considered only after browser profiling demonstrates a material main-thread problem and after text input and semantics ownership across threads is defined.

## Browser lifecycle and failures

The integration observes document visibility, Canvas connection at scheduling time, browser focus, geometry, display scale, and disposal. A disconnected Canvas does not destroy application state automatically, but it stops presenting. Replacing a Canvas requires disposing the old session and mounting a new one.

Exceptions do not cross the JavaScript and C++ boundary. Exported operations catch C++ failures and preserve HuxerUI diagnostics. Mount failures are rejected, fatal session-dispatch failures dispose the failed session, and text-input dispatch failures remain contained at the input boundary. Invalid or stale session IDs are ignored without dereferencing destroyed state.

`QueryProcessMetrics()` initially returns no value because browser process CPU and memory figures do not have the same reliable per-application semantics as native backends.

## Validation

Web platform work requires:

- A clean Emscripten configure and build in addition to an incremental build.
- Public header and shared Runtime tests on available native hosts.
- Web compile coverage for every PaintCommand variant and application entry registration.
- Browser tests for initial mount, disposal, resize, display-scale changes, frame coalescing, damage replay, pointer capture, Cancel, hover, wheel, keyboard, focus, and visibility restoration.
- Text tests for ASCII, surrogate pairs, grapheme clusters, multiline wrapping, bidirectional text, selection, composition, secure input, submission actions, and session mismatch.
- Resource tests for preload completion, locale, density variants, missing payloads, asynchronous image readiness, cache lifetime, and repaint.
- Focused rendering checks in Chromium, Firefox, and WebKit, with screenshot tests used as smoke coverage rather than the sole semantic assertion.
- Manual mobile-browser validation for real keyboards and IMEs that automation cannot reproduce faithfully.
- Accessibility validation before the backend status advances beyond technical preview.

Unavailable browsers, operating systems, mobile IMEs, and accessibility tools are reported explicitly rather than treated as passing.

## Delivery sequence

The first milestone added Emscripten platform selection, platform-owned application registration, ES module mounting, Canvas sizing, frame scheduling, Canvas 2D replay, and generated example entry points.

The second preview milestone added Pointer Events, wheel, keyboard, WebTextLayout, browser text input and composition events, resources, and asynchronous images.

The next milestone hardens disposal, failures, locale and display changes, browser integration tests, release-size settings, and SDK or CLI serving and packaging.

The backend remains a technical preview until the semantics tree and accessible browser mapping are available. Embedded-page gesture arbitration, DOM-backed PlatformView, PWA packaging, worker rendering, and alternative graphics backends remain independent later work.

## Invariants

- Web adds a PlatformAdapter and renderer, not another Runtime or component implementation.
- Ordinary Views render through RenderScene and Canvas rather than DOM.
- DOM use is limited to browser services, text measurement, text input, semantics, and future PlatformView.
- Future PlatformViews and Canvas slices share one isolated composition root and follow RenderScene paint order.
- Runtime logical coordinates remain CSS-pixel coordinates; display scale is applied at the platform boundary.
- Resources are ready and synchronously readable before Runtime starts.
- Browser text input emits shared TextInputCommandBatch values and never owns authoritative TextField state.
- Asynchronous image completion requests a frame without introducing image state into Runtime.
- Renderer batching and caching remain private optimizations driven by profiling.
- Stable Web support requires platform-neutral accessibility semantics.
