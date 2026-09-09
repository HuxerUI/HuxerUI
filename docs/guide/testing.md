# Windowless UI Testing

`HuxerUI::testing` runs an `Application` through the real shared Runtime with a deterministic testing adapter and no native window.
It tests composition, state, reconciliation, layout, layers, normalized input, text editing, animation timing, accessibility output, and render intent.
It does not test native renderers, font shaping, operating-system event conversion, permissions, IME integration, native views, or browser DOM content.
Keep native integration tests for those boundaries.

## Link and create a fixture

The optional library uses the explicit header `<huxerui/testing/ui_test.h>`; it is intentionally absent from `<huxerui/huxerui.h>`.
Source consumers enable `HUXERUI_BUILD_TESTING_LIBRARY=ON` and link `HuxerUI::testing`.
An installed SDK must contain the matching testing artifact:

```cmake
find_package(HuxerUI CONFIG REQUIRED COMPONENTS testing)
add_executable(app_tests app_tests.cpp)
target_link_libraries(app_tests PRIVATE HuxerUI::testing)
```

Use `COMPONENTS testing static` or `testing shared` to request an available core link form explicitly.
The first testing lookup fixes its core dependency; later conflicting requests are rejected, while repeated `COMPONENTS testing` preserves the existing choice.
Do not request both core forms together with testing.
Link only the testing target unless another dependency is needed; it supplies one ordinary core library transitively.
Do not also link a different core form into the same process.
The library has no dependency on Catch2, XCTest, JNI test runners, or assertion macros.
The runner owns assertions and failure reporting.
Apply `huxerui_enable_codegen(app_tests)` after adding sources if the test target contains `[[huxerui::composable]]` definitions.

```cpp
#include <huxerui/testing/ui_test.h>

using namespace huxerui;
using namespace huxerui::testing;

View Counter() {
  auto count = UseState(0);
  return Column {
    Text(std::to_string(count.Get())).Key("count"),
    Button("Increment").OnClick([count]() mutable { count = count.Get() + 1; }),
  };
}

int main() {
  Application application(Counter, {.show_debug_overlay = false});
  UiTest ui(application, {.viewport = {390.0F, 844.0F}});
  ui.Find(UiSelector::Text("Increment")).Tap();
  return ui.Find(UiSelector::Key("count")).One().text == "1" ? 0 : 1;
}
```

`UiTest` borrows the application, creates its ordinary root services, and commits the initial frame at virtual time zero.
The application must outlive the test object.
Create, use, and destroy the test on one thread, normally the runner's UI thread on mobile hosts.
Root hooks still execute: direct operating-system work inside application code is not sandboxed.
An exception during frame/input processing preserves its original type and invalidates the session; create a fresh fixture after such a failure.
Invalid queries and bounded wait failures do not themselves invalidate a session.
Destruction and initialization failure destroy Runtime and then perform a best-effort UI callback drain so queued transport cancellation and disposal can run before the adapter disappears.
This drain builds no frames, waits for no external work, and uses `maximum_callbacks_per_frame` as its total callback budget.
Cleanup exceptions do not replace an initialization error or escape destruction; after the budget is exhausted, remaining callbacks are discarded and late posts are rejected.

## Query mounted content

Queries do not require accessibility labels, Semantics, or testing tags.
`Text` matches a node's own resolved text, not descendant text or a semantic label.
`Value` reads non-secure built-in TextField content without focusing it.
Missing text differs from an empty string.
`Key` uses the same signed, unsigned, or string alternative as `View::Key`; keys are unique among siblings, not globally.

```cpp
auto save = ui.Find(UiSelector::AllOf(UiSelector::Type<Button>(), UiSelector::Text("Save")));
auto remove = ui.Find(UiSelector::Key("account-row")).Find(UiSelector::Text("Remove"));
bool exists = save.Exists();
std::size_t count = save.Count();
UiNodeInfo info = save.One();
std::vector<UiNodeInfo> matches = save.All();
```

`Type<T>` supports Text, Button, IconButton, TextField, Checkbox, RadioButton, Switch, Slider, Image, and Canvas.
Custom functions are not retained C++ node types and cannot be queried as such; use text, keys, or scoped predicates.
`Enabled` and `Focused` can be combined with `AllOf`.
The match order is mounted preorder, including presentation layers and mounted virtualization overscan but excluding unmounted virtual items and native descendants.
A descendant scope must resolve uniquely, even for `Exists()`.
`One()` and targeted operations throw `UiTestFailure` on missing or ambiguous matches.

Queries re-resolve against the last completed frame and expire when the fixture is destroyed.
`UiNodeInfo` is an owning value and remains usable after later frames or destruction.
Its bounds are transformed window-local logical coordinates.
`in_viewport` is only a conservative rectangular viewport/ancestor-clip check: path clips, opacity, occlusion, and modal barriers are not an interactability guarantee.
Hidden retained content can still be found; inspect layout participation and geometry when relevant.

Accessibility uses a separate tree and vocabulary:

```cpp
auto submit = ui.FindSemantics(UiSemanticSelector::Label("Submit form"));
SemanticNode semantic = submit.One();
```

Use `Identifier`, `Label`, `Value`, `Role`, `Enabled`, `Focused`, `Selected`, `Checked`, and `AllOf` there.
`PerformSemanticAction()` deliberately uses the accessibility path; ordinary `Tap()` never falls back to it.

## Input and controlled editing

`Tap()` sends pointer Down and Up at the transformed local center, pumping after each event.
It requires one enabled target with laid-out viewport geometry, but completion does not guarantee application activation: normal Runtime hit testing and modal routing decide the recipient.
It never retries, scrolls automatically, invokes callbacks directly, or retargets Up after recomposition.
`TapAt()` accepts a window-local point.
Both default to touch with pointer ID 1; `UiPointerOptions` selects another device or unused pointer ID.

`Drag(start, end)` defaults to 200 ms and ten equally timed movement segments, pumping after Down, every Move, and Up.
`PressKey()` sends Down and Up with a frame after each; it does not synthesize text.
High-level pointer helpers reject collision with a currently active raw pointer and attempt Cancel on failure without hiding the original exception.
Raw `SendPointer`, `SendScroll`, `SendKey`, and `SendTextInput` forward normalized input without pumping and preserve meaningful Runtime results.
Use them to test cancellation, multiple pointers, rejected input, and exact frame boundaries.

```cpp
auto editor = ui.Find(UiSelector::Key("name"));
editor.Tap();
editor.EnterText("A😀B");
editor.SetSelection({1, 3});
editor.EnterText("X");
editor.ReplaceText("replacement");
editor.Submit();
```

Text helpers require the uniquely selected node to own the active input session.
They use the normal editing protocol rather than changing State directly, so the controlled application may accept, reject, or transform edits.
Selection offsets use UTF-16 units, including surrogate-pair and affinity rules.
`ReplaceText` finishes composition and requests replacement of the complete current extent.
Secure editor values are absent from ordinary observations and excluded from semantic value matching and structural serialization; built-in secure paint remains masked.

## Frames and virtual time

`Pump()` commits exactly one frame without advancing time.
`Pump(100ms)` advances virtual time once and commits one frame at that time; it does not replay intermediate frames.
An entry snapshot of queued UI callbacks runs before the frame; callbacks posted while that batch is running wait for a later Pump.
Each batch has a finite callback budget.
Ordinary Runtime frame requests, animation state, and `Delay` use the same clock.

```cpp
using namespace std::chrono_literals;
UiSnapshot start = ui.CaptureSnapshot();
ui.Pump(100ms);
UiSnapshot middle = ui.CaptureSnapshot();
ui.PumpUntil([&] { return ui.Find(UiSelector::Text("Done")).Exists(); },
             {.timeout = 2s, .step = 16ms});
```

`PumpUntil` checks immediately and then advances bounded virtual steps; its predicate may observe but must not pump or send input.
Its timeout is virtual, not wall-clock time.
It does not wait for real workers, HTTP, browser promises, or operating-system callbacks and does not claim global idleness.
Use controlled service fixtures and let the runner yield for external work; the runner must also enforce its own wall-clock timeout.

## Resources and reference text

Supply an owning `PlatformResources` through `UiTestOptions::resource_provider` when content needs images, raw assets, localized catalogs, or built-in icons.
Deploy the application's generated resource package, including built-in assets, using the application's normal resource build; the test does not search the current directory or fabricate missing resources.
The provider implements ordinary `OpenRead` and may open packaged files through `File::OpenRead` or provide fixture streams.
`UiTestOptions::resources` determines locale and density consistently, overriding the provider's configuration.
Call `UpdateResourceConfiguration()` or `SetWindowMetrics()` and then Pump to publish observations for the new configuration.

The reference text profile uses one scalar cell per decoded Unicode scalar, with advance `font.size * 0.6`, ascent `font.size`, and line height `font.size * 1.25`.
It supports newline-separated hard lines and scalar wrapping except under `NoWrap`, and keeps caret/selection/hit positions on UTF-16 scalar boundaries.
It does not reproduce native fonts, span-dependent metrics, alignment, ellipsis, bidi, ligatures, locale-sensitive breaking, or complex grapheme navigation.
Use it for deterministic shared behavior, not native typography goldens.

## Structural captures

`CaptureSnapshot()` returns an owning `UiSnapshot` of the last completed frame without pumping.
`ToString()` exposes a versioned, locale-independent textual representation of semantic hierarchy and render intent.
The representation is a structural format, not JSON or a pixel image.
It contains transforms, clips, geometry, command order, semantic state, and encoded image content, but omits Runtime identities and native handles.
ExternalTexture pixels and native PlatformView descendants are explicitly uncaptured; equal snapshots do not establish equality of that content or of native rendering.
Capture strings can be large for encoded images.
Other application text is not anonymized; runners own storage, comparisons, attachments, and explicit baseline approval.
The library does not write files or update baselines automatically.

## Android execution

Android tests run through Instrumentation and a JNI test library, not a desktop executable or a visible Activity.
The repository provides an independent windowless smoke module; from `platform/android`, run:

```bash
./gradlew :ui_testing:connectedDebugAndroidTest
```

This builds the current source, packages the test libraries and generated resources, installs the test APK on connected devices or emulators, and reports the native smoke result.
Use `-PhuxeruiAbis=arm64-v8a` to build only that ABI when appropriate.
To run the same smoke against an installed SDK instead of compiling framework sources:

```bash
./gradlew :ui_testing:connectedDebugAndroidTest -PhuxeruiTestingSdk=/absolute/path/to/sdk
```

The SDK must contain matching headers, host tools, resources, and `libhuxerui_testing.so` plus `libhuxerui.so` for the selected ABIs.
The published `HuxerUI.aar` is Java-only; native libraries are selected through CMake dependencies, not copied wholesale from the SDK.
No public testing AAR or production application dependency is added.
The test APK owns its assets; no `adb push` or `packagePath` argument is needed.
The smoke is a fixed framework check, not automatic discovery of application tests or execution of the desktop regression suite.

For application tests, create a separate Android test module with a JNI library linking `HuxerUI::testing`, supply application code and resources, and execute the C++ test body from the instrumentation runner on the UI thread.
Keep these dependencies out of the production application's native targets and Gradle dependencies.
The repository module demonstrates both source and installed-SDK linking; it is not a general test registration framework.
Enforce a wall-clock timeout in the invoking test job in addition to any virtual `PumpUntil` limit.

See [UI Testing Internals](../design/ui-testing.md) for ownership and platform carrier details.
