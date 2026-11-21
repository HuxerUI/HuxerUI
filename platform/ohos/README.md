# HarmonyOS

Open this directory in DevEco Studio to build the [`huxerui` host module](huxerui/) and the [`example_runner` application](example_runner/). The host module supplies the `UIAbility` lifecycle, surface component, and binding declarations. The runner targets HarmonyOS API 19 and defaults to `arm64-v8a` and the [Counter example](../../examples/counter/).

`huxerui_add_app` compiles the framework's Node-API registration with the application's output name. Hvigor packages the selected example and the statically linked HuxerUI framework in `libhuxerui_example_runner.so`.

The runner stages the generated HuxerUI resource package under the HAP's `rawfile` resources before resource compilation. The host reads it through the resource manager and supplies the application data, cache, and temporary directories from `UIAbilityContext`.

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

Text measurement and replay use Native Drawing typography. Application-supplied fonts use private font collections, and attributed inline objects use paragraph placeholders. Line limits and ellipsis use the shared overflow implementation. `TextLayout` exposes line, caret, hit-test, and range geometry in logical units with UTF-16 offsets. When the API 19 drawing engine groups multiple graphemes into a glyph, it estimates the interior caret positions and retains the shaped line width. Editable clients use system keyboard sessions with selection, preedit, and action keys. Secure editors suppress surrounding text.

The renderer supports solid and gradient fills and strokes, rounded rectangle and path shadows, clips, transforms, group opacity, and raster images. Image Kit decodes PNG and JPEG assets and encodes bitmaps to PNG or JPEG. Use `ohos::PixelTexture` for live software frames. On API 22 or later, `ohos::NativeBufferTexture` imports completed RGBA8 buffers into the drawing backend. Check `IsSupported()` before selecting the buffer path.

Register an ArkUI node factory with `ohos::PlatformViewFactory` from `<huxerui/ohos/platform_registry.h>`. The runner's `platform_view` example embeds an ArkUI TextInput and draws HuxerUI controls above it. The factory owns the ArkUI node. HuxerUI manages attachment, rectangular clipping, scene ordering, and focus routing.

The clipboard publishes plain text, HTML, images as system PixelMaps, and custom MIME bytes as representations of one item. File lists use ordered system file URI records and return read-only file references. The system grants access to the receiving application. Reading requires [HarmonyOS pasteboard authorization](https://developer.huawei.com/consumer/en/doc/harmonyos-guides/get-pastedata-permission-guidelines/).

The system document picker selects files and exports local files. A picked file supports in-place writing when its provider grants write access. Check `FilePicker::CanOpenDirectories()` for directory selection, which requires a device and system version that expose [folder selection](https://developer.huawei.com/consumer/en/doc/HarmonyOS-Guides/select-user-file/). Selected directories retain an open descriptor for access to their children. Picked items have no local path projection.

To remember read access across launches, declare `ohos.permission.FILE_ACCESS_PERSIST` and use a device and provider that support [persistent file authorization](https://developer.huawei.com/consumer/en/doc/HarmonyOS-Guides/file-persistpermission/). `FileDropTarget` receives ordinary files as read-only references through ArkUI drag events.

HTTP uses Remote Communication Kit. Declare `ohos.permission.INTERNET` in the application module. Buffered requests work on API 19. Streaming relies on the separate response status callback introduced in API 20 and reports `Unsupported` on earlier versions.

Runtime permission requests use the application's declared permissions. Local notifications support immediate and scheduled delivery, cancellation, and activation with the submitted payload. For scheduling, declare `ohos.permission.PUBLISH_AGENT_REMINDER` and configure [agent-powered reminders](https://developer.huawei.com/consumer/en/doc/HarmonyOS-Guides/agent-powered-reminder/) for the application. Continued tasks use the process-local allowance. System background-task admission is pending.

Hardware keyboard and mouse input use the shared HuxerUI key, pointer, and scroll events. Text editing also uses the system input method. Register [links](https://developer.huawei.com/consumer/en/doc/HarmonyOS-Guides/app-linking-startup/) and [document types](https://developer.huawei.com/consumer/en/doc/HarmonyOS-Guides/file-processing-apps-startup/) in the ability's `skills` to receive `UrlActivation` and `FileActivation`.
