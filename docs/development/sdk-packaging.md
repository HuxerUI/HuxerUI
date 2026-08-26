# SDK Packaging

Repository packaging scripts build a complete SDK for the current desktop host and include the Android and Web target artifacts consumed by generated projects.
They are release and integration tools, not prerequisites for ordinary source builds.

## Requirements

Packaging requires:

- the host compiler and platform SDK;
- Android SDK, NDK, Java, and the repository Gradle wrapper requirements;
- Emscripten 4.0.19;
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

The SDK must contain the CLI, host code generators, public headers, framework resources, CMake package metadata, host libraries, Android artifacts, and the pinned Web library.
Install the archive into a temporary prefix and validate at least:

```bash
huxerui doctor
huxerui create app sdk_smoke --platform <host>,web,android
huxerui build <host>
```

CMake packaging changes require both an incremental build and a separate clean configure and build on the affected host.
Release CI produces Windows x86_64, macOS arm64 and x86_64, and Linux aarch64 and x86_64 archives.
