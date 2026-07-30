# HuxerUI Agent Guide

This file defines repository-wide rules for agents and automated contributors. A more specific `AGENTS.md` may
override it for files below that directory.

## Authority and sources of truth

Resolve conflicts in this order:

- The repository owner's explicit instruction for the current task.
- This `AGENTS.md`.
- Current public headers and executable tests.
- README and design documents.
- Existing implementation details.

If these sources disagree, report the conflict and agree on the intended contract before editing. Before changing a
subsystem, read its public header, implementation, focused tests, relevant README section, and design document. Reuse
an existing abstraction when it already owns the behavior.

HuxerUI's public identity is fixed:

- Namespace: `huxerui`.
- Include prefix: `<huxerui/...>`.
- Umbrella header: `<huxerui/huxerui.h>`.
- CMake targets: `HuxerUI::huxerui` and `HuxerUI::huxerui_static`.
- Scoped components: `[[huxerui::scope]]`.
- Application entry: `HUXERUI_APP`.

Do not restore an old identity or add legacy aliases without an explicitly approved compatibility policy. When a
breaking public API change is approved, migrate headers, implementation, tests, examples, and documentation together;
remove the old entry point instead of adding an unrequested forwarding alias.

## Collaboration and Git safety

Discuss intended edits before modifying files. Describe the subsystem, affected files, public behavior, and important
tradeoffs, then wait for confirmation. A direct request such as "implement this" or "do it" authorizes only the
described scope.

Read-only inspection is allowed before confirmation. Editing, formatting that changes files, regeneration, dependency
changes, staging, committing, pushing, and other repository mutations require authorization appropriate to their
scope.

Assume the repository owner may edit the worktree concurrently:

- Read each target file immediately before patching it.
- Inspect `git status --short` before and after the work.
- Preserve every pre-existing staged, unstaged, and untracked change.
- Never assume all changes in a dirty file belong to the agent.
- Keep patches narrow and stop to discuss a genuine overlap.
- Do not perform unrelated cleanup or whole-repository formatting.

Never use `git reset --hard`, `git checkout -- <path>`, `git restore <path>`, `git clean`, or equivalent commands to
discard owner work. Do not amend, rebase, merge, push, create branches, or create pull requests unless requested.

When asked for a commit message, use English Conventional Commits:

```text
feat(text-field): add line and length limits
fix(android): release pressed state after pointer up
docs(architecture): define node extension lifecycle
refactor(runtime): consolidate platform host ownership
test(virtual-list): cover keyed state restoration
```

Use a concise lowercase scope when useful. Describe the outcome, not the editing process.

## Language, comments, and formatting

Repository prose, documentation, diagnostics, identifiers, examples, and new comments are English. Preserve the
language of existing comments during unrelated work.

Comments explain non-obvious invariants, lifetimes, platform limitations, or algorithmic reasons. Do not restate code
or add numbered procedural comments, `Step` comments, decorative separators such as `// ---` or `// ===`, or
generated-sounding narration.

Use `.clang-format` as the baseline:

- C++ and Objective-C++: two-space indentation, 120-column limit, no tabs.
- Java: four-space indentation, 120-column limit, no tabs.
- Attach opening braces to declarations and control statements.
- Bind pointer and reference markers to the type: `Type* value` and `Type& value`.
- Do not automatically sort includes.

Run formatters only on files or ranges changed by the task. Generic clang-format does not preserve every HuxerUI DSL
rule, so inspect UI declarations manually afterward.

Follow local naming:

- Types, concepts, enum values, and public methods use `PascalCase`.
- Locals, parameters, and data members use `snake_case`; private fields normally have a trailing underscore.
- Local/core constants normally use `snake_case`. Preserve an established platform-specific convention.
- C++ source and header names use lowercase `snake_case`; Java class files follow Java naming.

Do not rename public API, add forwarding layers, or normalize an unrelated subsystem for stylistic uniformity.

## UI DSL and examples

Braced UI containers have one space before `{`, children are indented two spaces, and a chain starts on the same line
as the closing brace:

```cpp
return Column {
  Text("Title"),
  Row {
    Button("Cancel"),
    Button("Confirm"),
  }.With(Spacing(8.0F)),
}.With(
    Padding(16.0F),
    CrossAlign(CrossAxisAlignment::Stretch)
);
```

Apply the same style to examples, README and design snippets, demonstrative tests, and production UI:

- Align closing braces and parentheses with their opening construct.
- Split nested structural closures instead of leaving endings such as `}}})))`.
- Keep trailing commas on child expressions and multiline arguments where local style permits.
- Keep component-specific fluent configuration vertically chained.
- Distinguish `Column { ... }` UI syntax from `Frame{.height = 160.0F}` aggregate initialization.
- Do not add wrappers, lambdas, or temporaries solely to conceal poorly formatted nesting.
- Normalize only blocks touched by the task.

Example targets use the `example_` prefix and a semantic snake-case name. A new example lives in
`examples/<snake_case_name>/`, uses `huxerui_add_example`, has its own `CMakeLists.txt`, and is registered in
`examples/CMakeLists.txt`.

## Architecture and file ownership

The shared C++ core owns composition, state observation, reconciliation, mounted nodes, layout, virtualization,
interaction semantics, animation state, and DisplayList generation. Platform adapters own native lifecycle, frame
scheduling, event conversion, text services, clipboard integration, and DisplayList rendering.

```text
State or Environment invalidation
        |
RecomposeScope
        |
ViewSpec reconciliation
        |
MountedNode and NodeExtension
        |
measure, layout, hit testing, and frame callbacks
        |
DisplayList
        |
platform adapter
```

The shared runtime does not depend on native platform types. Platform adapters may depend on the public API and
narrowly scoped internal host contracts. Do not bypass the shared flow with native widgets or Runtime branches for
concrete controls, animations, ScrollBar, Ripple, Dialog, or similar features. Add a native capability only for a
genuine native service.

Public headers live in `include/huxerui`; implementation-only headers live in `src` or their platform directory.
Every public header:

- Starts with `#pragma once`.
- Compiles when included by itself.
- Directly includes every dependency required by its declarations and templates.
- Does not depend on `<huxerui/huxerui.h>`, transitive includes, `src/internal.h`, or another private header.
- Keeps public declarations in `huxerui` and internals in `huxerui::detail`.
- Contains no `using namespace` directive.

Use forward declarations only when a complete type is unnecessary. Keep include groups stable: matching module header,
standard library, public HuxerUI headers, then private quoted headers, following local style when no matching header
exists.

Place declarations by responsibility:

- Geometry and constraints: `geometry.h`.
- Drawing commands: `display_list.h`.
- Layout protocol and public `MountedNode`: `layout.h`.
- Virtualization protocol: `virtual_layout.h`.
- Generic modifiers and `NodeExtension`: `modifier.h`.
- Pointer/key events and typed event keys: `event.h`.
- Native-independent text input: `text_input.h`.
- Pure validation values and rules: `validation.h`.
- State, Environment, Theme, layers, root services, animation, presentation, and scrolling: matching headers.
- Built-in primitive Views: `view.h`, unless the feature becomes an independent subsystem.

Do not create a public header per trivial control or grow `view.h` with unrelated services, platform types, or
rendering machinery. A new public header must be added to `<huxerui/huxerui.h>`,
`HUXERUI_PUBLIC_HEADERS` in `tests/CMakeLists.txt`, packaging metadata when applicable, standalone header checks, and
public documentation.

Use a focused `*_internal.h` for feature contracts shared by several implementations. Keep a type in
`src/internal.h` only when Runtime subsystems genuinely share it.

## Functional UI, state, and identity

A component is an ordinary function returning `View`. The application root already owns the root scope and is not
annotated.

Use `[[huxerui::scope]]` when a reusable component needs independent local state, a component event hub, or a local
recomposition boundary:

```cpp
[[huxerui::scope]]
View Counter() {
  auto count = UseState(0);
  return Column {
    Text::Format("Count {}", count),
    Button("+1").OnClick([count] {
      count += 1;
    }),
  };
}
```

Prefer the attribute in application and example source. `HUXERUI_SCOPE_BEGIN` and `HUXERUI_SCOPE_END` are codegen
output and low-level test mechanisms. Stateless helpers participate in the caller's scope.

`UseState` identity is the current `RecomposeScope`, source location, and occurrence at that location:

- Different call sites do not shift one another.
- Repeated calls from one location use occurrence order.
- Slots not touched in a completed composition are removed.
- Reordered loops need stable child scopes or keys to avoid associating state with the wrong item.

Do not apply React's global hook-order rule to every HuxerUI `Use*` function. `UseEnvironment`, `UseTheme`, and
`UseEvents` do not allocate ordered state slots.

State reads subscribe the current scope. Writes invalidate subscribed scopes and coalesce into a later frame.
`State<T>` is a lightweight shared handle: pass it by value, read through conversion or `Get()`, and update through
assignment or supported mutation operators. Do not add another setter convention.

Unkeyed siblings reconcile by position. Dynamic siblings that insert, remove, or reorder stateful content use stable
semantic keys unique among the same parent's children. Do not use a visible index when a movable item has a stable
domain identity.

`View` is a transient copy-on-write value. Builder and fluent APIs preserve copy isolation. Retained mutable state
belongs in mounted runtime objects, not `ViewSpec`.

## Public API and extension choices

Keep the generic View surface centered on:

```cpp
view.With(modifiers...);
view.On<EventKey>(handler);
view.LayoutValue<Key>(value);
view.Key(stable_key);
```

Put required component values in constructors and component-only semantics in strongly typed rvalue-qualified fluent
methods. Consolidate coupled settings into one value type:

```cpp
TextField(value)
    .LineLimits(TextFieldLineLimits::MultiLine(1, 8))
    .MaxLength(200)
    .OnChanged(handler)
    .With(Frame{.height = 160.0F});
```

Keep controllers, events, keys, and reusable node modifiers as distinct APIs. Reserve `.With(...)` for modifiers; do
not add parallel options or generic property systems. Component-only settings are not public ViewModifiers.

Choose the narrowest existing extension mechanism:

- Function or scoped function component for composition.
- Built-in View type for a new primitive semantic or rendering node.
- `Layout<Derived>` or `VirtualLayout<Derived>` for layout policy.
- Property modifier for reusable data applied to `ViewSpec`.
- Retained modifier and `NodeExtension` for per-node lifecycle behavior.
- Typed event for semantic output.
- Environment or Theme for inherited values.
- Root service and LayerController for per-window presentation.
- DisplayCommand for a new platform-neutral drawing primitive.

Do not add another host, registry, callback convention, context store, or plugin abstraction when an existing mechanism
fits.

User-owned values such as text, checked state, selected value, progress, and declarative visibility are controlled.
The component emits a requested change and the owner supplies the next authoritative value. Hover, pressed pointer
IDs, caret blink, animation progress, momentum, and IME bookkeeping may be retained internally.

Validate public configuration at the earliest correct layer. Use `std::invalid_argument` for invalid caller input and
`std::logic_error` for framework invariant failures. English diagnostics begin with `HuxerUI`.

## Typed events

Define semantic event keys with `Event<Arguments...>`:

```cpp
struct SearchSubmitted : Event<std::string> {};
```

Subscribe with `.On<Key>(handler)` and emit component events through `UseEvents().Emit<Key>(arguments...)`.
Incompatible signatures fail at compile time. Each key has at most one handler on a View; a later registration
replaces it. Events do not bubble, and emitting without a handler is a no-op.

Group related built-ins in families such as `ViewEvents`, `ToggleEvents`, and `TextFieldEvents`. `OnClick`,
`OnChanged`, and `OnSubmitted` delegate to the same typed keys and never create parallel dispatch. The event key type
is its identity; do not add owner types or pass semantic callbacks through every component function.

## Layout and virtualization

Eager layouts derive from `Layout<Derived>` and implement:

```cpp
static LayoutResult Measure(
    LayoutContext& context,
    MountedNode& node,
    Constraints constraints
);
```

Layout invariants:

- Measure children only with `LayoutContext::Measure`.
- Treat parent `Constraints` as authoritative and return a constrained size.
- Place every measured child that participates in the result.
- Return placements relative to the content origin; Runtime applies origin and Padding.
- Do not mutate the child tree, call Runtime, or retain child references across reconciliation.
- Use `MountedNode::Cache<T>()` for mounted-lifetime derived data.
- Use typed `LayoutValue<Key>()` for parent-specific child metadata.
- `Frame` describes outer size; parent constraints win and Padding is inside it.
- `Grow` is parent policy and expands only on a bounded main axis.
- Different concrete layout descriptors do not reuse one mounted layout node.

Virtual containers derive from `VirtualLayout<Derived>` and implement:

```cpp
static VirtualLayoutResult Measure(
    VirtualLayoutContext& context,
    MountedNode& node,
    Constraints constraints
);
```

Virtual-layout invariants:

- Read logical count and viewport from the context.
- Request only visible and finite cache-extent items.
- Measure requested items through the context and include retained items in the final result.
- Return content-coordinate placements, scroll axis, viewport size, and total content size.
- Require bounded constraints on the scroll axis and validate extents.
- Let Runtime own reconciliation, duplicate-key checks, saved state, clipping, hit testing, and cleanup.
- `MountedNode::Children()` contains mounted items, not the logical collection.
- Do not maintain another mounted-item tree or state archive.
- Preserve the scroll anchor when fixed, estimated, or variable extents are corrected.

Items already support `.Key(...)`; add a keyed overload only for a distinct safety or performance benefit.

## Modifiers, NodeExtension, and animation

A property modifier applies reusable data to `ViewSpec` and is not retained. A retained modifier creates a persistent
`NodeExtension`; a descriptor may do both when required.

Property modifiers apply left to right, with the later write to one property winning. They do not create wrapper
nodes. Retained modifiers preserve declaration order. Prefer the automatic typed extension adapter; use an explicit
`ModifierDescriptor` only when the modifier also applies to `ViewSpec`, needs custom type erasure, or cannot satisfy
the automatic contract.

`NodeExtension` is behavior attached to one `MountedNode`, not a View, tree node, layout, component, or general plugin
host. Reconciliation matches by descriptor and retained-modifier position:

- A match reuses the extension and calls `Update`.
- A mismatch or reorder creates a new extension and destroys the displaced one.
- Removing the modifier destroys its extension.

The constructor establishes valid state, normally through `Update`. `Update` refreshes declarative configuration
without resetting compatible retained state. Use the callback's `MountedNode&`; do not retain raw node or child
references across reconciliation.

`OnFrame` advances retained time state. Return `needs_frame` only while an immediate frame is required and
`wake_after` for a known future deadline. Do not request Runtime frames directly or trigger ordinary state
recomposition every frame.

Runtime resets local presentation transform and opacity before extension traversal. `Offset`, `Opacity`, `Scale`, and
`Rotation` affect a View, descendants, clips, foreground extensions, and pointer hit testing without changing
measurement or parent layout. Animation state lives in the extension, retargets from the current presentation, honors
reduced motion, validates finite input, and avoids per-frame recomposition.

Extension Paint is a foreground pass after View content and children; the focus ring follows extensions. Scroll
containers restore their child clip before foreground extensions. Paint from `MountedNode::Frame()`, treat
`PresentationFrame()` as a transformed window-space bound, apply `PresentationOpacity()` once, and emit DisplayList
commands only.

Frame, paint, scroll, focus, key, and text-client callbacks follow declaration order. Extension hit testing runs in
reverse order with pointer coordinates inverse-mapped into Frame space.

Respect pointer IDs and treat Cancel as carefully as Up:

- `Ignored`: no participation.
- `Observe`: receive later events without owning the gesture.
- `Handled`: consume Down without retaining later delivery.
- `Capture`: retain delivery until Up, Cancel, unmount, or invalidation.

Only an input-capable extension overrides `GetTextInputClient()`. It returns one stable shared client for the mounted
lifetime. Do not add Runtime checks for a concrete modifier or component; Runtime dispatches fixed lifecycle
callbacks.

## DisplayList and rendering

`DisplayList` is the shared platform-neutral rendering output in HuxerUI logical coordinates. Before adding a command,
verify existing rect, text, circle, arc, border, clip, and transform commands cannot compose the result.

A new primitive updates together:

- Its command value and `DisplayCommand` variant.
- A type-safe `DisplayList` method.
- Every supported platform renderer and native bridge it requires.
- Command value/order tests and public rendering documentation.

Commands never contain native handles, callbacks, Views, MountedNodes, Runtime pointers, or mutable retained state.
Convert density, DPI, coordinate systems, and native conventions only in platform adapters.

Every `PushClip` has one `PopClip` and every `PushTransform` one `PopTransform`, including early returns. Every
platform visitor handles every variant alternative; never silently ignore a command.

## Environment, Theme, services, and layers

Environment is typed hierarchical propagation. Providers create nested frames and consumers read
`UseEnvironment<Value>()`. The value type owns its fallback through `Value::Default()` and is also its Environment
identity. Use Environment for ambient values, not another context registry or repetitive parameter plumbing.

Theme is a deferred Environment provider. Invoke its content after the nested Environment is active. Built-ins resolve
semantic tokens or component style values from the nearest Theme; explicit modifiers apply afterward and win. A
component style represents coherent themeable appearance, not component state.

Root hooks install per-window services and may use LayerController; they do not replace the application root. Built-in
Toast and Dialog services install automatically.

Use layers for content outside the application content tree. Preserve per-window ownership, captured Environment,
stable ordering, modal focus trapping, and focus restoration.

Declarative `Dialog` is a retained modifier whose lifetime belongs to one mounted owner and whose visibility remains
caller-controlled. Command-oriented `DialogHandle` is a root service with `Show`, `Update`, and `Dismiss`; it owns
window-level presentation and is not a modifier.

## Input, focus, text editing, and validation

Platform adapters convert native input into shared pointer, scroll, key, and text-input commands. They do not duplicate
Click, focus traversal, selection, undo, scroll, or component state machines.

Pointer behavior covers mouse, touch, pen, multiple IDs, Down/Move/Up/Cancel, capture, unmount, presentation
transforms, scroll arbitration, Click cancellation, and disabled barriers. Pressed visuals release on Up or Cancel,
including outside the original bounds.

Runtime owns focus identity. Interactive built-ins follow their semantics; custom Views opt in with `Focusable`. Tab
navigation skips disabled nodes. Modal layers constrain traversal and restore prior focus.

TextField is controlled by complete `TextEditingValue`: text, selection, affinity, and composition. `OnChanged` emits
the complete next value. Shared text input owns sessions, revisions, commands, selection, composition, undo/redo,
actions, secure policy, and geometry; native adapters own IME lifecycle and conversion.

Do not interchange UTF-8 bytes, UTF-16 code units, Unicode scalars, and grapheme clusters. Test emoji, surrogate
pairs, combining sequences, and grapheme boundaries. IME composition remains one transaction across unrelated
recomposition; authoritative controlled updates use revision and restart semantics.

Secure input does not expose real text through DisplayCommands, Copy/Cut, Android extracted or surrounding text, or
macOS attributed substring queries. Preserve native secure-input cleanup.

Content validation is separate from input filtering:

- `Validate()` and validation rules are pure and return `ValidationResult`.
- The application decides when validation runs and supplies the result.
- TextField renders the result without rejecting edits or mutating controlled values.
- Editing owns length, character, and IME policies; validation owns domain rules.
- `Pending` is controlled application state.

## Platform adaptation

`PlatformHost` exposes only capabilities required by shared Runtime: scheduling, monotonic time, text measurement and
layout, native text input, and clipboard access. Native hosts construct the same Runtime from one `AppDefinition` and
one `PlatformHost`, call `BuildFrame()`, and render its DisplayList.

Do not add Runtime subclasses or platform Runtime variants. Public semantics do not vary through platform
preprocessor branches. Fix issues at the narrowest layer: shared semantics, event conversion, native service contract,
rendering translation, or documented platform limitation.

Treat the supported-platform set as open-ended. Shared API and semantic changes audit every current platform and every
new platform added later, including renderer, input, lifecycle, build, packaging, and tests where applicable. Do not
hardcode current platforms as an exhaustive architectural set. Build every affected platform available in the current
environment and report unavailable or unverified platforms.

Changes to `detail::TextLayout` require parity audits for measurement, wrapping, HitTest, caret geometry, range
rectangles, and grapheme, word, and visual-line navigation across all text backends.

### Android

Android C++ host code is under `platform/android/huxerui/src/main/cpp`; Java host code is under
`platform/android/huxerui/src/main/java/org/huxerui`.

JNI changes update C++ signatures, Java declarations, cached IDs, and call sites together. Use explicit casts,
validate handles and session IDs, manage local/global references, preserve pending exceptions, and keep View work on
the UI thread. Java maps MotionEvent, hover, wheel, keyboard, EditorInfo, InputConnection, viewport, and Canvas to the
shared contract.

Minimum Android API is 23. Guard newer APIs or obtain approval to raise it.

### macOS

macOS uses Objective-C++, AppKit, CoreGraphics, CoreText, Carbon, and QuartzCore as configured in
`cmake/platform/Apple.cmake`.

Keep `NSTextInputClient` in the focused AppKit adapter. It forwards commands and geometry to Runtime and does not own
another text model. Use ARC-compatible ownership, balance Core Foundation/Core Graphics Create/Copy objects, keep
AppKit work on the main thread, and convert native coordinates at the host boundary.

Register new sources/frameworks in `Apple.cmake`. Put focused native text-input tests under `tests/platform`.

### Windows

Windows uses Win32, Direct2D, DirectWrite, and IMM. Keep shared layout in DIPs and convert pixels, screen coordinates,
DPI changes, and IME geometry at the boundary.

Check HRESULTs, handles, COM lifetime, clipboard ownership, and resource recreation. Prefer RAII and `ComPtr`. Keep
UTF-8 in shared APIs and convert to UTF-16 at the boundary. Register dependencies in
`cmake/platform/Windows.cmake`; do not weaken the Windows 10 target or warning policy without approval.

Add equivalent platform-specific guidance when a new backend gains repository-owned implementation.

## CMake, codegen, tests, and completion

Use C++20 with extensions disabled. Preserve `-Wall -Wextra -Wpedantic` and MSVC `/W4 /permissive-`.

Shared `src/*.cpp` files belong to the core object target; platform sources and libraries are selected through
`cmake/platform/*.cmake`. Consumers link public targets and never include `src` or know source-checkout platform
paths.

Enable `[[huxerui::scope]]` transformation with `huxerui_enable_codegen(target)` after all sources are added. Marked
definitions belong in `.cpp`, `.cc`, or `.cxx`; do not annotate the app root.

Host tools in `tools/prebuilt/<host>/<architecture>` run on the development host, not the target. Scope syntax,
transformation, source discovery, generated code, or entry-point changes update generator tests, runtime tests, CMake,
documentation, and the required host tools together. Do not edit generated files.

Place tests by ownership:

- Composition/scope identity: `tests/runtime/composition.cpp`.
- Interaction/focus: `tests/runtime/interaction.cpp`.
- Eager layout: `tests/runtime/layout.cpp`.
- Presentation/animation/NodeExtension: `tests/runtime/presentation.cpp`.
- Scrolling: `tests/runtime/scrolling.cpp`.
- Virtualization: `tests/runtime/virtual_layout.cpp`.
- TextField: `tests/runtime/text_field.cpp`.
- Pure text-input reducer/protocol: `tests/unit/text_input.cpp`.
- Runtime text-input sessions: `tests/runtime/text_input.cpp`.
- Pure validation: `tests/unit/validation.cpp`.
- Native platform behavior: `tests/platform`.
- Codegen transformation/runtime: `tests/codegen`.

Tests verify public outcomes and invariants, not private steps. Rendering tests inspect DisplayCommands. Stateful tests
cover mount, compatible recomposition, replacement, unmount, and keyed movement. Input tests include Cancel and
disabled paths. Animation tests use deterministic time. Invalid API tests verify exception category. Never weaken or
delete assertions merely to make a change pass.

On macOS, validate a Debug build in a compatible build directory. The following uses `build` as an example:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Android validation from `platform/android`:

```bash
./gradlew :demo:assembleDebug
```

On Windows, use a CMake generator, compiler toolchain, architecture, and configuration supported by HuxerUI and
available in the current environment. Run CTest with the matching configuration. Add documented build and test
commands for each new platform backend.

Validation depth is proportional to the affected contract:

- Documentation: current API names, links, snippets, and formatting.
- Public header or shared API: header checks, common tests, affected examples, and every supported platform build
  available locally.
- Component or Runtime: focused tests, full common tests, and affected platform integration.
- Layout or virtualization: geometry, state restoration, and common tests.
- Modifier or NodeExtension: reconciliation, scheduling, input, paint order, and animation.
- DisplayCommand: every supported renderer audited and every available platform built.
- Text input or TextLayout: reducer, TextField, native adapter, common, and affected platform tests.
- Codegen: transform, generated runtime, common build, and required host tools.
- CMake or packaging: an incremental build plus a separate clean configure/build.

Use an existing compatible build directory and do not delete owner-managed build output to repair configuration.

Public API or behavior changes update README and relevant design documents. Proposed or deferred work is marked
clearly. An example teaches one primary capability; `example_ui_gallery` remains the compact overview. Examples use
public API, correct controlled state, stable keys for dynamic stateful items, and layouts usable across configured
viewports. Register new targets and update the README example list.

Finish with `git diff --check` and `git status --short`. Report important files, exact validation outcomes, unavailable
platforms, remaining limitations, and whether anything was staged or committed. Never claim an unexecuted target,
architecture, platform, or test passed.
