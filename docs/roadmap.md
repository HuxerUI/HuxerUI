# Roadmap

The current foundation includes shared state and recomposition, custom and built-in layout, virtualized containers, retained modifiers, animation, scrolling, themes, layers, controlled text editing, and Android, macOS, and Windows backends.

Near-term framework work:

- Local measurement, layout, and paint invalidation
- Composite key paths
- Layout priority and intrinsic-size queries
- General-purpose clipping modifiers
- Event capture, bubbling, and explicit pointer capture
- Saveable state, keyframe and decay animation, and overscroll effects
- Semantics tree and accessibility

Platform and distribution work:

- iOS, OHOS, Linux, and Web backends
- Installed CMake package and host code-generator resolution
- SDK and CLI project creation, build, run, package, and diagnostics
- Typed platform modules and generated static registration
- NativeView lifecycle and reconciliation
- Versioned SDK distribution and signing support

Detailed design constraints and delivery sequences live in [`docs/design`](design/).

