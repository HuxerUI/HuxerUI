# HuxerUI Host Tools

Host tools are distributed by operating system and architecture:

```text
prebuilt/<windows|macos|linux>/<x86_64|arm64>/huxerui-<tool>[.exe]
```

These executables run on the build host. Their platform and architecture are independent of the application target and Android ABI. CMake selects the matching executable automatically and stops configuration when that host package is unavailable.

Current host tools are:

- `huxerui-codegen` for `[[huxerui::scope]]` transformation
- `huxerui-resource-codegen` for typed keys, the resource index, and package staging

Each distributed host and architecture directory must contain every tool required by the project configuration.

Prebuilt executables must be rebuilt from the matching tool source whenever that source changes. Tests compile the tool sources directly and therefore do not prove that a distributed executable is current.
