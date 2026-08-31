# HuxerUI Host Tools

Host tools are distributed by operating system and architecture:

```text
prebuilt/<windows|macos|linux|android>/<architecture>/<hcg|hrc>[.exe]
```

These executables run on the build host. Their platform and architecture are independent of the application target and Android ABI. CMake selects the matching executable automatically and stops configuration when that host package is unavailable.

Current host tools are:

- HuxerUI Code Generator (`hcg`) for `[[huxerui::composable]]` transformation and direct composition-call validation
- HuxerUI Resource Compiler (`hrc`) for typed keys with default or explicitly named generated headers, resource indexes, ordered package merging, and package staging

Each distributed host and architecture directory must contain every tool required by the project configuration.

Prebuilt executables must be rebuilt from the matching tool source whenever that source changes. Tests compile the tool sources directly and therefore do not prove that a distributed executable is current.
The checked-in packages bootstrap source builds, while SDK release jobs rebuild the matching package from source before using or distributing it.

Linux x86_64 and aarch64 tools require no GLIBC symbol newer than 2.28 and statically link the GNU C++ runtime.
Linux uses `aarch64` for 64-bit Arm packages and directories, Apple platforms use `arm64`, and Android uses the ABI name `arm64-v8a`.
Release CI applies that stricter tool contract through `.github/workflows/scripts/check_linux_binary_compatibility.sh` so a newer build host cannot silently raise their glibc baseline or add a GLIBCXX runtime requirement.

Rebuild a native package or an explicitly selected cross-platform package through the shared scripts:

```bash
sh scripts/build_tools.sh -p android
```

```powershell
.\scripts\build_tools.ps1 -p android
```

Android tool builds resolve the NDK from `ANDROID_NDK_HOME`, `ANDROID_NDK_ROOT`, or `ANDROID_HOME`, and accept an explicit `--android-ndk` or `-AndroidNdk` path.
They target Android API 24 and produce native Bionic executables for `arm64-v8a` hosts.
All platform builds accept an explicit CMake toolchain and output directory.
Selecting another operating system, or a non-native Linux architecture, requires that toolchain instead of relabeling native output.
Architecture is inferred from the selected platform and current CPU, with Android fixed to `arm64-v8a` and Windows fixed to `x86_64`.
It remains explicitly selectable for separate macOS arm64 and x86_64 packages and cross-architecture Linux builds.

## macOS distribution

Release binaries must be signed with a Developer ID Application certificate, packaged for distribution, and submitted to Apple's notarization service.
Use a distribution format supported by Apple's current notarization and ticket-stapling workflow when an offline-verifiable release is required.

For a trusted local checkout whose downloaded tools were quarantined by an archive utility, remove that attribute explicitly:

```bash
xattr -dr com.apple.quarantine tools/prebuilt/macos
```

This local operation does not replace release signing or notarization and must not run implicitly during CMake configuration.
