# mcpp examples

Three applications, built by `mcpp build` in their own directory. Each one is
about `import` and module units rather than about HuxerUI's widgets — the
framework's feature examples live in [`../../examples`](../../examples) and are
built by CMake.

| | What it shows |
|---|---|
| [`01-import`](01-import/) | The smallest form: `import huxerui;`, one composable, **no headers anywhere** |
| [`02-module-units`](02-module-units/) | One application across four module units, and the one case that still needs a global module fragment |
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

`import std;` is what makes 01 and 03 header-free. `UseState()`, `View` and
`Layout` instantiate `typeid` in their **caller**, and GCC checks that per
translation unit, so `std::type_info` has to be visible wherever a View is
built. A CMake project gets it from `#include <huxerui/huxerui.h>`; an importer
cannot, because a global module fragment's includes do not reach whoever
imports it. Importing `std` answers it without a header.

`02-module-units/src/banner.cppm` is the exception, and it is worth reading for
what it costs: a **hand-written** `HUXERUI_SCOPE(...)` needs the macro, macros
do not cross a module boundary, and an include needs a global module fragment.
Marking the function `[[huxerui::composable]]` instead needs none of that —
`hcg` injects the macro's expansion, so generated code is macro-free.
