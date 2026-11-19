# SDK, CLI, Platform Shell, and Library Design

## Ownership

CMake owns the common C++ targets, dependency graph, code generation, and resources. Each enabled platform builds from the application's root `CMakeLists.txt`. Source-controlled platform shells own lifecycle, platform configuration, signing, and distribution metadata.

The CLI creates project and shell sources, resolves the SDK and host tools, and coordinates the platform build and launch commands. Its generated metadata and build caches live under `.huxerui`. The repository's [mcpp integration](mcpp-build-system.md) owns its own project and build model.

The installed SDK combines the CLI, host tools, public CMake helpers, framework artifacts, and built-in resources under one release version. SDK installation and version selection are separate from external toolchain setup.

For commands and project workflows, see [Getting Started](../guide/getting-started.md), [Installation](../guide/installation.md), and [Packaging Applications](../guide/packaging.md).

## Application project

The generated `CMakeLists.txt` explicitly discovers sources and declares application targets, libraries, and resources. Its sibling `HuxerUIProject.cmake` supplies SDK discovery, platform-shell configuration, and the CLI's project plan. Library and preview roots use the same separation.

The common sources, resources, and platform shells are application source. Creation publishes the generated project only after all files succeed. `platform add` creates missing shells or platform library packages, preserves existing trees, and rolls back directories created by a failed multi-platform operation. Each shell owns its ignore rules for platform intermediates.

Editable templates live under `tools/huxerui_cli/templates` and are embedded in the CLI with CMakeRC. One project identity drives file contents and output paths. The SDK installs its application-development Skill under `share/huxerui/skills`, and `create` copies it into the selected agent directories so the project owns its copy.

A shared static `Application` declaration supplies the root and options. Platform entry points call `RunApplication()` after static initialization or attach a host to the registered application. See [application and window ownership](architecture.md#top-level-window-ownership-and-dispatch).

## SDK selection and distribution

Each release's CLI, CMake metadata, target artifacts, and resource package come from the same HuxerUI version. The host executables run on the development host independently of the application target.

### SDK home and discovery

`HUXERUI_HOME` is the public environment variable for selecting an installed SDK or an explicit source checkout.
For an installed SDK it names the self-contained installation prefix:

```text
HUXERUI_HOME/
  bin/
    huxerui[.exe]
    huxerui.dll                         # Windows Release framework runtime
    huxerui_debug.dll                   # Windows Debug framework runtime
  include/
  lib/
    huxerui.lib                         # Windows Release DLL import library
    huxerui_debug.lib                   # Windows Debug DLL import library
    huxerui_static.lib                  # Windows Release static library
    huxerui_static_debug.lib            # Windows Debug static library
    cmake/HuxerUI/
  share/huxerui/
    skills/huxerui-app-development/
    platform/
      android/
        HuxerUI.aar
        <abi>/libhuxerui_static.a
        <abi>/libhuxerui_testing.a
      ios/HuxerUI.xcframework/             # macOS SDK only
        ios-arm64/
        ios-arm64_x86_64-simulator/
      web/emscripten-<version>/libhuxerui.a
    resources/
    tools/<host>/<architecture>/
```

`HUXERUI_HOME/bin` belongs on `PATH` for a portable or installer-managed SDK. If `HUXERUI_HOME` is set, discovery uses that value and the command validates its layout. Otherwise the CLI looks for an installed SDK relative to its executable.

A supplied `--sdk <path>` takes precedence over discovery for build, run, package, open, doctor, and setup. Build and diagnostic commands may use this override for a source checkout. Without it, those commands expect an installed SDK. The CLI passes the effective `HUXERUI_HOME` to child processes. Generated CMake projects select `add_subdirectory` for source and `find_package(HuxerUI CONFIG REQUIRED)` for an installation. Both paths expose the same public targets and helpers.

CLI-managed caches are keyed by normalized framework location below `.huxerui/build/<home-key>`. Graph discovery has its own `library-graph` directory. Android passes the effective home through Gradle to `externalNativeBuild`. iOS supplies it as an Xcode build setting and keeps Simulator and device DerivedData separate. Direct IDE builds use their own environment or explicit settings.

### Platform release forms

Platform build systems consume platform artifacts through their normal mechanisms:

- Android uses a Java-only AAR plus ABI-specific static archives imported by the installed CMake package.
- Android arm64-v8a also has a host SDK for Termux containing Bionic CLI and host-tool executables.
- macOS uses its host libraries and installed `HuxerUIPlatform` Clang module, while the macOS SDK carries a static iOS XCFramework with its UIKit `HuxerUIPlatform` module for device and Simulator application-core builds.
- Windows and Linux use architecture- and toolchain-compatible CMake SDK archives.
- Web uses the static archives for the selected SDK's Emscripten version and configures the application root through `emcmake`.

The module maps describe the existing platform libraries for Objective-C and Swift consumers.

See [SDK Packaging](../development/sdk-packaging.md) for archive assembly and release verification.

### Installers

CMake install rules define the SDK layout. The platform installers verify archive checksums and layout before publishing the SDK. They stage a sibling directory, lock the installation prefix during replacement or version selection, and preserve the previous valid selection if publication fails. Uninstall removes the SDK and installer-owned environment entries. See [Installation](../guide/installation.md) for host paths, commands, and SDK switching behavior.

The CLI embeds both installer scripts. The `sdk` commands require the running CLI to belong to the selected installation and wait for the script to complete. Each candidate version contains its own CLI, target artifacts, and host tools. The installer checks that the candidate CLI reports the expected version and supports version selection before publishing its path.

SDK installers manage HuxerUI archives and environment selection. External compilers, CMake, JDKs, platform SDKs, and IDE initialization belong to [environment diagnosis and setup](#doctor-and-setup).

## CMake SDK

`HuxerUIConfig.cmake` exports the canonical framework targets and makes the application, library, resource, and code-generation helpers available. Repository-only compiler and platform configuration stays in the source tree.
Desktop packages import `HuxerUI::huxerui` and `HuxerUI::huxerui_static` by default. `huxerui_add_app` selects the static target.
Android, Web, and iOS import static framework artifacts through the canonical targets; Android applications link the framework into their own shared library.
Consumers may request only `COMPONENTS shared` or `COMPONENTS static`; requesting the Linux shared component does not resolve static-only dependencies, while the static component resolves the distribution-owned GTK 4, libepoxy, GIO, libsoup 3, pangoft2, and Fontconfig libraries through pkg-config.
Only a top-level desktop HuxerUI build registers SDK installation and CPack behavior; consuming the source tree through `add_subdirectory` creates framework targets and public helpers without adding packaging or install rules to the parent project.

`huxerui_add_app` creates the platform's application target, applies its supplied bundle metadata, links the framework, and enables composable code generation after the sources are known. It enables MSVC `/utf-8` for application sources. Shells that own resource staging can set `RESOURCE_OUTPUT_DIRECTORY`. The helper also registers the application's optional resource root and emits its artifact metadata.

Consumers that create their own targets call `huxerui_enable_codegen` and `huxerui_add_resources` explicitly. Only `huxerui_add_app` registers the built-in framework resources automatically. The [resource design](resources.md) defines package merging, namespace identity, and variant precedence.

## Generated integration

Build metadata describes the produced artifact rather than predicting it from shell settings. Desktop and Web builds use the application helper's metadata. Android reads Gradle's final APK metadata, and iOS reads the build result emitted by the Xcode phase.

### Library graph

For platform package discovery, the CLI configures the root project with `HUXERUI_LIBRARY_GRAPH_ONLY=ON` in a dedicated build directory. The CMake helpers create interface targets so the graph can be resolved without a compiler, native framework artifacts, or platform dependencies. The source and installed SDK entry points both honor this declaration-only path. Library aliases and dependency edges must be visible in both modes.

CMake traverses direct `huxerui_use_library` edges depth-first in declaration order and emits each resolved source-backed target once under the name used on its first visit. The resulting `.huxerui/generated/libraries.json` contains target names and resolved source roots. Predeclared binary targets without source roots are linked by CMake, while any platform packages they need are declared separately by the project.

The Android driver attaches the corresponding Gradle libraries during settings evaluation. The iOS driver writes a local Swift package aggregator under `.huxerui/generated/ios/libraries`, which the checked-in Xcode project references through a stable product. The native iOS CMake build separately follows the same edges to include each reachable static library once in the application-core archive. Windows, macOS, Linux, and Web resolve their platform library sources directly in CMake.

Normal native configuration and graph-only configuration use separate outputs. Build commands regenerate Android and iOS integration, and `open ios` does so before opening Xcode. Unchanged generated files retain their timestamps. Platform package dependencies, permissions, signing, sources, and product policy stay in CMake or the owning platform project.

## Platform drivers

The CLI core delegates platform identity, host compatibility, tool discovery, setup actions, shell generation, build and launch commands, artifact discovery, and device listing to its internal `PlatformDriver` interface.

Build commands validate the targets and use the host's selected toolchain. The desktop CMake path preserves an existing compatible generator and leaves parallelism to CMake and the build tool. Windows selects MSVC through the Visual Studio developer environment and uses Ninja when available, falling back to NMake. Fresh builds on Linux or macOS use Ninja when available unless the project or invocation selects another generator.

Profiling changes the source-build inputs and gets a separate configuration directory. Mobile drivers forward the setting to the native build through the Gradle property or Xcode build setting described in [Runtime profiling](../development/building.md#runtime-profiling).

### Doctor and setup

`doctor` constructs a read-only plan from the common and platform tool checks. Tool readiness and project-shell validation are reported separately. An unavailable device-discovery tool produces a failed check while the rest of the plan remains available. The `devices` operation validates discovery prerequisites independently of whether a device is connected.

The JSON plan uses schema version 1. Each `targets` entry reports readiness and lists its required check IDs in `requires`. The `checks` array provides IDs, statuses, and detected values. Each automatic or manual action in `actions` identifies prerequisite checks in `requires` and resulting checks in `provides`. Installation-only checks can be optional for target readiness. `projectErrors` carries shell validation errors. Display labels and detail text may change.

`setup` presents the same plan and runs automatic actions after confirmation. It chooses an action only when its prerequisite checks are ready. After an action succeeds, it rebuilds the plan using the same discovery and verifies the checks the action promised to satisfy before continuing. The native tools retain their own prompts and report cancellation or a required restart. The [environment guide](../guide/getting-started.md#diagnose-the-environment) describes the command arguments and exit codes.

Diagnostics, setup, device discovery, and builds share tool resolution. Child commands receive their resolved paths and environment overrides. The selected CMake version goes to the Gradle shell and its path goes to the Xcode scripts. Android respects the project's explicit `cmake.dir`. Managed downloads are verified before deployment. Managed CMake, JDK, and Emscripten installations remain discoverable in the host user data directory.

Platform drivers select the appropriate setup actions. Windows reuses a compatible Visual Studio instance or invokes the official Build Tools installer. Desktop Android uses project SDK and NDK settings with JDK 17 as the managed default. Web resolves the Emscripten version from the selected SDK and can prepare a compatible emsdk. Ubuntu 24.04 uses apt for development dependencies. Apple diagnostics and iOS platform downloads use the selected Xcode. Termux, mcpp, and signing retain their own prerequisites.

### Windows

The shell supplies the application manifest and native package inputs. The application helper selects the GUI subsystem and uses `mainCRTStartup` with MSVC. The installer UI selects `wWinMainCRTStartup` through its private target property. The public framework link interface leaves the consumer's subsystem unchanged. The driver launches the application executable directly.

### macOS

CMake creates the application bundle from the shell's Info.plist template and the actual executable name and version. Generated application, library, and preview roots set the macOS deployment default before `project()`, after considering explicit cache, environment, or toolchain selections. The driver opens the built bundle.

### Linux

The root project owns the executable, resources, and HuxerUI linkage. The driver discovers the executable through the application artifact metadata and runs it with its containing directory as the working directory. See [Linux platform support](../guide/platforms.md#linux) for host requirements and GTK PlatformView coverage.

### Web

The shell owns the generated HTML entry point, icons, and mount element. CMake synchronizes these files even when C++ compilation is up to date. Desktop hosts launch the site through `emrun`. On Windows, `emrun` opens the URL through `explorer.exe`. On Termux, the driver opens a loopback URL with `termux-open` and serves the build through Python's standard-library HTTP server.

### Android

Gradle owns the Android application shell and invokes the repository root through `externalNativeBuild`. It owns the namespace, SDK and NDK versions, ABI filters, manifest merging, signing, and APK output. The installed Java-only AAR supplies the Gradle integration, and CMake links the ABI-specific HuxerUI static library into the application's shared library.

Each native configure writes `<root>/<abi>/app.json` under the variant's `HUXERUI_ANDROID_APP_INTEGRATION_ROOT`. The Gradle shell uses AGP's public `MERGED_NATIVE_LIBS` artifact to validate that the same application library name is present for each participating ABI. The name comes from the CMake output, including `OUTPUT_NAME` or a configuration postfix. It must differ from `huxerui`. The matching plans also identify which resource output to stage.

The shell supplies the resolved library name through `BuildConfig.HUXERUI_APP_LIBRARY`. The generated MainApplication loads it before any HuxerUI Java service or Runtime is used. A custom host owns the equivalent load. Gradle registers resource staging as a variant-generated asset directory so APK assembly and lint depend on the native build and staging work.

The static framework retains and exports JNI entry points through the NDK linker, while the application owns any additional dynamic exports and library initialization. The generated shell defaults to `c++_static`. Custom hosts with interacting shared libraries can use one consistent `c++_shared` runtime through `huxeruiStl`.

The checked-in Gradle wrapper pins its distribution and checksum. A desktop host uses ADB to list devices and retain their ready, offline, and unauthorized states. Termux uses its local `arm64-v8a` build with the packaged `aapt2` and opens the APK with the system installer. Its Android SDK and NDK must be compatible with the local host.

### iOS

The source-controlled Xcode project owns process entry, platform resources, signing, and the final app bundle. Its build scripts invoke the common CMake project to produce the application-core archive, linker response file, and merged resources in `huxerui-ios/<target>`. The core contains the static `Application` declaration and the shell's `main.mm` calls `RunApplication()`.

The driver combines paired physical devices from `devicectl` and Simulators from `simctl`. It preserves CoreDevice identifiers for physical devices, uses `simctl` as the authority for simulator records, and prefers ready records among duplicate reports from the same tool. The Xcode build selects the matching Simulator or device destination. Launch uses `simctl` or `devicectl` for that destination. `open ios` refreshes the generated library aggregator before opening the checked-in project.

iOS archive export automation and public UIView embedding are outside the current SDK workflow. Repository examples use a single platform runner whose `HUXERUI_APP_TARGET` selects the application core.

### Watching development builds

`run <platform> --watch` owns one development session using the selected build, SDK, and device. The CLI tracks changes in the project and its declared local library roots, coalesces saves, and runs the existing incremental build. Changes to generated output do not trigger a new build. After a build failure, the current application keeps running and the next edit can retry.

Desktop sessions launch a copy of the built application from an independent directory, including its bundled or adjacent resources and adjacent shared libraries. A completed build is staged before the CLI stops the application it previously launched. If the replacement cannot start, the CLI relaunches the previous build with its original child environment. Web publishes versioned copies of the generated entry and its assets to the same loopback server and refreshes the open page. iOS installs each successful build and launches it using the original device selection.

For Android Debug watch sessions on desktop hosts, the current shell prepares a fingerprint of the APK outside the application library and HuxerUI resource package. On a compatible device, the first installation records that fingerprint and the installed APK path. A later matching build transfers the selected ABI library and full resource package together through ADB and `run-as`, then restarts the process. Startup loads one version's read-only library and binds its matching resource package. APK shell changes, older generated shells, and devices without private deployment support use the normal install and launch path. Incomplete transfers leave the running application alone. If the staged library cannot load, the session selects the previous valid version again.

At shutdown the CLI stops its local processes. Installed Android and iOS builds remain on the device.

## Package

The CLI publishes the completed distribution artifact under `dist/<platform>` after build, staging, and platform packaging succeed. A failed attempt preserves the previous publication. Artifact formats, ABI and signing options, and runtime-dependency declarations are described in [Packaging Applications](../guide/packaging.md).

Desktop staging uses the CMake install component recorded in `HUXERUI_APPLICATION_INSTALL_COMPONENT`. The generated install rules supply the executable and resource package. `HuxerUIRuntimeDependencies.cmake` collects non-system dependencies from those binaries and from explicitly declared runtime inputs. The root directory schedules deployment after ordinary install rules so payloads added by parent or sibling directories participate. Inputs are resolved in their declaring directories, and installation uses the final target-owned list for the selected configuration.

Discovery uses the original binaries. Copying, load-path rewriting, and validation use a fresh stage. Dependency collection and final validation apply the same system-library policy. Conflicting staged copies are checked by hash. A path outside the stage must be a recognized system dependency. Platform-specific runtime policies are described in the [desktop packaging requirements](../guide/packaging.md#desktop-system-requirements).

Linux selects the AppDir `usr` or system-package `usr/lib/<project-id>` root for that stage. CPack derives system package dependencies from the staged ELF files. The RPM hook filters private libraries' ELF capabilities from package exports and matching internal requirements.

### Windows installer

WiX Burn and MSI own the installation transaction and restart policy. The bootstrapper UI is an out-of-process HuxerUI application using the ordinary Windows Runtime and UiWindow. Its sources and localized resources live under `platform/windows/package`. The native picker and Burn retain ownership of their messages.

The package build restores and validates WiX dependencies and builds the installer UI against `HuxerUI::huxerui_static`. The application and installer use separate CMake stages. The installer stage contains its executable, resources, `mbanative.dll`, and its own non-system dependency closure. The bootstrapper executable is declared by the bundle source, and the generated WiX payload consumes the rest of the installer stage.

One root-owned installer session backs the composition-bound `InstallerHandle`. See the [Windows installer interface](../guide/packaging.md#custom-windows-installer-interface) for commands and observable status. Detection compares the related bundle version with `WixBundleVersion`. `Present` and `SameVersion` use the same repair eligibility check. A detected `NewerVersion` blocks downgrades even if the package is absent.

Burn supplies `InstallFolder` and `CreateDesktopShortcut` as the authored defaults and passes their values into MSI. The UI initializes its local drafts from the expanded values and submits one `InstallerInstallOptions` when installation starts. The MSI models the optional desktop shortcut as a separate Feature so its selection can survive major upgrades. The UI launches the installer folder picker through its `TaskScope` so the pointer event finishes before the native modal dialog starts.

User commands enter Burn from the bootstrapper UI thread. Burn callbacks publish status through the Windows UI dispatcher and retain Burn's callback thread rules for continuation. Embedded and quiet runs use a hidden top-level HWND for detection and apply. The HWND thread pumps messages until a terminal result, destroys the window, and calls Burn Quit. It retains the bootstrapper across synchronous shutdown callbacks, which cannot wait on that same thread.

Synchronous engine questions expose one identified prompt and ignore stale answers. Apply returns to Burn before the restart prompt is answered. An accepted restart is requested at shutdown. Closing during apply requests cancellation and lets rollback finish before exit. The installer session owns the transaction across component unmounts.

## Libraries and platform integration

A library is a compile-time CMake target. It can include common resources and platform packages, and its preview is an ordinary application that consumes the library through a local path. The [user guide](../guide/getting-started.md#libraries) describes dependency declarations and how to run the preview.

### Library scaffolding and preview

Library names do not require a `huxerui-` prefix and may use uppercase ASCII letters.
The scaffold preserves the supplied name for the repository directory and project display name and derives a lowercase snake-case project-local identifier for private source filenames and Preview targets.
For example, `CameraKit`, `camera-kit`, and `camera_kit` derive `camera_kit`; leading, trailing, or repeated separators are rejected.

`--namespace` owns the exact C++ namespace used by generated declarations and definitions.
It accepts one or more non-reserved ASCII C++ identifiers separated by `::` and otherwise preserves the supplied spelling.
`--target` owns the exact consumer-facing CMake target used by `huxerui_use_library`.
It accepts either one ASCII letter-and-digit segment or exactly two such segments separated by `::`, including targets such as `HuxerUI::Camera`.
The CLI does not reserve package prefixes; generated targets must still avoid actual CMake target collisions.
The default namespace is the project-local snake-case identifier.
The default public target removes project-name separators and capitalizes lowercase segment starts while preserving intentional capitalization, so `CameraKit` generates `CameraKit::CameraKit` and `HuxerUI-CameraKit` generates `HuxerUICameraKit::HuxerUICameraKit`.

The public target deterministically projects the common build identities without adding more CLI options or editable metadata.
Each target segment becomes a lowercase token without inserted separators.
An unqualified target is used directly as the implementation and public target without an alias; its lowercase token remains the resource namespace and both header tokens, so `CameraKit` produces target `CameraKit`, resource namespace `camerakit`, and primary header `<camerakit/camerakit.h>`.
A qualified target such as `Scave::Camera` produces implementation target and resource namespace `scave_camera`, primary header `<scave/camera.h>`, and alias `Scave::Camera`.
If both qualified segments normalize to the same token, as in `CameraKit::CameraKit`, the implementation target and resource namespace fold to `camerakit` rather than repeating the token.
Only qualified public targets need an alias.

`--id` is the complete stable reverse-domain project identifier rather than an organization prefix.
For an application it initializes the Android application ID and namespace, Apple bundle identifier, and equivalent platform product identifiers.
For a library it initializes the Android library namespace and the CLI's stable platform-package identity.
Maven coordinates, the C++ namespace, the CMake target, and resource namespaces remain owned by their respective platform or common project files and are not inferred by splitting `--id`.
When omitted, the scaffold uses `com.example.<normalized-name>` as an editable development default.

Project kind selects the platform artifact shape, so there are no separate Android application-versus-library or Apple application-versus-package options.
An Android application receives a Gradle application shell, while an Android library receives an independent Gradle library build.
An iOS application receives an Xcode application project, while an iOS library receives a Swift package with a library product.
Platform-specific SDK levels, dependencies, permissions, capabilities, publishing coordinates, and product policy are edited in those generated platform projects instead of being additional cross-platform CLI arguments.

Each platform selected for a library creates the matching application shell below `examples/preview`.
Windows, Linux, and macOS create CMake source roots under `platform/<platform>/src`, Android additionally creates an independent Gradle library under `platform/android`, and iOS creates a Swift package under `platform/ios/<Swift product>/`.
Library-owned iOS C++ and Objective-C++ sources can live alongside the package in `platform/ios/src` and compile into the common library target.
Web currently adds only the Preview shell because no separate platform-package shape has been defined for it.
`platform add` applies the same behavior after creation, fills whichever Library package or Preview shell is missing, and leaves an existing counterpart untouched.
The generated library's CMake planning output records the exact namespace and public target alongside its project identity.
`platform add` reads that planning output and recomputes the header, implementation target, and resource namespace through the same deterministic projection used during creation.

### Acquisition and resources

Remote acquisition uses FetchContent's build-directory cache. A tag resolves in the repository's exact `refs/tags/` namespace. FetchContent performs Git discovery when it needs a clone or update. Repeated acquisitions of the same CMake target from the same path or pinned URL share the result. A conflicting origin fails configuration.

The helper verifies the acquired target, records a direct dependency edge, and links it with the requested scope. Resource assembly follows those edges in declaration order. Only edges declared through `huxerui_use_library` participate in resource assembly and platform-package discovery.

Library resource directories are ordinary ordered target resource roots.
Their CMake `NAMESPACE` selects the domain, and applications may add a later root with the same namespace to replace selected variants.
The final package is one merged binary index and payload set rather than a runtime collection of library bundles.
PATH and URL libraries compile these packages from source; the binary installation contract for resource-bearing predeclared libraries remains future packaging work.

### Platform packages

Platform dependencies remain expressed in their owning ecosystem.
An Android library's `platform/android` directory is an Android library whose Gradle build declares Java or Kotlin sources, Android resources, C++ dependencies, and third-party libraries, while its library manifest declares mergeable Android components and permissions.
An iOS library's Swift package root is `platform/ios/<Swift product>/`. Its `Package.swift` declares targets, resources, linked system libraries, and Swift package dependencies.
The product name matches the suffix of a qualified public CMake target, such as `Camera` for `Scave::Camera`, or the CLI's normalized product name for an unqualified target. The template and aggregator derive the same directory and product from that target.

The application shell owns final application policy even when a platform package contributes mergeable declarations.
Android applications may override or remove declarations merged from a library manifest.
Apple usage descriptions, entitlements, capabilities, signing, and other application-target policy remain in the source-controlled Xcode shell; a Swift package never invents application-facing privacy text.
The CLI does not translate these declarations into CMake or synthesize policy on the application's behalf.

### Runtime registration

Platform package attachment makes library implementations available to the application target. The application explicitly installs each library that needs runtime registration through `AppOptions::application_hooks`. The library's hook registers its typed factories and chooses the correct scope for retained services. Generated build integration does not register runtime objects.

See [Platform Content Integration](platform-content.md) for the application-owned catalog, native view lifecycle, controllers, and cross-language payload bridge. The [ExternalTexture contract](platform-content.md#externaltexture) describes frame publication and renderer lifetime. Applications use the library's public components, typed services, and controllers.

## Validation

CLI tests cover project creation, shell ownership, tool discovery and setup planning, device resolution, and build and launch commands. Setup fixtures verify changes through the same checks that `doctor` reports. CMake integration tests cover declaration-only graph discovery, target acquisition, dependency ordering, and resource assembly.

Validate build and launch changes against each affected platform available on the development host. Use the [SDK archive workflow](../development/sdk-packaging.md#validate-an-archive) for installed consumers, SDK switching, artifact checks, and release validation.
