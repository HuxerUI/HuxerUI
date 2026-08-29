# Design Documents

These documents define internal ownership, invariants, platform boundaries, and extension contracts.
They explain the current implementation and deliberately unsupported behavior; they are not tutorials or a release roadmap.

## Runtime and composition

- [Architecture](architecture.md): Runtime, mounted nodes, modifiers, Environment, layers, and extension ownership.
- [View Composition and Environment](view-composition.md): declaration, compilation, reconciliation, and composition boundaries.
- [Composable Code Generation](composable-codegen.md): marker transformation, diagnostics, and CMake integration.
- [Incremental Layout and Rendering](incremental-rendering.md): retained geometry, invalidation, scene construction, and damage.
- [Interaction and Indication](interaction-indication.md): interaction facts, visuals, paint ordering, and customization.
- [Gesture Recognition and Arbitration](gestures.md): recognition, competition, ownership, velocity, and cancellation.
- [Typed Drag-and-Drop](drag-drop.md): typed payloads, targets, previews, auto-scroll, and cancellation.
- [Animation and Scene Transitions](animation.md): timing, controllers, synchronized transitions, and frozen scenes.

## Application structure

- [Application Activation and Lifecycle](application.md): cold start, later activation, lifecycle, and platform delivery.
- [Navigation](navigation.md): stacks, typed routes, Back, URL history, and activation integration.
- [Indexed Pages](indexed-pages.md): retained peer pages and controlled selection.
- [Window Insets and System Bars](window-insets.md): safe area, edge-to-edge content, and system-bar appearance.
- [Window Chrome](window-chrome.md): system and custom desktop chrome.
- [System Tray and Window Visibility](system-tray.md): tray presentation, window request handling, visibility, and application termination.
- [Task and Structured Concurrency](tasks.md): task ownership, cancellation, and UI-thread resumption.

## Content and services

- [Text and Fonts](text.md): styles, measurement, runs, and platform caches.
- [Text Input and TextField](text-input.md): editing values, sessions, geometry, IME, and secure input.
- [Select](select.md): controlled finite choices, popup ownership, item identity, focus, and semantics.
- [Canvas and Path](canvas.md): vector paths, drawing, retention, and renderer mapping.
- [Resources, Images, and Localization](resources.md): typed resources, packages, variants, and locale.
- [Semantics and Accessibility](semantics.md): semantic frames, actions, identity, and platform bridges.
- [Files and Application Storage](files.md): storage, external references, pickers, and platform mapping.
- [HTTP Client](http.md): requests, cancellation, transport ownership, and errors.

## SDK and platforms

- [SDK, CLI, Platform Shell, and Library](sdk-cli.md): installed SDK structure, project tooling, libraries, and platform integration.
- [Web Platform](web.md): browser lifecycle, rendering, input, resources, services, and integration limits.

Deferred behavior remains in the owning design document only when its boundary affects current architecture.
Cross-subsystem capability direction is summarized in the [Roadmap](../roadmap.md), while product scheduling and release plans belong in GitHub issues.
