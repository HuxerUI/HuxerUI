# SDK Packaging

Repository packaging scripts build a complete SDK for the current desktop host and include the Android and Web target artifacts consumed by generated projects.
The macOS SDK additionally contains the static iOS XCFramework for device and Simulator application-core builds.
They are release and integration tools, not prerequisites for ordinary source builds.

## Requirements

Packaging requires:

- the host compiler and platform SDK;
- Android SDK, NDK, Java, and the repository Gradle wrapper requirements;
- Emscripten 4.0.19;
- Xcode when packaging on macOS;
- CMake and the generator used by the script.

The scripts validate required artifacts and fail instead of publishing a partial SDK.

## Windows

```powershell
.\scripts\package_sdk.ps1
```

Optional directories:

```powershell
.\scripts\package_sdk.ps1 -BuildDirectory .\build\sdk -OutputDirectory .\build\sdk\packages
```

## macOS and Linux

```bash
sh scripts/package_sdk.sh
```

Optional directories:

```bash
sh scripts/package_sdk.sh --build-dir ./build/sdk --output-dir ./build/sdk/packages
```

Packaging output belongs in a build directory and must not be written into the source tree.

## Validate an archive

Every SDK must contain the CLI, host code generators, public headers, framework resources, CMake package metadata, host libraries, Android artifacts, and the pinned Web library.
The macOS SDK must also contain `share/huxerui/platform/ios/HuxerUI.xcframework` with `ios-arm64` and `ios-arm64_x86_64-simulator` slices; Windows and Linux SDKs do not carry iOS binaries.
Install the archive into a temporary prefix and validate at least:

```bash
huxerui doctor
huxerui create app sdk_smoke --platform <host>,web,android
huxerui build <host>
```

On macOS, also create an iOS-only smoke project and run `huxerui build ios --profile release` so the installed XCFramework participates in a complete Simulator application-core build.

CMake packaging changes require both an incremental build and a separate clean configure and build on the affected host.
Release CI produces Windows x86_64, macOS arm64 and x86_64, and Linux aarch64 and x86_64 archives.
