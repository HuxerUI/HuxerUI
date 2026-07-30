# HuxerUI Host Tools

Host tools are distributed by operating system and architecture:

```text
prebuilt/<windows|macos|linux>/<x86_64|arm64>/huxerui-<tool>[.exe]
```

These executables run on the build host. Their platform and architecture are independent of the application target and Android ABI. CMake selects the matching executable automatically and stops configuration when that host package is unavailable.
