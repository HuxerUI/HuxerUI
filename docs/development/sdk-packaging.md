# SDK Packaging

Use this workflow to build an SDK archive from a HuxerUI checkout. See [Installation](../guide/installation.md) to consume an SDK or [Packaging Applications](../guide/packaging.md) to publish an application.

## Native installation and SDK assembly

`cmake/HuxerUIBuild.cmake` defines the native `HuxerUILibraries` component for top-level builds. `cmake/HuxerUISdk.cmake` assembles the [installed SDK layout](../design/sdk-cli.md#sdk-home-and-discovery) from the native install output, headers, CMake metadata, host tools, resources, and platform artifacts. Profiling must be disabled for installation. Published archives use `HUXERUI_OPTIMIZATION=default` and `HUXERUI_ENABLE_LTO=OFF`. See [Runtime profiling](building.md#runtime-profiling) for source-build diagnostics.

Desktop archive scripts assemble the SDK for their host and include the Android, HarmonyOS, and Web artifacts. The macOS archive also includes the device and Simulator iOS XCFramework. Release CI additionally builds an Android arm64-v8a host SDK for Termux, reusing the separately assembled Android target libraries.

### Android artifacts

The package scripts and release CI run `:HuxerUI:assembleRelease -PhuxeruiBuildNative=false` (or its Debug equivalent) and verify that the resulting AAR is Java-only. For each ABI, they build the root CMake project with the static framework and testing library enabled, then install the `HuxerUILibraries` component into the ABI directory. Target-only installation works with the CLI disabled. The `:ui_testing` module supplies a test consumer.

The Android NDK version, minimum API, STL, and ABI settings come from `platform/android/gradle.properties`. Set `ANDROID_HOME` or `ANDROID_SDK_ROOT` to the SDK containing that NDK.

## Requirements

Install the following tools before running the SDK packager.

- the host compiler and platform SDK;
- Android SDK, NDK, Java, and the repository Gradle wrapper requirements;
- the Emscripten version pinned by `HUXERUI_WEB_EMSCRIPTEN_VERSION` in `cmake/HuxerUISdk.cmake`;
- Xcode when packaging on macOS;
- CMake, Ninja for Android, and the host generator used by the script.

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

## HarmonyOS artifacts

The release toolchain uses DevEco Command Line Tools `26.0.0.865`, HarmonyOS NDK `26.0.0.105`, and [OpenHarmony Clang `19.1.4` (`79830f`) with its matching NDK libc++](https://repo.huaweicloud.com/openharmony/compiler/clang/19.1.4-79830f/windows/). To assemble the Windows bundle, replace `sdk/default/openharmony/native/llvm` under `command-line-tools` with the Clang distribution. Copy `__config_site`, `libc++.a`, and `libc++experimental.a` from the matching NDK libc++ distribution into their corresponding paths under `native/llvm` for both target ABIs.

Configure the [HarmonyOS build environment](../../platform/ohos/README.md#requirements) on Windows. The SDK packager builds the HAR and static libraries for `arm64-v8a` and `x86_64`. To build those artifacts separately, run the following command.

```powershell
.\scripts\package_sdk.ps1 -OhosArtifactsOnly -BuildDirectory .\build\sdk
```

Use a separate build directory for each HarmonyOS toolchain, or clear the corresponding CMake cache under `build/sdk/ohos` before selecting a different toolchain.

Copy `build/sdk/platform-artifacts/ohos` from the same source revision to a directory outside the Unix packager's build staging directory, then set `HUXERUI_OHOS_ARTIFACTS` to the copied directory before running `package_sdk.sh`. Release CI builds these inputs once on Windows and includes them in all desktop archives.

Set the GitHub Actions repository variable `HUXERUI_OHOS_CLT_URL` to the Windows toolchain ZIP URL. The ZIP contains `command-line-tools` at its root. Release CI downloads and extracts this bundle before building the HarmonyOS SDK artifacts.

## Validate an archive

Verify its checksum and compare the extracted contents with the [SDK layout and platform artifacts](../design/sdk-cli.md#platform-release-forms). On macOS, verify the AppKit `HuxerUIPlatform` module and both iOS XCFramework slices. Check that the packaged libraries pass Objective-C and Swift import-and-link fixtures. macOS libraries and host tools must declare macOS 12 as their minimum deployment version, and both XCFramework slices must declare iOS 15.

Install the archive into an isolated temporary prefix. Verify SDK discovery with `HUXERUI_HOME` pointing to that prefix and again with the variable unset while invoking the CLI from the installation. From a scratch directory on a desktop host, build a generated consumer against the archive.

```bash
huxerui doctor
huxerui create app sdk_smoke
cd sdk_smoke
huxerui build
```

The generated smoke project must contain `.agents/skills/huxerui-app-development/SKILL.md` copied from the installed SDK.

`HuxerUISdkUpdateTests` uses local SDK fixtures and a stub release transport to check version comparisons, repeated switches in the same shell, default-version markers, profile preservation, checksum rejection, unsafe links, locking, and publication failure recovery.
These tests must not query public releases or replace a developer's SDK.

On macOS, also create an iOS-only smoke project and run `huxerui build ios --config release` so the installed XCFramework participates in a complete Simulator application-core build.

For HarmonyOS package verification, build a CMake application against the installed desktop SDK with `huxerui package ohos --abi arm64,x64`. Verify that the HAP contains one application shared library per ABI and that the signed HAP installs and launches on an arm64 device.

CMake packaging changes require both an incremental build and a separate clean configure and build on the affected host.

## Publish a release

The `v<version>` tag must match the CMake project version. Release CI validates the complete archive and checksum set before uploading assets.
Release CI produces Windows x86_64, macOS arm64 and x86_64, Linux aarch64 and x86_64, and Android arm64-v8a archives.
Linux release CI rejects binaries that require symbols newer than GLIBC 2.38, GLIBCXX 3.4.32, or CXXABI 1.3.15.
Release jobs rebuild their host tools from source before configuration instead of publishing the executables checked into the selected release ref.
Distributed Linux host tools are built in a GLIBC 2.28 environment and statically link the GNU C++ runtime independently of the newer Linux SDK-library baseline.

Release CI validates the tag, builds the archives, and uploads them to a GitHub release, creating a missing release as a draft.
It does not derive release notes from commits or publish a draft.
Before publishing, summarize the changes between the previous published tag and the current tag, paste the curated notes into the draft, and publish it manually.
Rerunning CI preserves an existing release body and draft or published state while replacing its packaged assets.

## Host-tool updates

The `Update Host Tools` workflow maintains the checked-in `hcg` and `hrc` packages independently of SDK releases.
Pushes to `main` are filtered by relevant paths before starting the workflow; a matching push runs a change-selection job to distinguish rebuilding with writeback from validation only.
Tool sources and build configuration under `tools/codegen/` and `tools/resource_compiler/` (excluding Markdown), `src/resources/resource_format.h`, `src/resources/vector_format.h`, `scripts/build_tools.sh`, `scripts/build_tools.ps1`, the workflow itself, and its `host_tools.py` helper trigger rebuilding and writeback.
Workflow-support tests, shell build-script tests, and platform validation scripts trigger validation only, without updating prebuilts.
Framework regression tests are not run by this workflow and do not select tool builds.
Ordinary framework changes, CLI changes, documentation, and generated prebuilts are excluded by the workflow's path filter.
`platform/android/gradle.properties` is not inspected or watched; this workflow pins its Android NDK version explicitly.

Use **Actions > Update Host Tools > Run workflow** on `main` to rebuild manually.
Leave `update_prebuilts` enabled to publish validated tools, or disable it to build and validate without repository writes.
Commit-message markers, tags, and scheduled runs are not used.
GitHub's [path-filter diff limits](https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax#git-diff-comparisons) can skip relevant paths in unusually large changes, so use the manual entry point if a large update did not start a run.
When GitHub bypasses path filtering for a very large commit batch or a diff timeout, the change-selection job still skips unrelated changes.

Configure the repository Actions secret `HOST_TOOLS_PUSH_TOKEN` before enabling writeback.
Use a fine-grained personal access token restricted to this repository with **Contents: Read and write**, owned by an account explicitly permitted to push through the `main` ruleset; any organization approval requirements also apply.
The workflow does not change branch protection, grant bypass rights, or inherit the triggering administrator's permissions.
Only the publication job receives this credential; missing credentials or rejected pushes fail explicitly, and validation-only runs need no write token.
Rotate the credential before it expires.

All six packages are built from the triggering commit into fresh output directories using the shared build scripts.
Desktop jobs execute the new tools against code-generation, resource-compilation, and resource-merge smoke inputs without configuring or building the framework.
Linux retains the GLIBC 2.28 and static GNU C++ runtime checks, and macOS retains the macOS 12 deployment check.
Android tools are cross-compiled and checked for their ELF architecture, Android interpreter, and absence of an unpackaged shared C++ runtime; they are not executed on the Linux runner.
Archives record the source commit and SHA-256 checksums, remain available as Actions artifacts for seven days, and must form a complete matching set before publication.

Publication creates a single `build(tools): refresh prebuilt host tools` commit containing only the twelve executables, with the source commit recorded in its body.
Unix executable modes are restored explicitly; unchanged files do not produce an empty commit.
The publisher preserves unrelated commits that arrive during the build and retries a racing fast-forward push up to three times without rebasing or force-pushing.
If tool build inputs changed or another update already replaced prebuilts since the source commit, it skips the obsolete artifacts; newer source changes start their own build, while failed runs can be retried manually.
Automatic binary-only commits do not start another workflow run under normal path filtering.
Until a successful update completes, `main` can temporarily contain newer tool sources than prebuilts; SDK release jobs still rebuild from source and do not depend on this asynchronous update.

For local development, rebuild a native package with `scripts/build_tools.sh` or `scripts/build_tools.ps1`.
The scripts default to the current host and accept explicit platform, architecture, CMake toolchain, Android NDK, build-directory, and output-directory arguments.
Android builds resolve the NDK automatically, while other cross-platform and non-native Linux builds require an explicit CMake toolchain.
Short forms are available as `-p`, `-a`, `-t`, `-n`, `-b`, and `-o`; architecture is inferred by default and remains available only when selecting another supported architecture.
