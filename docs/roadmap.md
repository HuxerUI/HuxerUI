# Roadmap

The current foundation includes shared state and recomposition, local measurement and layout invalidation, retained subtree rendering, shared damage tracking, native partial redraw on macOS and Windows, custom and built-in layout, virtualized containers, retained modifiers, animation, scrolling, themes, layers, controlled text editing, and Android, macOS, and Windows backends.

Runtime foundation work:

- Composite key paths
- Layout priority and intrinsic-size queries

Framework capability work:

- Composition-scoped effects with post-commit setup and cleanup semantics
- Canvas, box shadows, and demand-driven PaintCommand expansion
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
- iOS, OHOS, Linux, and Web backends

The completed Runtime invalidation foundation enables rendering-heavy Canvas, page-transition, and NativeView expansion.
SDK delivery proceeds from the installable CMake foundation through CLI workflows and module registration before NativeView modules and versioned distribution.

Detailed design constraints and delivery sequences live in [`docs/design`](design/).

