# Roadmap

The current foundation includes shared state and recomposition, responsive viewport classes, local measurement and layout invalidation, retained subtree rendering, shared damage tracking, native partial redraw on macOS and Windows, custom and built-in layout, virtualized containers, retained modifiers, animation, scrolling, Tabs, themes, shadows, Canvas and Path drawing, typed app resources, Image, layers, controlled text editing, installable platform-specific CMake targets, CLI project generation, diagnostics, Android and iOS device discovery, platform build and launch orchestration, Android, macOS, and Windows backends, and iOS and Emscripten Web technical previews.

Runtime foundation work:

- Composite key paths
- Layout priority and intrinsic-size queries

Framework capability work:

- Composition-scoped effects with post-commit setup and cleanup semantics
- Framework string migration, plural messages, and inherited Locale text shaping
- Demand-driven PaintCommand expansion for gradients and advanced strokes
- Navigation stacks, scoped navigation controllers, navigation-aware predictive Back, and page transitions
- Shape and path-based clipping modifiers
- Event capture, bubbling, and explicit pointer capture
- Saveable state, keyframe and decay animation, and overscroll effects
- Semantics tree and accessibility

SDK, native integration, and distribution work:

- Signed HuxerUI Android releases on Maven Central
- CLI package and native artifact collection
- Typed platform modules and generated static registration
- NativeView lifecycle, reconciliation, host composition, focus, and accessibility
- Versioned SDK distribution and signing support
- iOS archive export, distribution signing, embeddable UIView integration, and accessibility
- OHOS and Linux backends
- Web semantics and accessibility, browser integration tests, release packaging, and mobile IME validation following the [Web Platform Design](design/web.md)

The completed Runtime invalidation foundation supports retained Canvas drawing and enables page-transition and NativeView expansion.
App resources and Image follow the ownership, packaging, caching, and localization constraints in [App Resources, Images, and Localization Design](design/resources.md).
SDK delivery proceeds from the installable CMake foundation through CLI workflows and module registration before NativeView modules and versioned distribution.

Detailed design constraints and delivery sequences live in [`docs/design`](design/).
