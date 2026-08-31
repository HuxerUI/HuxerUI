# SDK Packaging

Repository packaging scripts build a complete SDK for the current desktop host and include the Android and Web target artifacts consumed by generated projects.
Release CI additionally cross-builds the Android arm64-v8a host SDK for Termux with the Android NDK.
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

Every SDK must contain the CLI, host code generators, public headers, framework resources, the canonical application-development Skill under `share/huxerui/skills`, CMake package metadata, Android artifacts, and the pinned Web library.
Desktop SDKs additionally contain their host libraries; the Android host SDK consumes the packaged Android target artifacts instead of carrying a duplicate host library.
The macOS SDK must expose the macOS `HuxerUIPlatform` Clang module beside its public headers and contain `share/huxerui/platform/ios/HuxerUI.xcframework` with `ios-arm64` and `ios-arm64_x86_64-simulator` slices.
Both XCFramework slices must expose the iOS `HuxerUIPlatform` module, and Objective-C plus Swift import-and-link fixtures must pass against the packaged libraries.
macOS libraries and host tools must declare macOS 12 as their minimum deployment version, while both XCFramework slices must declare iOS 15.
Windows and Linux SDKs do not carry iOS binaries.
Install the archive into a temporary prefix and validate at least:

```bash
huxerui doctor
huxerui create app sdk_smoke --platform <host>,web,android
huxerui build <host>
```

The generated smoke project must contain `.agents/skills/huxerui-app-development/SKILL.md` copied from the installed SDK.

On macOS, also create an iOS-only smoke project and run `huxerui build ios --profile release` so the installed XCFramework participates in a complete Simulator application-core build.

CMake packaging changes require both an incremental build and a separate clean configure and build on the affected host.
Release CI produces Windows x86_64, macOS arm64 and x86_64, Linux aarch64 and x86_64, and Android arm64-v8a archives.
Linux release CI rejects binaries that require symbols newer than GLIBC 2.35, GLIBCXX 3.4.32, or CXXABI 1.3.15.
Release jobs rebuild their host tools from source before configuration instead of publishing the executables checked into the selected release ref.
Distributed Linux host tools are built in a GLIBC 2.28 environment and statically link the GNU C++ runtime independently of the newer Linux SDK-library baseline.

Release CI validates the tag, builds the archives, and uploads them to a GitHub release, creating a missing release as a draft.
It does not derive release notes from commits or publish a draft.
Before publishing, summarize the changes between the previous published tag and the current tag, paste the curated notes into the draft, and publish it manually.
Rerunning CI preserves an existing release body and draft or published state while replacing its packaged assets.

Rebuild repository host-tool packages with `scripts/build_tools.sh` or `scripts/build_tools.ps1`.
The scripts default to the current host and accept explicit platform, architecture, CMake toolchain, Android NDK, build-directory, and output-directory arguments.
Android builds resolve the NDK automatically, while other cross-platform and non-native Linux builds require an explicit CMake toolchain.
Short forms are available as `-p`, `-a`, `-t`, `-n`, `-b`, and `-o`; architecture is inferred by default and remains available only when selecting another supported architecture.
