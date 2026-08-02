# Roadmap

The current foundation includes shared state and recomposition, local measurement and layout invalidation, retained subtree rendering, shared damage tracking, native partial redraw on macOS, Windows, and Linux, custom and built-in layout, virtualized containers, retained modifiers, animation, scrolling, themes, shadows, Canvas and Path drawing, typed app resources, Image, layers, controlled text editing, and Android, Linux, macOS, and Windows backends.

Runtime foundation work:

- Composite key paths
- Layout priority and intrinsic-size queries

Framework capability work:

- Composition-scoped effects with post-commit setup and cleanup semantics
- Framework string migration, plural messages, and inherited Locale text shaping
- Demand-driven PaintCommand expansion for gradients and advanced strokes
- Navigation stacks, scoped navigation controllers, platform back handling, and page transitions
- General-purpose clipping modifiers
- Event capture, bubbling, and explicit pointer capture
- Saveable state, keyframe and decay animation, and overscroll effects
- Semantics tree and accessibility

SDK, native integration, and distribution work:

- Installed CMake package, host code-generator resolution, and external consumer validation
- SDK and CLI project creation, build, run, package, and diagnostics
- Typed platform modules and generated static registration
- NativeView lifecycle, reconciliation, host composition, focus, and accessibility
- Versioned SDK distribution and signing support
- iOS, OHOS, and Web backends

The completed Runtime invalidation foundation supports retained Canvas drawing and enables page-transition and NativeView expansion.
App resources and Image follow the ownership, packaging, caching, and localization constraints in [App Resources, Images, and Localization Design](design/resources.md).
SDK delivery proceeds from the installable CMake foundation through CLI workflows and module registration before NativeView modules and versioned distribution.

Detailed design constraints and delivery sequences live in [`docs/design`](design/).

