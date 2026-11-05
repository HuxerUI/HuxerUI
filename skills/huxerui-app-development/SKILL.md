---
name: huxerui-app-development
description: "Develop HuxerUI applications and app-side components using public SDK APIs, CMake/CLI, or mcpp modules. Use for HuxerUI project setup, UI, integration, review, and troubleshooting; not framework internals or generic C++."
---

# HuxerUI App Development

Use the project's selected HuxerUI SDK or package to create, modify, review, or troubleshoot applications and app-side reusable components. Framework internals, renderers, SDK packaging, and the framework's own build are outside this skill's scope.

## Recognize the project

Treat the task as HuxerUI SDK application work when the user names HuxerUI or its CLI, or when the project contains one of these signals:

- `find_package(HuxerUI CONFIG REQUIRED)`
- `HuxerUI::huxerui` or `HuxerUI::huxerui_static`
- an include under `<huxerui/...>`
- `import huxerui;` in a source or module interface unit
- an `mcpp.toml` that depends on `huxerui.huxerui`, beside a `build.mcpp` that imports `huxerui.rules`

Inspect the manifest and build program to identify the build path; an import alone does not establish the project's complete build setup. Generated mcpp package projects use `mcpp build` and `mcpp run`, obtain HuxerUI through a dependency, and have no generated CMake platform shells. Read [cpp-modules-and-mcpp.md](references/cpp-modules-and-mcpp.md) for that path. Read [mcpp-build.md](references/mcpp-build.md) only for an existing mcpp project that manually consumes a prebuilt SDK.

## Establish the SDK truth

Before relying on an API, identify the SDK or dependency revision selected by the project. For an mcpp package project, inspect its dependency declaration and resolved package's public interface. For an installed CMake SDK:

1. Read `HuxerUI_DIR` from the active build directory's `CMakeCache.txt`, then derive the SDK prefix from `lib/cmake/HuxerUI`.
2. Otherwise use `HUXERUI_HOME` when it identifies an installed SDK.
3. Otherwise locate `huxerui` on `PATH` and run `huxerui doctor`.
4. If none is available, ask for the SDK location before relying on exact API or build behavior.

Prefer the selected version's public headers, module interface, package configuration, and user documentation over this skill. If the project explicitly uses a source dependency, inspect that dependency's public contracts. Do not substitute an unrelated checkout or private implementation for the selected SDK, and do not invent compatibility aliases.

## Work from public contracts

- Components are ordinary C++ functions returning `View`.
- `View` is a lightweight copy-on-write declaration value. Pass and return it by value; do not add `std::move` to temporary chains, container children, or return statements as a routine optimization.
- Mark a reusable function `[[huxerui::composable]]` when it directly calls a composition-bound `UseXxx()` API or needs its own recomposition lifetime. Do not annotate the application root.
- One application Runtime owns services and application tasks; each UiWindow owns an independent UI. Choose state and task lifetimes deliberately, and use public application/window handles for ordinary app work.
- Keep controlled values authoritative in app state and write emitted changes back for the next composition. Read `State<T>` through implicit conversion by default and use `state->member` for member access.
- Use stable semantic keys for dynamic stateful siblings that insert, remove, or reorder.
- Preserve complete `TextEditingValue` state for `TextField`.
- Use only public headers, public CMake targets, generated project conventions, and platform-specific public headers.
- Prefer generated `ImageResource` values for static artwork displayed by HuxerUI components. Keep scalable in-UI artwork as SVG under `resources/images`; use `VectorAsset::Create` or `Canvas` only for genuinely programmatic or dynamic drawing. Platform application and system-tray icons follow their separate packaging contracts.
- Compose built-in layouts and their modifiers before deriving `Layout` or `VirtualLayout`. A custom layout is for a genuinely new measurement or placement algorithm, not ordinary alignment, spacing, growth, wrapping, overlay, paging, scrolling, or responsiveness.
- Start application visuals from `FlatTheme`, `FlatDarkTheme`, `MaterialTheme`, or `MaterialDarkTheme`. Customize their tokens or typed styles instead of recreating a built-in theme with a bare `ThemeDefinition`.
- Choose the narrowest extension mechanism after exhausting its built-in equivalent. Prefer an ordinary component, existing event or modifier, Environment/Theme value, presentation service, or root service. Use a custom `Layout`, retained modifier with `NodeExtension`, `PlatformModule`, `PlatformView`, or `ExternalTexture` only when its distinct geometry, mounted lifetime, or platform boundary is actually required.
- Do not invent APIs, legacy names, private includes, or source-checkout paths.

## Route the task

Read only the references needed for the request:

- Creating, opening, configuring, building, running, or diagnosing a project: [project-workflow.md](references/project-workflow.md)
- Windowless UI tests and lifecycle injection: [project-workflow.md](references/project-workflow.md#ui-testing)
- Writing or editing any UI declaration or snippet: [dsl-style.md](references/dsl-style.md)
- Selecting and configuring built-in controls, TreeView, date/time pickers, or their controlled values: [components.md](references/components.md)
- Arranging UI, scrolling, virtualization, responsiveness, app shells, mounted coordinate conversion, or any proposed custom layout: [layout-and-ui.md](references/layout-and-ui.md)
- Composition, state, keyboard routing and shortcuts, events, environment, lifecycle, tasks, and callback integration: [fundamentals.md](references/fundamentals.md)
- `.With(...)`, events, keys, and modifier ownership: [modifiers.md](references/modifiers.md)
- Pointer buttons, hover, pointer cursors, context-menu requests, raw pointer input, pointer interception, repeated tap, long press, drag, multi-pointer transform, typed in-process drag-and-drop, and external file reception: [gestures-and-drag-drop.md](references/gestures-and-drag-drop.md)
- Custom retained behavior, after confirming that events, gestures, animation, lifecycle, or other built-ins do not fit: [node-extensions.md](references/node-extensions.md)
- Built-in themes, system appearance, token or component-style customization, indication, animation, layers, and scene transitions: [theme-animation-presentation.md](references/theme-animation-presentation.md)
- Fonts, text scaling, attributed text, links, selection, text input, validation, virtual semantics, and accessibility: [text-input-and-semantics.md](references/text-input-and-semantics.md)
- In-UI image and SVG resources, localization and shaping, clipboard access, file/directory references, path access, pickers, copying, HTTP, and async work: [resources-files-network.md](references/resources-files-network.md)
- Navigation, browser routes, application activation, multiple windows, safe areas, window chrome, and system tray behavior: [navigation-and-window.md](references/navigation-and-window.md)
- Local operating-system notifications, authorization, native templates, scheduling, cancellation, and activation data: [local-notifications.md](references/local-notifications.md)
- Canvas, custom drawing, paint geometry, programmatic vectors, external textures, and GPU frame publication: [canvas-paint-and-images.md](references/canvas-paint-and-images.md)
- Non-visual platform services: [platform-modules.md](references/platform-modules.md)
- Embedded platform controls: [platform-views.md](references/platform-views.md)
- mcpp package projects, `import huxerui;`, module units, and `mcpp` commands: [cpp-modules-and-mcpp.md](references/cpp-modules-and-mcpp.md)
- Existing mcpp projects manually linking a prebuilt SDK: [mcpp-build.md](references/mcpp-build.md)
- Fast public API and header lookup: [api-index.md](references/api-index.md)

## Apply changes safely

Inspect the project's build target or manifest, entry point, theme, state ownership, component patterns, and relevant platform configuration before editing. Preserve its structure and selected toolchain. Do not add platforms, compatibility layers, or a new architectural abstraction unless requested.

After editing, check the items applicable to the current change:

- layout under bounded, unbounded, narrow, and expanded constraints relevant to the task;
- controlled values, stable keys, focus, keyboard, hover, pressed, disabled, and semantics;
- reduced-motion behavior for animation;
- platform-specific public contracts and unavailable-platform limits;
- HuxerUI DSL formatting from `dsl-style.md`;
- build and run results only for the current host and affected platforms that are actually available.

When an API is uncertain, inspect the active SDK header, CMake package, or CLI help instead of guessing.
