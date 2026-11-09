# Build System Integration Contract

> Status: Draft for review. This proposes common behavior for HuxerUI build-system integrations; it does not claim that every current workflow implements it.

CMake is the reference integration; mcpp, xmake, and other build systems may implement the same observable contract through their own project formats, dependency resolution, and target APIs.
Conformance applies to each integration's declared host/target combinations and capabilities.
The current implementations remain documented in [SDK, CLI, Platform Shell, and Library](sdk-cli.md) and [mcpp Build System](mcpp-build-system.md).

## Responsibilities

| Owner | Responsibility |
| --- | --- |
| HuxerUI CLI | Select the project, build system, platform, SDK/source, and device; pass explicit options; orchestrate build, launch, and package publication. |
| Build-system integration | Resolve project settings, dependencies, and tools; generate code and resources; compile and link targets; report effective settings and actual artifacts. |
| Platform shell and tools | Own application lifecycle, platform metadata, permissions, signing, installation, launch, and distribution containers. |

Projects keep their build system's native configuration as the source of truth.
Direct native builds and CLI-driven builds must use the same target, code-generation, resource, and dependency rules.
Any shared request or result records are generated metadata, not another user-maintained project manifest.

## CLI build intent

`build` produces application artifacts; `run` builds the matching configuration and launches it on the selected destination; `package` builds and publishes the platform's distributable output.
The default logical profile is `debug` for `build` and `run`, and `release` for `package`.
The CLI reports exit code `0` for success, `2` for invalid command usage, and `1` for an operation failure, preserving the failing step and underlying tool diagnostics.

The shared request carries the following information where applicable:

| Information | Meaning |
| --- | --- |
| Operation and project | Build, run, or package; project root, application identity, and selected build system. |
| Host and target | Host OS and architecture for executable tools; target platform, optional canonical ABI list, platform SDK, and deployment minimum for produced binaries. |
| Framework selection | Selected HuxerUI SDK/package or source checkout, including its resolved version. |
| Settings | Logical profile and explicit optimization, LTO, and framework-linkage choices. |
| Destination and output | Selected device or simulator, incremental build location, and requested publication location. |

Integrations map this intent to native commands and settings; compiler flags are implementation details of that mapping.
Native configuration names may differ from logical profiles, as with an Android Release variant using a CMake `RelWithDebInfo` configuration.
The request's transport and command-line spelling must be agreed when wiring an integration into the CLI; this draft specifies their meaning.

The CLI's `--abi` option selects one or more ABIs for a single target platform. The CLI resolves aliases to that platform's canonical spellings and passes a deduplicated list to the build-system integration. An absent list preserves native project defaults. Integrations must reject unsupported explicit selections and keep incremental outputs for different ABI selections distinct where the native tools require it.

Report which ABIs each produced artifact contains. Integrations may produce one artifact with several ABIs where the platform supports it, or separate artifacts with unambiguous ABI-specific locations. The requested selection must be satisfied in full.

## Common settings

Explicit CLI values take precedence over matching project options, followed by integration defaults.
An omitted value preserves project configuration; explicit `off` and explicit `default` remain distinguishable from omission.
These settings govern integration-managed source targets and build steps; independent project toolchain customization remains project-owned.

| Setting | Values and default | Required meaning |
| --- | --- | --- |
| Profile | `debug`, `release`; operation-dependent default above | Select development or optimized application configuration independently of the options below. |
| Optimization | `default`, `size`, `minsize`; `default` | Keep the native profile's optimization level, prefer code size, or select a stronger size mode supported by the toolchain. |
| LTO | `on`, `off`; `off` | Enable or disable integration-managed link-time optimization in release configurations. |
| Framework linkage | `static`, `shared`; applications default to `static` on every platform | Select the HuxerUI framework's linkage, independently of system and third-party dependency linkage. |

Optimization and LTO choices apply to release configurations; they do not enable release optimization in Debug.
Integrations must diagnose an unsupported explicit choice rather than silently substitute another mode.
They must report effective settings and their scope: prebuilt libraries retain their published settings, while source targets can be rebuilt with overrides.
Size optimization preserves the framework's required exception, RTTI, threading, ownership, and lifecycle behavior.

The current CMake mappings are `HUXERUI_OPTIMIZATION` and `HUXERUI_ENABLE_LTO`.
`HUXERUI_BUILD_SHARED` and `HUXERUI_BUILD_STATIC` select available source framework artifacts; the consuming target selects its link form.
The CLI currently exposes optimization and LTO overrides; uniform framework-linkage control is proposed by this draft.

## Linkage and artifact names

Applications use static HuxerUI linkage by default on every supported platform and build-system integration.
Shared framework linkage requires an explicit selection and declared platform support; an unavailable link form produces a diagnostic.
SDKs and framework source builds may provide both forms, while application consumption follows the same static default.
System and third-party dependencies retain their own linkage requirements.

Framework artifact basenames are fixed across integrations:

| Framework artifact | Basename | Platform filenames |
| --- | --- | --- |
| Static library | `huxerui_static` | `huxerui_static.lib` on Windows; `libhuxerui_static.a` on the other current platforms. |
| Shared library, where supported | `huxerui` | `huxerui.dll` on Windows, `libhuxerui.so` on Linux/Android, and `libhuxerui.dylib` on macOS. |

The iOS static XCFramework contains `libhuxerui_static.a` in each slice, and a Windows shared-framework import library uses `huxerui.lib`.
Filenames above are shown before any configuration postfix.
The current Web SDK archive is named `libhuxerui.a`; adopting this draft requires updating its packaging and consumers to `libhuxerui_static.a` together.

An application's binary or compiled-module basename defaults to its declared application target name.
Platform prefixes, extensions, and bundle formats apply to that basename: for a target named `gallery`, examples include `gallery.exe`, `gallery`, `gallery.app`, `libgallery.so`, and the Web modules `gallery.js` and `gallery.wasm`.
Android's default static-framework integration links HuxerUI into `lib<application-target>.so`, and its shell loads the actual application library reported by the build.
Bundle display names, application identifiers, and distribution-container filenames remain owned by platform configuration.

For the same declared target, platform, configuration, and naming settings, all integrations must produce the same default application and framework filenames.
Applications may explicitly override their output basename through native project settings.
Application overrides and any configuration postfixes must be reflected in generated artifact metadata; the shell must not require a second manual library-name setting.

## Targets and dependencies

The framework dependency must communicate its public includes, minimum C++20 requirement, and required compile/link usage requirements.
Higher language modes are allowed when supported by the selected toolchain; C++ module support is a separately declared capability.
HuxerUI-managed C++ targets use standard language mode with extensions disabled.
Consumers use the public headers and APIs without implementation-only include paths.

The integration must preserve the complete dependency closure, including static libraries, system frameworks, required runtime libraries, and configuration-dependent link requirements.
Combining archives does not replace dependency propagation.
Artifacts must match the selected target ABI, toolchain runtime, and declared deployment requirements; dependency acquisition may use the build system's native package manager.

Entry points discovered by the platform must survive archive extraction, dead stripping, and LTO.
On Android, static framework integration retains the required named JNI entries and loads the application's actual shared-library name before HuxerUI Java services are used.
Libraries retain ownership of their initialization; linking archives does not combine multiple `JNI_OnLoad` implementations.

## Code generation and resources

`hcg` and `hrc` execute on the host and must be compatible with the selected HuxerUI distribution.
Each source requiring composable transformation is compiled through its transformed output exactly once, with the original source's include context preserved.
Generated files stay in build-owned output locations.
Changes to source files, resource inputs, tools, or relevant options must invalidate their generated outputs and dependent build steps.

Built-in, library, and application resources follow the existing [resource ordering, namespace, and package contract](resources.md).
The integration produces one final application resource package using the current `hrc` format; the platform integration owns embedding or staging it and its runtime discovery.
Framework libraries, resources, headers, and host tools must resolve consistently in both supported SDK and source-checkout workflows.

## Generated results

Reuse the existing project, ordered library-graph, and application-integration records where their semantics apply.
Shared machine-readable records use JSON with an explicit schema version and describe:

- Project and application identity, plus ordered library identities and source roots needed by platform integration.
- Requested and effective configuration, target ABI, framework selection, and applicable common settings.
- Actual artifact paths and kinds, resource locations, and platform-specific bundle or library-loading information needed by the caller.

Artifact information comes from the configured targets or native tool outputs, including output-name and configuration-postfix changes.
The CLI consumes these results instead of guessing filenames or parsing build scripts for target declarations.
An unsupported schema version or a missing required artifact must produce a diagnostic.
Changes to target, toolchain, framework selection, profile, or effective options must trigger the appropriate reconfiguration or rebuild; cache directory layout remains build-system-owned.
Package publication validates the staged result before replacing the previous published output.

## Minimum acceptance

An integration's declared support is checked with:

- CLI and direct native builds using equivalent project settings and producing equivalent application behavior.
- Clean and incremental builds in declared SDK/source modes and Debug/Release configurations.
- Supported optimization, LTO, and link-form choices, including explicit `off`, inherited defaults, and unsupported-option diagnostics.
- A library dependency with external link requirements, required platform entry points, and application/library resources.
- Source and resource changes; default artifact names, explicit output-name overrides, and configuration postfixes; paths containing spaces; multiple configurations or ABIs where supported.
- Installation or package relocation, startup, resource loading, and validation of the packaged native artifacts on available target environments.

Unavailable hosts or devices are reported separately from passed checks.
Acceptance checks observable outcomes; internal target names, helper organization, and compiler flag sequences may differ across build systems.
