# Roadmap

The current foundation includes shared state and recomposition, composition-scoped Lifecycle and structured TaskScope concurrency, responsive viewport classes, local measurement and layout invalidation, retained subtree rendering, shared damage tracking, native partial redraw on Windows and macOS, custom and built-in layout, virtualized containers, retained modifiers, animation, scrolling, Tabs, selection navigation, shared accessibility semantics for controls, presentation, scrolling, destination selection, and virtual collections, Windows UI Automation, macOS AppKit, Android AccessibilityNodeProvider, and iOS UIKit bridges, application drawers, navigation stacks and predictive Back, themes, shadows, Canvas and Path drawing, typed app resources, Image, layers, controlled text editing, typed nonvisual platform modules on Windows, macOS, Linux, Web, Android, and iOS, installable platform-specific CMake targets, CLI project generation, diagnostics, Android and iOS device discovery, and Windows, macOS, Linux, Web, Android, and iOS platform build, launch, and backend integration.

Runtime foundation work:

- Composite key paths
- Layout priority and intrinsic-size queries

Framework capability work:

- Plural messages and inherited Locale text shaping for ordinary text content
- Demand-driven PaintCommand expansion for gradients and advanced strokes
- Shape and path-based clipping modifiers
- Event capture, bubbling, and explicit pointer capture
- Saveable state, decay animation, ordinary View lifecycle transitions, and overscroll effects
- Complete component, modal, collection, navigation, virtualization, and platform adapter coverage for the implemented `SemanticFrame` foundation following the [Semantics and Accessibility Design](design/semantics.md)

SDK, platform integration, and distribution work:

- Signed HuxerUI Android releases on Maven Central
- CLI package and platform artifact collection
- Typed platform modules with explicit application-installed RootHooks, `PlatformPayload` calls, results, events, and platform dependency projection
- PlatformView hosting on Linux, Web accessibility attachment, and remaining cross-platform integration coverage
- Versioned SDK distribution and signing support
- iOS archive export, distribution signing, and embeddable UIView integration
- OHOS backend
- Web semantics and accessibility, browser integration tests, release packaging, and mobile IME validation following the [Web Platform Design](design/web.md)

The completed Runtime invalidation foundation supports retained Canvas drawing and enables page-transition and PlatformView expansion.
App resources and Image follow the ownership, packaging, caching, and localization constraints in [App Resources, Images, and Localization Design](design/resources.md).
Page stacks, transition ownership, Back routing, and future URL-backed paths follow the [Navigation Design](design/navigation.md).
Retained timing, synchronized presentation properties, and explicit frozen-scene transitions follow the [Animation and Scene Transition Design](design/animation.md).
Accessibility proceeds from shared semantic declarations and the immutable `SemanticFrame` through component defaults before platform adapters.
SDK delivery proceeds from the installable CMake foundation through CLI workflows and module registration before PlatformView modules and versioned distribution.

Detailed design constraints and delivery sequences live in [`docs/design`](design/).
