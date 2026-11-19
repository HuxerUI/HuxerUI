# HuxerUI Documentation

## User guide

- Use [Installation](guide/installation.md) to install an SDK and switch versions.
- Use [Getting Started](guide/getting-started.md) to create a project, prepare its toolchain, and run it.
- Check [Platform Support](guide/platforms.md) for host requirements and backend capabilities.
- Use [Packaging Applications](guide/packaging.md) to choose target ABIs, build distributable artifacts, and configure signing and runtime dependencies.
- [Core Concepts](guide/core-concepts.md): Views, composition, state, identity, events, and Environment.
- [Layout and Scrolling](guide/layout.md): constraints, responsive layout, scrolling, and virtualization.
- [Components and Input](guide/components.md): controls, navigation, gestures, and text editing.
- [Themes and Presentation](guide/themes-and-presentation.md): themes, indication, animation, and layers.
- [Resources and Images](guide/resources.md): packaged assets, localized strings, and image editing.
- [Files and Storage](guide/files.md): application storage, external files, and pickers.
- [HTTP Client](guide/http.md): buffered and streaming responses, transfer progress, Tasks, and errors.
- [System Tasks](guide/system-tasks.md) explains task selection, control, UI observation, and platform setup.
- [Extending HuxerUI](guide/extending.md): custom components, layouts, modifiers, and platform services.
- [Windowless UI Testing](guide/testing.md): real-Runtime queries, input, virtual frames, and structural captures.
- [C++20/23 modules and mcpp](../skills/huxerui-app-development/references/cpp-modules-and-mcpp.md): `import huxerui;` and `import std;`, module units, applications with no headers, and building with mcpp instead of CMake.

## Framework development

- Use [Building HuxerUI](development/building.md) to configure the source tree, run tests, and build examples.
- Use [SDK Packaging](development/sdk-packaging.md) to assemble, validate, and publish SDK archives and maintain host tools.
- Find the private vulnerability reporting channel in the [Security Policy](../SECURITY.md).

## Design reference

[Architecture](design/architecture.md) describes the application, window, and host boundaries. [Layout and Virtualization](design/layout.md) defines measurement, placement, and virtual item lifetime. [Design Documents](design/README.md) indexes the owning contracts and drafts. The [Roadmap](roadmap.md) records future capability areas.
