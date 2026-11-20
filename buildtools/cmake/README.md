# CMake Integration

CMake is the first implementation of the [Build System Integration Contract](../specs/build-system-integration.md).

Its implementation is the repository root [`cmake/`](../../cmake) directory together with the root `CMakeLists.txt`.
That location is part of the SDK source layout: the CLI recognizes a source checkout by it, and application projects reach the helpers through `HUXERUI_HOME`.
This directory therefore holds only the integration's [support declaration](support.json) and its [conformance adapter](cts_adapter.py), which the [conformance suite](../cts/README.md) reads and loads.

The implementation is documented in [SDK, CLI, Platform Shell, and Library](../../docs/design/sdk-cli.md) and [Building](../../docs/development/building.md).
