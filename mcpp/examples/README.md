# mcpp examples

Three applications, built by `mcpp build` in their own directory. Each one is
about `import` and module units rather than about HuxerUI's widgets — the
framework's feature examples live in [`../../examples`](../../examples) and are
built by CMake.

| | What it shows |
|---|---|
| [`01-import`](01-import/) | The smallest form: `import huxerui;`, one composable, **no headers anywhere** |
| [`02-module-units`](02-module-units/) | One application across four module units, a scope written by hand without the macro, and a Windows MSI |
| [`03-library`](03-library/) | `import` crossing a package boundary: a library package and the application that consumes it |

```bash
cd 01-import && mcpp build && mcpp run
```

Each depends on the framework by path (`huxerui = { path = "../../.." }`), so
they build against the checkout they live in rather than a release.

## What they have in common

Every manifest is short because the dependency edge carries the rest. `huxerui`
re-exports the build rules, the composable transform (`hcg`) and the resource
compiler (`hrc`), so an application's `build.mcpp` is one call:

```cpp
import mcpp;
import huxerui.rules;

int main() { return huxerui::rules::configure({}) ? 0 : 1; }
```

`[build] sources = []` in each manifest is not an oversight. `huxerui.rules`
owns the source selection, and a build program can *add* a source but not
replace one — a package that also globbed `src/**/*.cpp` would link both the
original and the transformed copy of every composable.

## The header question

`import std;` is what makes them header-free. `UseState()`, `View` and
`Layout` instantiate `typeid` in their **caller**, and GCC checks that per
translation unit, so `std::type_info` has to be visible wherever a View is
built. A CMake project gets it from `#include <huxerui/huxerui.h>`; an importer
cannot, because a global module fragment's includes do not reach whoever
imports it. Importing `std` answers it without a header.

Each example pins `standard = "c++23"` for that, and it is a workaround:
C++20 is enough for every implementation here, but mcpp's clang-on-Windows path
hardcodes a c++23 floor for `import std;` rather than probing the STL it found
([mcpp#603](https://github.com/mcpp-community/mcpp/issues/603)).

There is no exception. `02-module-units/src/banner.cppm` writes a scope by hand
without the `HUXERUI_SCOPE` macro, because the macro expands to
`return Scope([=]() -> View { … })` and `Scope` and `View` are both exported —
the macro was only hiding them. Macros do not cross a module boundary, and
under modules nothing needs one to: `hcg` injects that same expansion for a
`[[huxerui::composable]]` function, which is what a real unit would write.
