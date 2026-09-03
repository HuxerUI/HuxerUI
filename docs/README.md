# HuxerUI Documentation

The documentation is organized by audience.
User guides describe the current public SDK, development guides cover this repository, and design documents record internal ownership and invariants.

## User guide

- [Installation](guide/installation.md): install, upgrade, select, and remove an SDK.
- [Getting Started](guide/getting-started.md): create, diagnose, build, and run an application.
- [Core Concepts](guide/core-concepts.md): Views, composition, state, identity, events, and Environment.
- [Layout and Scrolling](guide/layout.md): constraints, responsive layout, scrolling, and virtualization.
- [Components and Input](guide/components.md): controls, navigation, gestures, and text editing.
- [Themes and Presentation](guide/themes-and-presentation.md): themes, indication, animation, and layers.
- [Files and Storage](guide/files.md): application storage, external files, and pickers.
- [HTTP Client](guide/http.md): buffered and streaming responses, transfer progress, Tasks, and errors.
- [Extending HuxerUI](guide/extending.md): custom components, layouts, modifiers, and platform services.
- [Platform Support](guide/platforms.md): supported hosts, toolchains, and platform capabilities.
- [Packaging Applications](guide/packaging.md): platform-native outputs, explicit runtime payloads, and custom Windows installer interfaces.

## Framework development

- [Building HuxerUI](development/building.md): configure, build, test, and run repository examples.
- [SDK Packaging](development/sdk-packaging.md): produce and validate release-ready SDK archives.

## Design reference

[Design Documents](design/README.md) indexes the internal contracts by subsystem.
[Scrolling](design/scrolling.md) defines input normalization, nested consumption, offset ownership, momentum, and overscroll.
Design documents are implementation references, not a public API stability guarantee.

## Scope

Documentation describes released behavior present in the repository.
[Roadmap](roadmap.md) records the next capability areas without assigning release dates.
Detailed proposals remain explicitly marked as future work until implemented.
