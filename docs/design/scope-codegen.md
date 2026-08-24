# Scope Code Generation Design

Status: initial implementation

This document defines an opt-in CMake integration that transforms functions marked with `[[huxerui::scope]]` into the existing explicit HuxerUI scope form. The marker declares an independent local state and recomposition boundary. The transformation is build-time syntax sugar and does not introduce a new runtime scope, state model, or recomposition path.

[View Composition and Environment Design](view-composition.md) proposes a coordinated breaking migration from this marker to `[[huxerui::composable]]` after the Environment-independent ViewSpec foundation. The current implementation and this document remain authoritative until that migration is implemented; the proposed marker must not be treated as available API yet.

## Goals

- Let functions declare a local scope with ordinary C++ function-body syntax.
- Preserve the current `Scope`, `Composer`, and `RecomposeScope` semantics.
- Require an explicit per-target CMake opt-in.
- Keep the initial transformer independent of Clang and compiler versions.
- Preserve useful source locations in compiler diagnostics.
- Fail the build when a marked function cannot be transformed safely.

## User-facing API

Applications enable local scope code generation after creating a target:

```cmake
add_executable(my_app
    main.cpp
    counter.cpp
)

target_link_libraries(my_app PRIVATE HuxerUI::huxerui)
huxerui_enable_codegen(my_app)
```

A component can then be written as a regular function:

```cpp
[[huxerui::scope]]
View Counter(int initial) {
  auto count = UseState(initial);

  return Column{
      Text(count),
      Button("+1").OnClick([count] {
        ++count;
      }),
  };
}
```

The marker applies to a function definition, not to calls of that function. Calling `Counter()` produces a `Scope` View with the same behavior as the explicit macro form. Ordinary View-returning functions remain unmarked. The application root already owns an implicit root scope and should not use this marker.

## Generated form

The transformer removes the marker and wraps the original function body with the existing scope macros:

```cpp
View Counter(int initial) {
  HUXERUI_SCOPE_BEGIN
  auto count = UseState(initial);

  return Column{
      Text(count),
      Button("+1").OnClick([count] {
        ++count;
      }),
  };
  HUXERUI_SCOPE_END
}
```

The transformation is semantically equivalent to:

```cpp
View Counter(int initial) {
  return ::huxerui::Scope([=]() -> ::huxerui::View {
    auto count = UseState(initial);

    return Column{
        Text(count),
        Button("+1").OnClick([count] {
          ++count;
        }),
    };
  });
}
```

All returns in the original body therefore return from the deferred scope factory. Parameters and `this` follow the capture behavior of the existing `HUXERUI_SCOPE_BEGIN` macro.

## CMake integration

`huxerui_enable_codegen(target)` operates on an existing target. It must:

- Reject a name that does not identify a build target.
- Inspect C++ source files already attached to the target.
- Create one generated source for every source that contains a scope marker.
- Leave sources without a marker unchanged.
- Compile generated sources instead of their marked originals.
- Mark generated sources with the CMake `GENERATED` property.
- Add dependencies on both the original source and the transformer executable.
- Generate files under a target-specific directory in the binary tree.
- Preserve the original source directory for quoted include lookup.
- Support repeated CMake configuration without adding duplicate generated sources.
- Reject repeated activation for the same target with incompatible options.

A representative output layout is:

```text
<binary-dir>/hcg/<target>/<source-path-hash>/<source-file-name>
```

Combining a hash of the absolute input path with the original source basename prevents equal basenames in different directories from colliding.

The integration is opt-in per application target. HuxerUI library sources are not transformed merely because the application links `HuxerUI::huxerui`. Codegen-enabled targets suppress the compiler warning for unknown C++ attributes so editors that consume the CMake compilation database accept the scope marker in original sources. Unsupported header definitions remain outside the initial transformation contract.

## Initial transformer

The first implementation uses two layers:

- Exact marker matching locates `[[huxerui::scope]]`.
- A lightweight C++ lexical scanner locates and matches the marked function body.

Regular expressions may locate the marker, but must not determine the closing brace of a function body. Nested blocks, lambdas, aggregate initialization, comments, and string contents make brace matching with a regular expression unsafe.

The scanner needs the following lexical states:

```text
normal source
line comment
block comment
string literal
character literal
raw string literal
```

Only braces encountered in normal source affect brace depth. Escaped characters, raw-string delimiters, and line continuations must be handled without interpreting their contents as C++ structure.

For every marker, the transformer:

- Confirms that the marker is followed by a function definition.
- Finds the opening brace of the function body.
- Finds its matching closing brace using lexical brace depth.
- Records both insertion offsets.
- Removes the marker.
- Inserts `HUXERUI_SCOPE_BEGIN` after the opening brace.
- Inserts `HUXERUI_SCOPE_END` before the matching closing brace.

Edits are applied from the end of the source toward the beginning so earlier source offsets remain valid when a file contains multiple marked functions.

## Source locations

Generated sources should use `#line` directives around inserted text and original source regions:

```cpp
#line 24 "/project/src/counter.cpp"
View Counter(int initial) {
  HUXERUI_SCOPE_BEGIN
#line 25 "/project/src/counter.cpp"
  auto count = UseState(initial);
  return Text(count);
  HUXERUI_SCOPE_END
}
```

Diagnostics for user-authored expressions should point to the original file and line whenever possible. Diagnostics originating in generated wrapper code may point to a generated location that clearly identifies the scope transformer.

The generated file must include the same public headers as the original source. The transformer does not inject `<huxerui/huxerui.h>` implicitly; missing HuxerUI declarations remain ordinary compiler errors in user code.

## Validation and diagnostics

Finding a marker without a transformable function definition is a hard build error. The transformer must not silently remove or ignore a marker.

Diagnostics should include:

- Original file path.
- Marker line and column.
- A concise reason the function is unsupported or malformed.
- The relevant first-version restriction when one applies.

Representative errors include:

```text
counter.cpp:18:1: scope marker must precede a function definition
counter.cpp:31:1: scope-marked function definitions in headers are not supported
counter.cpp:46:1: unable to match the scope-marked function body
```

The generated source is retained after a failure that occurs during C++ compilation so developers can inspect the transformation.

## Initial restrictions

The first version supports scope-marked definitions in `.cpp`, `.cc`, and `.cxx` files. It supports ordinary free functions and non-template member functions whose bodies can be located without preprocessing their syntax.

The first version does not support:

- Scope-marked definitions in headers.
- Function templates.
- Scope-marked coroutine functions.
- Scope-marked `constexpr` or `consteval` functions.
- Functions generated by macros.
- A marker generated by another macro.
- Function bodies whose brace structure depends on conditional compilation.
- Syntax between the marker and body that the lightweight scanner cannot classify safely.

Unsupported input must produce a transformer error. These restrictions can be relaxed independently without changing the user-facing marker or CMake API.

## Capture and lifetime semantics

Generated components use the existing `[=]` scope capture. The code generator does not invent separate capture rules.

This means:

- Referenced value parameters are copied into the deferred scope factory.
- A referenced `this` is captured as a pointer under C++20 rules.
- Reference parameters can outlive their referent and require care.
- Move-only values cannot be captured when the resulting scope factory must be stored in the current copyable `std::function<View()>`.

The transformer may add targeted diagnostics for unsupported captures later. The first version documents these constraints and otherwise relies on normal C++ compilation of the generated wrapper.

## Interaction with explicit scopes

Explicit scope macros remain supported:

```cpp
View Counter() {
  HUXERUI_SCOPE_BEGIN
  auto count = UseState(0);
  return Text(count);
  HUXERUI_SCOPE_END
}
```

Unmarked functions are never transformed. A marked function that already contains a top-level explicit HuxerUI scope should be rejected to prevent an accidental double scope boundary.

`Scope(factory)` also remains available as the lower-level API when custom capture behavior is required.

## Build and incremental behavior

The generated source content should be deterministic for identical input and transformer versions. The custom command should avoid rewriting an unchanged output so incremental builds do not recompile unnecessarily.

The transformer executable version participates in the generated output dependency. Updating the transformer regenerates affected target sources.

The CMake integration should expose generated files to IDE generators while keeping original files visible as non-compiled project sources. Developers edit original files only.

## Testing

Transformer tests should cover:

- One marked function.
- Multiple marked functions in one source.
- Unmarked functions between marked functions.
- Nested blocks and lambdas.
- Braces inside normal and raw strings.
- Braces inside line and block comments.
- Multiple return statements.
- Member functions and overloaded functions.
- Empty component bodies.
- Malformed and unmatched bodies.
- Marked declarations without definitions.
- Unsupported header and template definitions.
- Stable output across repeated transformations.
- Accurate `#line` mappings for a deliberate compilation error.

CMake integration tests should cover:

- Enabling transformation on an executable and a library target.
- A target containing both transformed and untouched sources.
- Equal basenames from different source directories.
- Reconfiguration without duplicate sources.
- Incremental rebuild after changing one marked source.
- Failure when the requested target does not exist.
- Failure when the same target is enabled incompatibly.

Runtime tests should verify that generated components have the same state isolation, dependency tracking, local recomposition, key behavior, and lazy state restoration as their explicit-scope equivalents.

## Future evolution

The lightweight scanner is an intentional first version, not a commitment to parse all future C++ syntax. If real usage requires header definitions, templates, complex constraints, or macro-aware transformation, the implementation can move to a Clang-based frontend while preserving:

```cpp
[[huxerui::scope]]
```

and:

```cmake
huxerui_enable_codegen(target)
```

The public component syntax and runtime model do not depend on which transformer implementation is used.
