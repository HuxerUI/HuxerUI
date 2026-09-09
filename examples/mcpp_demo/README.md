# HuxerUI mcpp demo

This directory is intentionally outside the CMake example list. It demonstrates the independent `huxerui mcpp build` frontend by compiling a small HuxerUI desktop page from an `mcpp.toml` manifest.

Build the HuxerUI static library first from the repository root:

```bash
cmake -S . -B build -G Ninja \
  -DHUXERUI_BUILD_EXAMPLES=OFF \
  -DHUXERUI_BUILD_TESTS=OFF \
  -DHUXERUI_BUILD_CLI=ON \
  -DHUXERUI_ENABLE_PROFILING=OFF
cmake --build build --target huxerui_static --parallel
```

Then build this demo through the SDK CLI:

```bash
build/bin/huxerui mcpp build --source examples/mcpp_demo
```

The generated executable is placed in mcpp's `target/` directory. Run it with mcpp from this directory:

```bash
cd examples/mcpp_demo
mcpp run
```

The manifest links the locally built Linux HuxerUI static library and its GTK dependencies. This is a host-specific proof of the build delegation path; packaging and cross-platform integration are not part of this demo.

The demo currently requests C++26 through mcpp and uses the GCC 16.1.0 toolchain. The same page was also verified with `standard = "c++23"`; both standards compile successfully.

The page intentionally uses stateless public components only, so it does not require HuxerUI's `hcg` code generator or `hrc` resource compiler. Stateful composables and packaged resources need an additional mcpp integration layer.
