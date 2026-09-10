# UI Testing Internals

## Ownership

The optional `testing/` implementation links one ordinary core and creates the existing Runtime with a `TestingPlatformAdapter`.
There is no Runtime subclass, production per-frame query cache, test-ID property, or second authoritative mounted tree.
The explicit `<huxerui/testing/ui_test.h>` include is an intentional exception to the production umbrella rule.
Public usage and limitations are documented in [Windowless UI Testing](../guide/testing.md).

`UiTestSession` owns the adapter, queue, Runtime, virtual clock, and completed-frame observations.
Queries hold a weak session and immutable selector chains; each operation resolves again without retaining mounted pointers.
InternalAccess supplies the full mounted root, unlike the application-only `RootNode` accessor used by existing low-level tests.
Traversal includes layers and records parent indexes, own text, typed keys, local geometry, transformed and conservatively clipped bounds, and interaction flags.
Secure built-in editor clients are filtered before querying text.
No new component switch is introduced into Runtime input dispatch.
The session builds one ID-to-node index over its owning immutable SemanticFrame and shares it with queries and structural serialization.
Internal semantic matches borrow nodes; only public observations copy their data.

The queue accepts posts from other threads but only the creating thread executes callbacks and Runtime operations.
One Pump moves the entry callback batch, advances the shared virtual clock, runs the batch, and calls BuildFrame once.
An over-budget batch fails rather than draining indefinitely.
Normal destruction and initialization failure use the same shutdown path: tear down Runtime while the adapter remains alive, execute queued cancellation/disposal callbacks on the owning thread, and close the shared queue.
The cleanup drain does not build frames or wait for workers and uses `maximum_callbacks_per_frame` as its total callback budget, including callbacks posted by other cleanup callbacks.
Cleanup exceptions are contained so destruction cannot throw and an initialization failure retains its original exception.
Once the queue is empty or the budget is exhausted, remaining callback captures are released outside its mutex and late posts are rejected.
Callback delivery after closure is ignored.
Reentrant test operations fail, and PumpUntil predicates are observational.

Ordinary input keeps the real Runtime's capture, clipping, gesture arbitration, focus, modal routing, and controlled text reducer.
High-level pointer sequences use fixed identity and best-effort Cancel cleanup.
Reference text is deliberately a small, internally consistent scalar model rather than a shaping engine.
Unsupported services retain normal unavailable behavior; resources, text-input state/action tracking, clipboard, and a virtual clock are supplied by the test adapter.
Start, Update, Restart, and Stop maintain the existing TextInputState value with session checks; no separate testing input-session abstraction is introduced.
Frame requests retain their earliest finite deadline but never schedule work autonomously; Pump alone commits frames.
Pump consumes the previous wakeup before callbacks and BuildFrame, then retains deadlines reasserted by tasks, callbacks, or FrameCommit.
PumpAndSettle combines that deadline with the queued callback count under bounded virtual time and total frame count, including its initial frame.
Continuous requests advance by the minimum step; future requests can jump to their deadline, while queued callback batches need no time advance.
This intentionally cannot distinguish a repeating animation, caret, or delayed task from other pending Runtime work and does not wait for external workers.
ScrollUntil uses mounted identities only to detect container replacement; it sends ordinary wheel input through ScrollBy and never materializes virtual children itself.
Window and quit requests retain the base adapter's no-op behavior.

## Capture

Each committed test frame is captured into an owning canonical named-field tree while RenderScene pointers remain valid.
Public captures share this immutable value, so raw input between frames cannot expose a partially updated tree or dangling render data.
Semantic and render traversal are distinct and ordered.
Every PaintCommand alternative has an explicit serializer; an unhandled new alternative fails compilation.
Schema 2 uses named fields, fixed six-decimal classic-locale numbers, normalized negative zero, escaped control characters, and stable command names.
It excludes process identities, pointers, revisions, source paths, and external texture pixels.
Encoded image bytes are retained in hexadecimal rather than identified by an address or process-local hash.
Structural equality is not native-rendering equality.
Equality and Diff traverse the canonical tree directly, independently of the text export.
ToString serializes that tree on demand and returns an owning string; no duplicate text export is retained or generated during Pump.
Diff reports field changes and entire inserted or removed subtrees, using bounded sibling lookahead and unique semantic identifiers to align nearby changes without allocating a quadratic edit matrix.
Anonymous content falls back to order; the result does not infer stable identity across arbitrary reorders.
Report limits bound displayed changes and value lengths, not captured content or equality; omitted differences are marked explicitly.
Image bytes remain part of equality but produce only a content-change summary in diagnostics.
No parser, file I/O, baseline policy, public diff model, or production cache is added.

## Distribution

`HUXERUI_BUILD_TESTING_LIBRARY` controls only the library; production core targets never link it.
Desktop SDKs export `HuxerUI::testing` with an ordinary shared/static core dependency selected by the requested available component.
The imported testing target binds to that core on first lookup; later incompatible requests are rejected rather than changing existing consumers.
Windows testing archives use the same `_debug` naming policy as core libraries so both configurations can coexist.
Android uses a separate `libhuxerui_testing.so` alongside `libhuxerui.so` for each ABI.
The published Android AAR remains Java-only; CMake shared-library dependencies supply native APK contents.
The production Gradle module never builds testing or selects its runner.
The independent `:ui_testing` module owns only the windowless test APK; SDK packaging does not build or consume this module.
The Android SDK build compiles core and testing together in one source CMake tree per ABI, with smoke tests disabled; Gradle builds only the Java-only production AAR.
iOS uses a separate `HuxerUITesting.xcframework` with matching device and simulator slices.
Web uses `libhuxerui_testing.a` with the same Emscripten options and version as the core archive.
Installed package lookup never substitutes a desktop archive for a mobile/Web artifact.
Missing optional artifacts make the required testing component unavailable.
SDK packaging scripts build these artifacts; neither test runners nor assertion libraries are shipped as production dependencies.

## Portable smoke carriers

`HUXERUI_BUILD_TESTING_SMOKE_TESTS=ON` adds the current platform's thin carrier in `tests/platform/testing` and requires `HUXERUI_BUILD_TESTING_LIBRARY=ON`.
The shared smoke body creates an ordinary application, loads a generated raw resource and built-in check icon, observes a live layer, taps a Button, checks State-driven text, and captures render output.
Carriers transport diagnostics and deploy the same generated package; they do not implement queries or Runtime behavior.

| Platform | Carrier | Invocation |
| --- | --- | --- |
| macOS, Windows, Linux | C++ executable | `huxerui_ui_testing_smoke [package-directory]` |
| Android | JNI library and independent instrumentation module | `:ui_testing:connectedDebugAndroidTest`; resources are deployed automatically |
| iOS | XCTest bundle | Xcode XCTest execution, or the matching Simulator platform's `xctest` agent with `simctl spawn` |
| Web | Emscripten module plus browser page | Serve the build's `bin` directory over localhost HTTP and load `index.html`; visible result and `data-status` report completion |

Desktop default package location is compiled into the repository carrier; an explicit argument permits relocated resource deployment.
The Linux executable still links distribution GTK/GIO dependencies: no window is created, but display-free execution must be validated on a Linux host.
iOS copies the package into the XCTest bundle, and Web preloads it into `/package`.
Web tests run in a browser, not an assumed Node-compatible environment, and yield before executing the smoke body.
The browser page bounds module initialization; an external runner must still bound a synchronously stuck WASM test.

The Android `:ui_testing` module builds source libraries by default, or imports `HuxerUI::testing` from the explicit installed prefix supplied through `-PhuxeruiTestingSdk=<sdk-directory>`.
It is not a dependency of production applications and needs no Java platform services for its windowless smoke.
Gradle stages the generated package into test assets after native builds; the runner extracts those assets into a fresh private cache directory and reuses the common file-backed resource provider.
The runner removes its temporary package after execution, runs native tests on the Android UI thread, and reports individual test status and failures to Instrumentation.
The production module's existing Java-only suite remains `:HuxerUI:connectedDebugAndroidTest` and is not switched by testing options.
Use a runner wall-clock timeout and never treat an APK build as a device test result.

Compile, execute, resource-fixture, shared-suite, and native integration results are separate claims.
An unavailable toolchain/device is unavailable validation; host success is not evidence of another platform's execution.
These carriers validate the testing adapter, not AppKit/UIKit, Java event conversion, GTK/Pango, Win32, or the browser's native rendering/text integration.

## Regression coverage

`tests/runtime/ui_testing.cpp` exercises public query independence from Semantics, typed/scoped keys, owning observation lifetime, real input, disabled/cancel paths, active pointer collisions, secure text, controlled UTF-16 editing, virtual Delay, layer traversal, generated resources, and thread affinity.
Focused channel fixtures cover normal and failed-initialization cleanup, cancellation/disposal delivery, cleanup exceptions, bounded follow-up work, and rejected late delivery without retained callback captures.
`tests/cmake/testing_consumer` is a standalone installed-header consumer; configure separate builds with `HUXERUI_TEST_LINK_FORM=static` and `shared` where available, using a relocated SDK prefix.
The installed-consumer CTest runs default and explicit link forms, preloaded core targets, repeated lookup, and conflicting-request rejection whenever the SDK includes testing.
UI regressions also cover drag, complete keyboard activation, scoped semantic actions, animation intermediate geometry, and frame-count preservation during shutdown.
Bounded settle, input-state synchronization, partially clipped targets, real virtual-list scrolling, query diagnostics, and structural comparison have focused public-API regressions.
Existing low-level unit and Runtime tests remain in place.
