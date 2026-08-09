# Roadmap

The current foundation includes shared state and recomposition, responsive viewport classes, local measurement and layout invalidation, retained subtree rendering, shared damage tracking, native partial redraw on macOS and Windows, custom and built-in layout, virtualized containers, retained modifiers, animation, scrolling, Tabs, selection navigation, shared accessibility semantics for controls, presentation, scrolling, destination selection, and virtual collections, Android AccessibilityNodeProvider, macOS AppKit, and Windows UI Automation bridges, application drawers, navigation stacks and predictive Back, themes, shadows, Canvas and Path drawing, typed app resources, Image, layers, controlled text editing, installable platform-specific CMake targets, CLI project generation, diagnostics, Android and iOS device discovery, platform build and launch orchestration, Android, macOS, and Windows backends, and iOS and Emscripten Web technical previews.

Runtime foundation work:

- Composite key paths
- Layout priority and intrinsic-size queries

Framework capability work:

- Composition-scoped effects with post-commit setup and cleanup semantics
- Framework string migration, plural messages, and inherited Locale text shaping
- Demand-driven PaintCommand expansion for gradients and advanced strokes
- Shape and path-based clipping modifiers
- Event capture, bubbling, and explicit pointer capture
- Saveable state, keyframe and decay animation, and overscroll effects
- Complete component, modal, collection, navigation, virtualization, and native adapter coverage for the implemented `SemanticFrame` foundation following the [Semantics and Accessibility Design](design/semantics.md)

SDK, native integration, and distribution work:

- Signed HuxerUI Android releases on Maven Central
- CLI package and native artifact collection
- Typed platform modules and generated static registration
- NativeView lifecycle, reconciliation, host composition, focus, and accessibility
- Versioned SDK distribution and signing support
- iOS archive export, distribution signing, embeddable UIView integration, and accessibility
- OHOS backend
- Web semantics and accessibility, browser integration tests, release packaging, and mobile IME validation following the [Web Platform Design](design/web.md)

The completed Runtime invalidation foundation supports retained Canvas drawing and enables page-transition and NativeView expansion.
App resources and Image follow the ownership, packaging, caching, and localization constraints in [App Resources, Images, and Localization Design](design/resources.md).
Page stacks, transition ownership, Back routing, and future URL-backed paths follow the [Navigation Design](design/navigation.md).
Accessibility proceeds from shared semantic declarations and the immutable `SemanticFrame` through component defaults before platform-specific native adapters.
SDK delivery proceeds from the installable CMake foundation through CLI workflows and module registration before NativeView modules and versioned distribution.

Detailed design constraints and delivery sequences live in [`docs/design`](design/).
