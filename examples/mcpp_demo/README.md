# HuxerUI mcpp demo

A HuxerUI application built natively by mcpp. It is deliberately outside the
CMake example list: `examples/CMakeLists.txt` does not add it, so it exercises
the mcpp leg and nothing else.

```bash
mcpp build
mcpp run
```

Nothing needs to be installed first: mcpp provisions the GTK 4 stack from the
xlings payloads HuxerUI declares, and builds the framework itself as part of
this build. No CMake step, no `apt-get`.

## What this demonstrates

The whole manifest is:

```toml
[dependencies]
huxerui = { path = "../.." }
```

and the whole build program is:

```cpp
import mcpp;
import huxerui.rules;
int main() { return huxerui::rules::configure({ .resources = "resources" }) ? 0 : 1; }
```

The previous revision of this demo spelled out an SDK include directory, the
HuxerUI static library, twenty absolute `/lib64/*.so` paths and a runtime
`library_dirs` entry, and its README recorded that "stateful composables and
packaged resources need an additional mcpp integration layer".

`huxerui.rules` is that layer, so the page now uses a `[[huxerui::composable]]`
function with `UseState` and ships a resource package. Both are scheduled as
build-graph edges rather than done in the build program, so they are
incremental, parallel and attributable to the file that failed.

## It consumes HuxerUI as a C++20 module

`src/main.cpp` opens with `import huxerui;`, not `#include <huxerui/huxerui.h>`,
and **nothing else about the code changes** -- same names, same DSL, same
`[[huxerui::composable]]`. `modules/huxerui.cppm` includes the public headers in
its global module fragment and re-exports what they declare, so both spellings
name the same entities with the same linkage against the same library.

This works only because `hcg` injects the *expansion* of `HUXERUI_SCOPE_BEGIN` /
`HUXERUI_SCOPE_END` rather than the macro names: macros do not cross a module
boundary, so generated code naming them would not compile here. The macros
remain public API for hand-written code.

GTK reaches the link line without this manifest naming it: a dependency's
`build.mcpp` emits `link-lib` / `link-search` that reach the **final** link, and
HuxerUI's resolves the GTK stack through pkg-config -- the same `.pc` files
`cmake/platform/Linux.cmake` reads.

## Scope

Linux, Windows and macOS. Android, iOS and Web are CMake-only: mcpp's target
table has no rows for them, and `modules/toolchain-model/src/triple.cppm`
declines `androideabi` and `wasi` as outside its target language. See
`.agents/docs/2026-09-09-mcpp-native-build-and-modules-plan.md`.
