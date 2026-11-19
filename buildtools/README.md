# Build Tools

This directory holds what makes HuxerUI buildable by more than one build system: the contract every build-system integration implements, the integrations' entry points, and the conformance suite that checks them.
It is distinct from `tools/`, which holds the host programs a build runs (`hcg`, `hrc`, and the CLI).

| Path | Content |
| --- | --- |
| [`specs/`](specs/build-system-integration.md) | The Build System Integration Contract: the standard project model, CLI intent, platform shells, libraries, generated records, and conformance. |
| [`cmake/`](cmake/README.md) | The CMake integration's support declaration and conformance adapter. Its implementation stays in the repository root `cmake/` directory, whose location is part of the SDK source layout. |
| [`mcpp/`](mcpp/README.md) | The mcpp integration: rule package, shared source selection, tools, unit-test package, examples, its support declaration, and its conformance adapter. |
| [`cts/`](cts/README.md) | The conformance test suite: real projects that every integration builds, the facts each must produce, and the runner. |

The root `CMakeLists.txt`, `mcpp.toml`, and `build.mcpp` remain at the repository root because each build system requires its root declaration there.

## Adding a build system

1. Add `buildtools/<name>/support.json` declaring the platforms, hosts, and capabilities the integration supports.
2. Add the build system's native declaration to every project under `cts/projects/` and to their libraries.
3. Add `buildtools/<name>/cts_adapter.py`, which builds a project once and translates the native results into the contract's record; [`cts/adapter_api.py`](cts/adapter_api.py) defines what an adapter provides. The runner discovers the integration from its `support.json` and loads the adapter beside it, so `cts/cts.py` does not change.
4. Add its job to `.github/workflows/build-system-conformance.yml`.

A project's `expected.json` does not change when an integration is added: the expected facts belong to the project, not to an integration.
