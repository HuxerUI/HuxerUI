# HarmonyOS

Open this directory in DevEco Studio to build the [`huxerui` host module](huxerui/) and the [`example_runner` application](example_runner/). The host module supplies the `UIAbility` lifecycle, surface component, and binding declarations. The runner targets HarmonyOS API 19 and defaults to `arm64-v8a` and the [Counter example](../../examples/counter/).

`huxerui_add_app` compiles the framework's Node-API registration with the application's output name. Hvigor packages the selected example and the statically linked HuxerUI framework in `libhuxerui_example_runner.so`.

Use the HarmonyOS command-line tools with an OHOS C++ toolchain whose standard library supplies `<source_location>` and `<stop_token>`. The LLVM 15.0.4 standard library supplied by some HarmonyOS SDKs lacks both headers. If using a separate toolchain, select it locally with `-DCMAKE_TOOLCHAIN_FILE=<path>` in the entry module's `externalNativeOptions.arguments`.

Install dependencies and build the runner from this directory.

```sh
ohpm install --all
hvigorw --mode module -p product=default -p module=example_runner@default assembleHap --no-daemon
```

To build another example, add `-DHUXERUI_EXAMPLE=dynamic_list` to `externalNativeOptions.arguments` in `example_runner/build-profile.json5`. Use the directory name under `examples/`. The example must use capabilities available on the current backend.

Build the ArkTS host as a standalone HAR with `hvigorw --mode module -p product=default -p module=huxerui@default assembleHar --no-daemon`. An application can reference the resulting `huxerui.har` in its `oh-package.json5` and build its application library through CMake.

For device testing, use DevEco Studio to configure debug signing for `org.huxerui.examples` and register the test device in its provisioning profile.

For the Windows DevEco emulator, select `x86_64` locally in `externalNativeOptions.abiFilters` and use an x86_64 OHOS toolchain. Install the unsigned HAP with `hdc app install <path-to-hap>`.

Text measurement and replay use Native Drawing typography. Line limits and ellipsis use the shared overflow implementation. `TextLayout` exposes line, caret, hit-test, and range geometry in logical units with UTF-16 offsets. When the API 19 drawing engine groups multiple graphemes into a glyph, it estimates the interior caret positions and retains the shaped line width. Text-input sessions, inline objects, and application-supplied fonts are pending. The renderer supports solid fills and strokes, clips, transforms, and group opacity. Gradients, shadows, raster images, external textures, and platform views are pending.
