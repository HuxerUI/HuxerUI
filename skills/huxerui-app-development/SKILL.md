---
name: huxerui-app-development
description: Develop, review, and troubleshoot HuxerUI apps and app-side libraries using the public SDK or mcpp package.
---

# HuxerUI App Development

Use this skill for consumer projects. For framework development, follow the framework repository's own instructions.

## Identify the build

Use the selected SDK or resolved package revision as the authority for public APIs and build behavior. For an explicit source dependency, use that checkout's public contracts. Inspect the existing entry point, target, state ownership, and affected platform configuration before editing.

- Use the [CMake and CLI workflow](references/project-workflow.md) for SDK discovery, scaffolding, ABI selection, packaging, and windowless UI tests.
- Use the [mcpp package workflow](references/cpp-modules-and-mcpp.md) for projects with `huxerui.huxerui` and `huxerui.rules`. Read `mcpp.toml` and `build.mcpp` to identify the integration.
- Use the [manual SDK workflow](references/mcpp-build.md) for existing mcpp projects that link SDK binaries directly.

Keep the project's build system and compatible toolchain. Report build and run results for the host and affected platforms actually validated.

## Shared rules

- Compose ordinary functions returning `View` by value. Mark reusable functions that call composition-bound hooks or need their own recomposition scope with `[[huxerui::composable]]`. Leave the application root unannotated.
- Keep controlled values in app state, including the complete `TextEditingValue` for `TextField`. Use stable semantic keys for dynamic stateful siblings. Read `State<T>` through conversion or `state->member` where supported. See [State](references/fundamentals.md#state) for deduction and overload cases.
- Follow [DSL style](references/dsl-style.md) in UI declarations and snippets. Use built-in layouts, themes, events, modifiers, and services before extending them. Prefer generated `ImageResource` values for static artwork and SVG for scalable in-UI assets.
- Use `platform` for platform implementations, windows, objects, capabilities, and bridges. Use `system` for operating-system scheduling, tasks, requests, and assigned identifiers. Omit qualifiers when the context suffices. Preserve JNI/NDK, Java `native`, native handles to C++ objects, native libraries, and fixed external names.
- Confirm public renames before changing them, then migrate callers, tests, examples, documentation, and affected bindings or exports together. Preserve threading, ownership, and lifetime. Omit unused callback parameter names.

## Read by task

- For component and state work, read [composition, state, and tasks](references/fundamentals.md), [built-in controls](references/components.md), or [layout and virtualization](references/layout-and-ui.md).
- For interaction, read [modifiers and mounted View targets](references/modifiers.md), [gestures and drag-and-drop](references/gestures-and-drag-drop.md), or [retained node extensions](references/node-extensions.md).
- For appearance and rendering, read [theme, animation, and presentation](references/theme-animation-presentation.md), [text editing and accessibility](references/text-input-and-semantics.md), or [Canvas and external textures](references/canvas-paint-and-images.md).
- For app services, read [navigation and windows](references/navigation-and-window.md), [resources, files, and network](references/resources-files-network.md), or [local notifications](references/local-notifications.md).
- For platform integration, read [platform modules](references/platform-modules.md) for services and [platform views](references/platform-views.md) for embedded controls.
- Use the [public API index](references/api-index.md) to find headers and focused references.
