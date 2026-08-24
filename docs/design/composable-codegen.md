# Composable Code Generation Design

Status: implemented

This document defines the opt-in CMake integration that transforms functions marked with `[[huxerui::composable]]` into the existing explicit HuxerUI scope form.
The marker declares an independent local state and recomposition boundary.
The transformation is build-time syntax sugar and does not introduce a new runtime scope, state model, or recomposition path.

## Goals

- Let functions declare a composable scope with ordinary C++ function-body syntax.
- Reject direct composition-bound `UseXxx()` calls from unmarked functions.
- Preserve the current `Scope`, `Composer`, and `RecomposeScope` semantics.
- Require an explicit per-target CMake opt-in.
- Keep the initial transformer independent of Clang and compiler versions.
- Preserve useful source locations in compiler diagnostics.
- Fail the build when a marked function cannot be transformed safely.

## User-facing API

Applications enable composable code generation after creating a target:

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
[[huxerui::composable]]
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
- Defer that inspection until the current CMake source directory has finished so later `target_sources()` calls participate in the same pass.
- Create one generated source for every source that contains a composable marker or a possible `UseXxx()` call.
- Leave sources without a marker or possible composition call unchanged.
- Compile generated sources instead of their marked originals.
- Mark generated sources with the CMake `GENERATED` property.
- Add dependencies on both the original source and the transformer executable.
- Generate files under a target-specific directory in the binary tree.
- Preserve the original source directory for quoted include lookup.
- Support repeated CMake configuration without adding duplicate generated sources.

A representative output layout is:

```text
<binary-dir>/hcg/<target>/<source-path-hash>/<source-file-name>
```

Combining a hash of the absolute input path with the original source basename prevents equal basenames in different directories from colliding.

The integration is opt-in per target.
HuxerUI application and library helpers enable it for their source targets, while linking `HuxerUI::huxerui` alone does not transform consumer sources.
Codegen-enabled targets suppress the compiler warning for unknown C++ attributes so editors that consume the CMake compilation database accept the composable marker in original sources.
Unsupported header definitions remain outside the initial transformation contract.

## Initial transformer

The first implementation uses two layers:

- Exact marker matching locates `[[huxerui::composable]]`.
- A lightweight C++ lexical scanner locates and matches the marked function body.
- The same scanner rejects direct namespace or unqualified calls named `UseXxx()` outside composable bodies.

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

The composition-call check is intentionally lexical rather than a C++ call-graph analysis.
It recognizes direct `UseXxx()` and `namespace::UseXxx()` calls, ignores member calls such as `object.UseValue()`, and skips comments, literals, and preprocessor directives.
This naming rule also covers third-party composition facilities without maintaining a framework-owned list.
The application root is exempt when its function is registered by an `Application` object in the same translation unit because Runtime already executes it inside the implicit root scope.
A root registered from another translation unit should remain Environment-independent and delegate composition-bound work to a composable function.
A custom composition hook named `UseXxx()` is also exempt inside its own body.
Hooks may return non-View values and deliberately share the caller's active composition scope, so wrapping them in a View-producing composable Scope would be incorrect.
Calling such a hook still requires a composable caller, another hook, or the Application root.
Aliases, function pointers, macro-generated calls, and indirect wrappers without the `UseXxx` naming convention are outside the guarantee of this lightweight check.

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

Diagnostics for user-authored expressions should point to the original file and line whenever possible. Diagnostics originating in generated wrapper code may point to a generated location that clearly identifies the composable transformer.

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
counter.cpp:18:1: composable marker must precede a function definition
counter.cpp:46:1: unable to match the composable function body
counter.cpp:52:16: composition function UseState() must be called from a [[huxerui::composable]] function
```

The generated source is retained after a failure that occurs during C++ compilation so developers can inspect the transformation.

## Initial restrictions

The CMake integration scans and transforms composable definitions in `.cpp`, `.cc`, and `.cxx` files.
Headers are not scanned and must not contain composable definitions.
It supports ordinary free functions and non-template member functions whose bodies can be located without preprocessing their syntax.

The first version does not support:

- Function templates.
- Composable coroutine functions.
- Composable `constexpr` or `consteval` functions.
- Functions generated by macros.
- A marker generated by another macro.
- Function bodies whose brace structure depends on conditional compilation.
- Syntax between the marker and body that the lightweight scanner cannot classify safely.

Unsupported syntax detected in a processed source is a transformer error; semantic restrictions that remain valid lexical input are diagnosed by the C++ compiler.
These restrictions can be relaxed independently without changing the user-facing marker or CMake API.

## Capture and lifetime semantics

Generated components use the existing `[=]` scope capture. The code generator does not invent separate capture rules.

This means:

- Referenced value parameters are copied into the deferred scope factory.
- Captures are immutable because the generated lambda is not `mutable`; ordinary move constructors therefore do not consume a captured value across recompositions.
- A referenced `this` is captured as a pointer under C++20 rules.
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

Unmarked function bodies are never wrapped in a generated scope, although a source containing a possible `UseXxx()` call still passes through lexical validation.
A marked function that already contains a top-level explicit HuxerUI scope is rejected to prevent an accidental double scope boundary.

An inline `Scope` factory lambda also remains available as the lower-level API when custom capture behavior is required, and direct composition calls inside that lambda are valid.

## Build and incremental behavior

The generated source content should be deterministic for identical input and transformer versions. The custom command should avoid rewriting an unchanged output so incremental builds do not recompile unnecessarily.

The transformer executable version participates in the generated output dependency. Updating the transformer regenerates affected target sources.

Build tools compile the generated path, while `#line` mappings keep diagnostics attached to the original source that developers edit.

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
- Unsupported template definitions.
- Stable output across repeated transformations.
- Accurate `#line` mappings for a deliberate compilation error.

CMake integration tests should cover:

- Enabling transformation on an executable and a library target.
- A target containing both transformed and untouched sources.
- Equal basenames from different source directories.
- Reconfiguration without duplicate sources.
- Incremental rebuild after changing one marked source.
- Failure when the requested target does not exist.

Runtime tests should verify that generated components have the same state isolation, dependency tracking, local recomposition, key behavior, and lazy state restoration as their explicit-scope equivalents.

## Future evolution

The lightweight scanner is an intentional first version, not a commitment to parse all future C++ syntax. If real usage requires header definitions, templates, complex constraints, or macro-aware transformation, the implementation can move to a Clang-based frontend while preserving:

```cpp
[[huxerui::composable]]
```

and:

```cmake
huxerui_enable_codegen(target)
```

The public component syntax and runtime model do not depend on which transformer implementation is used.
