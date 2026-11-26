# HarmonyOS

HuxerUI applications require HarmonyOS 6.0.0 (API 20) or later. CMake builds the C++ application, and Hvigor assembles the HAP. The desktop HuxerUI SDK provides the [`huxerui` ArkTS host](huxerui/) as a HAR and includes static framework libraries for `arm64-v8a` and `x86_64`. Each application links the framework and its Node-API entry into its own shared library.

## Requirements

Use CMake and DevEco Command Line Tools with the HarmonyOS 26.0.0 Release NDK (`26.0.0.105`), OpenHarmony Clang `19.1.4` (`79830f`), and its matching NDK libc++. The C++ standard library must provide `<source_location>` and `<stop_token>`. See [HarmonyOS SDK packaging](../../docs/development/sdk-packaging.md#harmonyos-artifacts) for toolchain preparation.

Set `DEVECO_SDK_HOME` to the DevEco SDK directory and `OHOS_NATIVE_SDK` to the NDK's `native` directory. Add `ohpm`, `hvigorw`, and `hdc` to `PATH`. Use an `OHOS_NATIVE_SDK` path without spaces so Hvigor can pass `CMAKE_TOOLCHAIN_FILE` to CMake.

For direct CMake builds, use `$OHOS_NATIVE_SDK/build/cmake/ohos.toolchain.cmake` with `-DOHOS_ARCH=arm64-v8a` or `-DOHOS_ARCH=x86_64`, `-DOHOS_STL=c++_static`, and `-DOHOS_COMPATIBLE_SDK_VERSION=20.0.0`. The HuxerUI targets enable the required libc++ features.

## Building the example runner

The [`example_runner` module](example_runner/) builds repository examples. It defaults to `arm64-v8a` and the [Counter example](../../examples/counter/). Run `ohpm install --all` from `platform/ohos` to install its dependencies. Use `runExample` from the same directory to build, install, and launch a selected example.

```sh
hvigorw --mode module -p product=default -p module=example_runner@default -p huxeruiExample=ui_gallery runExample --no-daemon
```

Use the directory name under `examples/` for `huxeruiExample`. Add `-p device=<id>` to select a target from `hdc list targets`. Use `assembleHap` in place of `runExample` to build the HAP without installing it. For the Windows DevEco emulator, set `HUXERUI_OHOS_ABIS=x86_64` in the environment before running the command.

For installation on a physical device, use DevEco Studio to obtain a debug certificate and provisioning profile for `org.huxerui.examples`. Include the device in the provisioning profile. Save the corresponding DevEco `signingConfigs` entry as `.huxerui/signing.json` under `platform/ohos`. The runner loads this Git-ignored configuration for the `default` product.

```json
{
  "name": "default",
  "type": "HarmonyOS",
  "material": {
    "certpath": "D:/signing/debug.cer",
    "storeFile": "D:/signing/debug.p12",
    "profile": "D:/signing/debug.p7b",
    "storePassword": "<value from DevEco Studio>",
    "keyAlias": "<value from DevEco Studio>",
    "keyPassword": "<value from DevEco Studio>",
    "signAlg": "SHA256withECDSA"
  }
}
```

To build the ArkTS host independently, run `hvigorw --mode module -p product=default -p module=huxerui@default -p buildMode=release assembleHar --no-daemon` from `platform/ohos`. The output is `huxerui/build/default/outputs/default/huxerui.har`.

## Using an installed SDK

Install the [desktop HuxerUI SDK](../../docs/guide/installation.md) and configure the HarmonyOS toolchain. The CLI generates a CMake application with a Hvigor project under `platform/ohos` and stages the SDK's HAR before each build.

```sh
huxerui create app my_app --platform ohos --build-system cmake
cd my_app
huxerui doctor ohos
huxerui build ohos
huxerui package ohos --abi arm64,x64
```

The package command writes a release HAP under `dist/ohos`. The generated project defaults to `arm64-v8a`. When running without an explicit `--abi`, the CLI selects `x86_64` for a detected local emulator.

Configure signing for the application's bundle name in DevEco Studio before installing on a physical device. Use `huxerui devices ohos` to list destinations and `huxerui run ohos` to build, install, and launch the application.

After switching HuxerUI SDKs, clean the Hvigor build mode you intend to rebuild. For a release build, run `hvigorw --mode module -p product=default -p module=entry@default -p buildMode=release clean --no-daemon` from the generated project's `platform/ohos` directory.

## Integrating an existing Hvigor project

Use the desktop SDK's `share/huxerui/platform/ohos/huxerui.har` as the application's `@huxerui/huxerui` dependency. With `HUXERUI_HOME` set to the same SDK, resolve its CMake package and declare the application target.

```cmake
find_package(HuxerUI CONFIG REQUIRED PATHS "$ENV{HUXERUI_HOME}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
huxerui_add_app(my_app SOURCES main.cpp)
```

`huxerui_add_app()` links the SDK archive and the Node-API registration into `libmy_app.so`. Import this library from the entry ability and return it from `HuxerUIAbility.getApplicationModule()`. Set the `HUXERUI_OHOS_RESOURCE_OUTPUT_ROOT` environment variable for the CMake build and stage `<resource-root>/<abi>/package` under the module's `rawfile` resources before Hvigor processes them. The [example runner](example_runner/hvigorfile.ts) shows the task dependencies for resource staging. Configure application signing and run `assembleHap` for the entry module.

## Platform capabilities

The main window supports `SafeArea` and `EdgeToEdge` content, drawing behind the system bars in immersive layout. `SystemBarsAppearance` controls the status-bar icons, and HarmonyOS controls the bottom gesture indicator. `UseSystemThemeBrightness()` follows system light and dark mode changes.

`OpenWindowAsync()` creates child windows in the running ability's `WindowStage`. Each loads the same host page and composes its own C++ root in the shared Runtime. Pass the page's `getUIContext()` to `handleHuxerUIBack()` so the back action goes to the originating window. Run with `-p huxeruiExample=multi_window` to open or cancel a child window, share a counter, and edit text independently in each window.

Text measurement and replay use Native Drawing typography. Application-supplied fonts use private font collections, and attributed inline objects use paragraph placeholders. Line limits and ellipsis use the shared overflow implementation. `TextLayout` exposes line, caret, hit-test, and range geometry in logical units with UTF-16 offsets. When the drawing engine groups multiple graphemes into a glyph, HuxerUI estimates the interior caret positions and retains the shaped line width. Editable clients use system keyboard sessions with selection, preedit, and action keys. Secure editors suppress surrounding text.

Turn on the system screen reader to navigate and activate HuxerUI controls using their shared semantics. The `platform_view` example demonstrates an accessible ArkUI editor in the same window as HuxerUI controls.

The renderer supports solid and gradient fills and strokes, rounded rectangle and path shadows, clips, transforms, group opacity, and raster images. Image Kit decodes PNG and JPEG assets and encodes bitmaps to PNG or JPEG. Use `ohos::PixelTexture` for live software frames. On API 22 or later, `ohos::NativeBufferTexture` imports completed RGBA8 buffers into the drawing backend. `ohos::GlTexture` accepts existing OpenGL ES textures when `GlTexture::IsSupportedCurrent()` reports support for the API 23 EGL buffer import path.

The `platform_module` example runs a window-scoped ArkTS timer with typed results, events, and cancellation. The `platform_view` example embeds an ArkTS TextInput with controlled text and draws HuxerUI controls above it. The runner supplies both [ArkTS factories](example_runner/src/main/ets/entryability/ExampleExtensions.ets). HuxerUI manages attachment, rectangular clipping, scene ordering, and focus routing. See [HarmonyOS platform extensions](../../docs/guide/platforms.md#harmonyos-platform-extensions) for the application contract.

The clipboard publishes plain text, HTML, images as system PixelMaps, and custom MIME bytes as representations of one item. File lists use ordered system file URI records and return read-only file references. The system grants access to the receiving application. Reading requires [HarmonyOS pasteboard authorization](https://developer.huawei.com/consumer/en/doc/harmonyos-guides/get-pastedata-permission-guidelines/).

The system document picker selects files and exports local files. A picked file supports in-place writing when its provider grants write access. Check `FilePicker::CanOpenDirectories()` for directory selection, which requires a device and system version that expose [folder selection](https://developer.huawei.com/consumer/en/doc/HarmonyOS-Guides/select-user-file/). Selected directories retain an open descriptor for access to their children. Picked items have no local path projection.

To remember read access across launches, declare `ohos.permission.FILE_ACCESS_PERSIST` and use a device and provider that support [persistent file authorization](https://developer.huawei.com/consumer/en/doc/HarmonyOS-Guides/file-persistpermission/). `FileDropTarget` receives ordinary files as read-only references through ArkUI drag events.

HTTP uses Remote Communication Kit for buffered and streaming requests, including server-sent events. Declare `ohos.permission.INTERNET` in the application module.

Runtime permission requests use the application's declared permissions. Local notifications support immediate and scheduled delivery, cancellation, and activation with the submitted payload. For scheduling, declare `ohos.permission.PUBLISH_AGENT_REMINDER` and configure [agent-powered reminders](https://developer.huawei.com/consumer/en/doc/HarmonyOS-Guides/agent-powered-reminder/) for the application.

`BackgroundTask` uses a WorkScheduler extension. `ContinuedTask` and `OngoingTask` use configured continuous background modes on the entry ability. See [HarmonyOS system tasks](../../docs/guide/system-tasks.md#harmonyos) for application setup.

Hardware keyboard and mouse input use the shared HuxerUI key, pointer, and scroll events. Text editing also uses the system input method. Register [links](https://developer.huawei.com/consumer/en/doc/HarmonyOS-Guides/app-linking-startup/) and [document types](https://developer.huawei.com/consumer/en/doc/HarmonyOS-Guides/file-processing-apps-startup/) in the ability's `skills` to receive `UrlActivation` and `FileActivation`.
