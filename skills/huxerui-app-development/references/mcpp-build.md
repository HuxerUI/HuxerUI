# Consuming a Prebuilt SDK with mcpp

Use this reference only for an existing mcpp project that manually consumes a prebuilt HuxerUI SDK. Package projects using `huxerui.huxerui` and `huxerui.rules` follow [cpp-modules-and-mcpp.md](cpp-modules-and-mcpp.md); preserve that integration rather than replacing it with manual SDK flags.

`huxerui mcpp build` validates the source directory and delegates to mcpp. It does not discover the SDK, inject its link interface, or run HuxerUI code/resource generation automatically. Existing manual consumers must provide those integrations themselves.

## Resolve the selected SDK

Use the project's configured SDK path first. If it does not specify one, follow [SDK discovery](project-workflow.md#locate-the-tools-and-sdk). Verify that the selected SDK contains `include/huxerui/huxerui.h` and locate its `HuxerUIConfig.cmake`. Inspect `huxerui mcpp --help` and the installed mcpp help before relying on flags.

Read the installed exported targets and platform package fragments for include paths, library names, transitive libraries, link options, and runtime files. Use release metadata for that exact artifact to establish compiler, architecture, standard library, runtime linkage, and deployment baseline. An unrelated source checkout's current CI is not evidence of how an older SDK binary was built.

`huxerui doctor` diagnoses host prerequisites; success does not prove ABI compatibility. A compiling header or CMake package also cannot establish the binary's complete compiler provenance.

## Configure the consumer

- Select a supported language standard; the header-based SDK baseline is C++20.
- Match the artifact's architecture and compatible compiler, standard library, exception/RTTI, runtime linkage, and deployment settings.
- Use public SDK headers and libraries rather than private source directories or object files.
- For static linking, reproduce the installed public target's platform dependencies and link order.
- For shared linking, preserve runtime-library deployment and loading paths.
- Keep explicit codegen and resource-generation steps when the application uses those facilities; the delegating frontend does not supply them.

The frontend accepts:

```text
huxerui mcpp build [--project <dir>] [--release] [--locked] [--offline] [--verbose]
```

`--project` selects the directory containing `mcpp.toml` and defaults to the current directory. Use `mcpp run` when the configured project supplies a runnable target. Follow the installed mcpp version's manifest syntax.

## Diagnose and validate

Classify failures before changing configuration:

- Missing headers/libraries: selected SDK path or public link interface.
- Undefined framework/platform symbols: transitive libraries, static link order, or incompatible binary interfaces.
- Symbol-version or loader errors: target runtime baseline and deployed dependencies.
- Missing transformed composables or packaged resources: the consumer's generation/staging integration.

A newer `-std` setting does not rebuild the SDK or repair an ABI mismatch. Do not mix incompatible architectures, standard libraries, MSVC/MinGW binaries, or static/dynamic runtime policies across a boundary exposing HuxerUI or standard-library types.

Validate the affected host with the existing toolchain. For distribution work, inspect actual dependencies and deployment targets with the appropriate platform tools, then run the result on a supported target when available. Report the compiler, architecture, linkage, and checks that ran; successful compilation alone is not a portability claim.
