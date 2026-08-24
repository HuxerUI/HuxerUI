# HuxerUI Agent Guide

This file defines repository-wide rules for agents and automated contributors. A more specific `AGENTS.md` may override it for files below that directory.

## Authority and workflow

Resolve conflicts in this order:

- The repository owner's explicit instruction for the current task.
- This `AGENTS.md`.
- Current public headers and executable tests.
- README, user guides, and design documents.
- Existing implementation details.

If these sources disagree, report the conflict and agree on the intended contract before editing.

Before changing a subsystem, read its public header, implementation, focused tests, and relevant user and design documentation. Reuse an existing abstraction when it already owns the behavior.

HuxerUI's public identity is fixed:

- Namespace: `huxerui`.
- Include prefix: `<huxerui/...>`.
- Umbrella header: `<huxerui/huxerui.h>`.
- CMake targets: `HuxerUI::huxerui` and `HuxerUI::huxerui_static`.
- Scoped components: `[[huxerui::scope]]`.
- Application declaration: `huxerui::Application`.
- Native application entry: `huxerui::RunApplication()`.

Do not restore an old identity or add legacy aliases without an explicitly approved compatibility policy. When a breaking public API change is approved, migrate headers, implementation, tests, examples, and documentation together and remove the old entry point.

## Collaboration and Git safety

Discuss intended edits before modifying files. Describe the affected subsystem, files, public behavior, and important tradeoffs, then wait for confirmation. A direct request such as "implement this" or "do it" authorizes only the described scope.

Read-only inspection is allowed before confirmation. Editing, formatting that changes files, regeneration, dependency changes, staging, committing, pushing, and other repository mutations require authorization appropriate to their scope.

Assume the repository owner may edit the worktree concurrently:

- Read each target file immediately before patching it.
- Inspect `git status --short` before and after the work.
- Preserve every pre-existing staged, unstaged, and untracked change.
- Never assume all changes in a dirty file belong to the agent.
- Keep patches narrow and stop to discuss a genuine overlap.
- Do not perform unrelated cleanup or whole-repository formatting.

Never use `git reset --hard`, `git checkout -- <path>`, `git restore <path>`, `git clean`, or equivalent commands to discard owner work. Do not amend, rebase, merge, push, create branches, or create pull requests unless requested.

When asked for a commit message, use English Conventional Commits with a concise lowercase scope when useful:

```text
feat(text-field): add line and length limits
fix(android): release pressed state after pointer up
docs(architecture): define node extension lifecycle
refactor(runtime): consolidate platform adapter ownership
```

Describe the outcome, not the editing process.

## Language and formatting

Repository prose, documentation, diagnostics, identifiers, examples, and new comments are English. Preserve the language of existing comments during unrelated work.

Comments explain non-obvious invariants, lifetimes, platform limitations, or algorithmic reasons. Do not restate code or add numbered procedural comments, `Step` comments, decorative separators such as `// ---` or `// ===`, or generated-sounding narration.

Use `.clang-format` as the reference baseline for non-DSL code:

- C++ and Objective-C++: two-space indentation, 120-column limit, no tabs.
- Java: four-space indentation, 120-column limit, no tabs.
- Attach opening braces to declarations and control statements.
- Bind pointer and reference markers to the type: `Type* value` and `Type& value`.
- Do not automatically sort includes.

Do not run clang-format unless formatting is necessary for the requested change. Never apply it to whole files or UI DSL declarations. The formatter does not preserve HuxerUI DSL layout, including chained `.With(...)` placement and indentation, so format touched UI declarations manually according to the rules below. When formatting is explicitly necessary, limit it to the smallest non-DSL range changed by the task.

Follow local naming:

- Types, concepts, enum values, and public methods use `PascalCase`.
- Locals, parameters, and data members use `snake_case`; private fields normally have a trailing underscore.
- Local and core constants normally use `snake_case`; preserve an established platform-specific convention.
- C++ source and header names use lowercase `snake_case`; Java class files follow Java naming.

Markdown prose uses one semantic paragraph or list item per source line. Preserve headings, tables, fenced code blocks, and semantic blank lines instead of applying a fixed prose column width.

Do not rename public API, add forwarding layers, or normalize an unrelated subsystem for stylistic uniformity.

## UI DSL and examples

Braced UI containers have one space before `{`, children are indented two spaces, and a chain starts on the same line as the closing brace:

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

Apply the same style to production UI, tests, examples, README, and design snippets:

- Align closing braces and parentheses with their opening construct.
- Split nested structural closures instead of leaving endings such as `}}})))`.
- Keep trailing commas on child expressions and multiline arguments where local style permits.
- Keep component-specific fluent configuration vertically chained.
- Distinguish `Column { ... }` UI syntax from `Frame{.height = 160.0F}` aggregate initialization.
- Do not add wrappers, lambdas, or temporaries solely to conceal poorly formatted nesting.
- Normalize only blocks touched by the task.

Example targets use the `example_` prefix and a semantic snake-case name. A new example lives in `examples/<snake_case_name>/`, uses `huxerui_add_example`, has its own `CMakeLists.txt`, and is registered in `examples/CMakeLists.txt`.

## Architecture and ownership

The shared C++ core owns composition, state observation, reconciliation, mounted nodes, layout, virtualization, interaction semantics, animation state, and RenderScene generation. Platform adapters own native lifecycle, frame scheduling, event conversion, text services, clipboard integration, and RenderScene rendering.

Use one shared `Runtime` implementation and one `PlatformAdapter` boundary per native host view. Do not add Runtime subclasses, platform Runtime variants, or concrete-component branches in Runtime.

The shared runtime does not depend on native platform types. Add a native capability only for a genuine native service; fix behavior at the narrowest layer that owns it.

Public headers live in `include/huxerui`; implementation-only headers live in `src` or their platform directory. Every public header:

- Starts with `#pragma once`.
- Compiles when included by itself.
- Directly includes every dependency required by its declarations and templates.
- Does not depend on `<huxerui/huxerui.h>`, transitive includes, `src/internal.h`, or another private header.
- Keeps public declarations in `huxerui` and internals in `huxerui::detail`.
- Contains no `using namespace` directive.

Use forward declarations only when a complete type is unnecessary. Keep include groups stable: matching module header, standard library, public HuxerUI headers, then private quoted headers.

Do not create a public header per trivial control or grow `view.h` with unrelated services, platform types, or rendering machinery. A new cross-platform public header updates `<huxerui/huxerui.h>`, public header checks, packaging metadata when applicable, and public documentation. Platform-specific public headers remain explicit includes and update their available-host header checks, packaging metadata when applicable, and public documentation.

Use a focused `*_internal.h` for feature contracts shared by several implementations. Keep a type in `src/internal.h` only when Runtime subsystems genuinely share it.

Detailed architecture belongs in [Architecture Design](docs/design/architecture.md), text editing contracts in [Text Input and TextField Design](docs/design/text-input.md), code generation in [Scope Code Generation Design](docs/design/scope-codegen.md), and SDK or library planning in [SDK, CLI, Platform Shell, and Library Design](docs/design/sdk-cli.md).

## Public API and state

A component is an ordinary function returning `View`. The application root already owns a scope. Use `[[huxerui::scope]]` only when a reusable component needs independent local state, a component event hub, or a local recomposition boundary.

`UseState` identity belongs to the current `RecomposeScope`, source location, and occurrence at that location. Reordered dynamic content needs stable child scopes or keys. `UseEnvironment`, `UseTheme`, and `UseEvents` do not allocate ordered state slots.

State reads subscribe the current scope and writes invalidate subscribed scopes. Pass `State<T>` by value, read through conversion or `Get()`, and update through assignment or supported mutation operators; do not add another setter convention.

Unkeyed siblings reconcile by position. Dynamic siblings that insert, remove, or reorder stateful content use stable semantic keys unique among the same parent's children.

`View` is a transient copy-on-write value. Retained mutable state belongs in mounted runtime objects, not `ViewSpec`.

Keep the generic View surface centered on:

```cpp
view.With(modifiers...);
view.On<EventKey>(handler);
view.LayoutValue<Key>(value);
view.Key(stable_key);
```

Put required component values in constructors and component-only semantics in strongly typed rvalue-qualified fluent methods. Keep controllers, events, keys, and reusable node modifiers as distinct APIs. Reserve `.With(...)` for modifiers and do not add parallel options or generic property systems.

Choose the narrowest existing extension mechanism:

- Function or scoped function component for composition.
- Built-in View type for a new primitive semantic or rendering node.
- `Layout<Derived>` or `VirtualLayout<Derived>` for layout policy.
- Property modifier for reusable data applied to `ViewSpec`.
- Retained modifier and `NodeExtension` for per-node lifecycle behavior.
- Typed event for semantic output.
- Environment or Theme for inherited values.
- Root service and `LayerController` for per-window presentation.
- PaintCommand for a new platform-neutral drawing primitive.

Do not add another host, registry, callback convention, context store, or plugin abstraction when an existing mechanism fits.

User-owned text, checked state, selection, progress, and declarative visibility are controlled values. Components emit requested changes and owners provide the next authoritative value. Transient hover, pressed, animation, momentum, caret, and IME bookkeeping may remain mounted state.

Validate public configuration at the earliest correct layer. Use `std::invalid_argument` for caller input and `std::logic_error` for framework invariant failures. English diagnostics begin with `HuxerUI`.

## Extension invariants

Typed events use the event key type as identity. `OnClick`, `OnChanged`, and `OnSubmitted` delegate to built-in typed keys and never create parallel dispatch. Do not reintroduce owner types or callback parameters where component events already fit.

Layouts measure children only through their context, obey parent constraints, return constrained sizes and valid placements, and do not retain child references across reconciliation. Runtime owns child reconciliation, keyed state, clipping, hit testing, scrolling, and virtual-item cleanup.

`LayoutValue<Key>` is semantic parent-child metadata. Keep a distinct key when one value type can represent multiple meanings.

Property modifiers apply left to right and do not create wrapper nodes. Retained modifiers preserve declaration order and reconcile compatible `NodeExtension` instances by descriptor and position.

`NodeExtension` is behavior attached to one `MountedNode`, not a general plugin host. Update compatible declarative configuration without resetting retained state, do not retain raw node references, and request frame timing only through `FrameResult`. Paint-visible retained state changes call the protected `InvalidatePaint()` operation; scheduling alone does not invalidate a PaintSequence. Runtime dispatches lifecycle capabilities without concrete modifier or component checks.

Presentation modifiers affect drawing, descendants, clipping, foreground extensions, and pointer hit testing without changing measurement or parent layout. Honor reduced motion and avoid per-frame state recomposition.

PaintCommands contain platform-neutral immutable drawing data only. A new command updates its variant, type-safe PaintSequence API, command tests, public rendering documentation, and every supported renderer. Balance clip and transform commands on all paths and handle every variant explicitly.

Environment is typed hierarchical propagation, Theme is its visual specialization, and root services own per-window capabilities. Use layers only for content outside the application tree while preserving captured Environment, modal focus, and restoration.

Input behavior remains shared. Platform adapters convert native pointer, key, scroll, clipboard, and text-input operations without duplicating component state machines.

TextField is controlled by complete `TextEditingValue`. Preserve selection, affinity, composition, session identity, revision semantics, secure-data policy, and the distinction between UTF-8 bytes, UTF-16 units, Unicode scalars, and grapheme clusters. Validation reports application-owned domain state and does not filter edits.

## Platform constraints

Treat supported platforms as an open-ended set. Shared API or semantic changes audit every affected current platform and future adapter boundary. Build every affected platform available in the current environment and report unavailable or unverified platforms.

### Android

Android C++ host code lives under `platform/android/huxerui/src/main/cpp`; Java host code lives under `platform/android/huxerui/src/main/java/org/huxerui`.

JNI changes update C++ signatures, Java declarations, cached IDs, and call sites together. Validate handles and session IDs, manage local and global references, preserve pending exceptions, and keep View work on the UI thread.

The minimum Android API is 23. Guard newer APIs or obtain approval to raise it.

### iOS

iOS platform configuration lives in `cmake/platform/IOS.cmake`. Register new UIKit sources and frameworks there.

Keep UIKit application lifecycle, `UITextInput`, and `CADisplayLink` work on the main thread. Preserve ARC-compatible ownership, balance Core Foundation and Core Graphics Create/Copy objects, and convert native coordinates at the View boundary. The minimum deployment target is iOS 13.

### macOS

macOS platform configuration lives in `cmake/platform/MacOS.cmake`. Register new AppKit sources and frameworks there.

Keep AppKit work and `NSTextInputClient` interaction on the main thread. Preserve ARC-compatible ownership, balance Core Foundation and Core Graphics Create/Copy objects, and convert native coordinates at the host boundary.

### Windows

Windows platform configuration lives in `cmake/platform/Windows.cmake`. The backend targets Windows 10 and later by default. `HUXERUI_WINDOWS_7_COMPAT=ON` may target Windows 7 SP1 with Platform Update by dynamically resolving newer Win32 APIs and retaining capability-based rendering fallbacks. Windows 7 without Platform Update is unsupported.

Keep shared layout in DIPs and convert pixels, screen coordinates, DPI, UTF-16, and IME geometry at the boundary. Check HRESULTs, handles, COM lifetime, clipboard ownership, and resource recreation; prefer RAII and `ComPtr`.

### Linux

Linux platform configuration lives in `cmake/platform/Linux.cmake`; source files live under `platform/linux/`. The graphics and text stack (Cairo, fontconfig, pixman, FreeType, HarfBuzz, libpng, libjpeg, zlib, expat) is fetched from pinned git tags and built as static libraries through FetchContent and meson-driven ExternalProject into a staging prefix; X11, xkbcommon, EGL, and OpenGL ES 2 resolve through pkg-config.

Include `linux_internal.h` before any huxerui header in Linux sources: Xlib defines C macros (`None`, `Bool`, `True`, `False`, `Status`) that collide with shared enumerators, and the header undefines them after including X headers. Keep the shared layout in DIPs and convert pixels, DPI, and IME geometry at the host boundary. The renderer rasterizes PaintCommands with Cairo into a retained bitmap and presents it through an EGL/OpenGL ES swap; keep damage-limited Cairo redraw and whole-bitmap or damaged-row presentation. X11 clipboard transfers and XIM composition are event-driven: never block the event loop without a timeout.

Add equivalent focused guidance when a new backend gains repository-owned implementation.

## Build, validation, and documentation

Use C++20 with extensions disabled. Preserve `-Wall -Wextra -Wpedantic` and MSVC `/W4 /permissive-`.

Shared `src/*.cpp` files belong to the core object target; platform sources and libraries are selected through `cmake/platform/*.cmake`. Consumers link public targets and never include `src` or depend on source-checkout platform paths.

Enable `[[huxerui::scope]]` transformation with `huxerui_enable_codegen(target)` after all sources are added. Marked definitions belong in `.cpp`, `.cc`, or `.cxx`; do not annotate the app root.

Host tools in `tools/prebuilt/<host>/<architecture>` run on the development host, not the target. Scope syntax, transformation, source discovery, generated code, or entry-point changes update codegen tests, Runtime tests, CMake, documentation, and required host tools together. Do not edit generated files.

Place tests by ownership under `tests/unit`, `tests/runtime`, `tests/platform`, or `tests/codegen`. Tests verify public outcomes and invariants rather than private steps. Cover mount, compatible recomposition, replacement, unmount, keyed movement, Cancel and disabled input paths, deterministic animation time, and exception categories where relevant.

Use the project's existing configured build directory and native host toolchain. Do not let a generic command silently select another compiler, generator, ABI, or binary format.

Validate the current host by default. Build another platform only when the change affects that platform's adapter, build configuration, packaged artifact, or shared boundary requiring platform verification, or when the owner requests it.

After every platform-scoped change, explicitly assess whether shared CMake, CLI, packaging, public headers, or Runtime code can affect another platform. Test every affected platform available in the current environment and report each unavailable platform; a platform guard alone is not evidence that other platforms are unaffected.

Treat a confirmed host-specific test limitation as unavailable validation rather than a product regression. Report the limitation and run the closest valid target instead of repeatedly invoking an incompatible toolchain or assertion.

Validation depth is proportional to the affected contract:

- Documentation: current API names, links, snippets, fences, and formatting.
- Public header or shared API: header checks, common tests, affected examples, the current-host build, and affected platform builds when required by the boundary above.
- Component or Runtime: focused tests, full common tests, and affected platform integration.
- Layout or virtualization: geometry, constraints, state restoration, and common tests.
- Modifier or NodeExtension: reconciliation, scheduling, input, paint order, and animation.
- PaintCommand: command tests, every renderer audit, and every available platform build.
- Text input or TextLayout: reducer, TextField, Runtime session, native adapter, common, and affected platform tests.
- Codegen: transform, generated Runtime behavior, common build, and required host tools.
- CMake or packaging: an incremental build plus a separate clean configure and build.

Use an existing compatible build directory and do not delete owner-managed output to repair configuration.

Public API or behavior changes update the relevant user and design documentation. Update README when the project overview, supported platforms, quick start, documentation index, or example list changes.

Proposed or deferred work is marked clearly. An example teaches one primary capability; `example_ui_gallery` remains the compact overview. Examples use public API, controlled state, stable keys for dynamic stateful items, and layouts usable across configured viewports.

Finish with `git diff --check` and `git status --short`. Report important files, exact validation outcomes, unavailable platforms, remaining limitations, and whether anything was staged or committed. Never claim an unexecuted target, architecture, platform, or test passed.
