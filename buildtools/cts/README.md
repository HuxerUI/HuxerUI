# Build System Conformance Test Suite

The suite checks that a build-system integration produces the standard model of the [Build System Integration Contract](../specs/build-system-integration.md).
It consists of real HuxerUI projects, the facts each project must produce, and a runner that builds them with one integration and reads those facts from the generated records and the produced artifact.

No integration is the reference.
Each project states its expected facts once in `expected.json`, and every integration is checked against that file.

## Projects

Each project has one set of sources and resources, plus one native declaration per build system.
It carries no platform shell: before a project is built for a target, the CLI `--cli` names adds that target's standard shell with `huxerui platform add <platform>`, as a developer does, so the suite checks the shells the CLI ships; the project ignores what it adds.

| Project | Checks |
| --- | --- |
| `app_minimal` | Default profile, default artifact name, static framework linkage, the final resource package and its runtime location, composable code generation, and resource resolution in a started application. |
| `app_library` | A library that depends on another library: the ordered library-graph record, public identities and derived resource namespaces, library and transitive resources in the final package, and their resolution at run time. |
| `app_modules` | The same program as `app_library` written only with C++ modules and no `#include`. It requires the `modules` capability and must produce the same library graph and resource package as `app_library`. |

A project lists the capabilities it needs in `requires`.

### Created projects

The suite also checks the projects `huxerui create` writes, so a change to the CLI or the templates that breaks a new project fails here.
[`created-projects.json`](created-projects.json) declares each one by kind and build systems: an application with CMake, one with mcpp, one with both, a library with mcpp and its Preview, and a library with both, which also exports its headers as a module.
Into that library's Preview the runner writes `src/cts_import_check.cppm`, a module unit that imports the library's module; the Preview's mcpp declaration compiles it and its CMake declaration, which globs `*.cpp`, does not, so the mcpp build fails when the generated interface does not reach the library's names.
A project is created for every integration whose declaration it carries, and the library's Preview is the application that is built.
`platforms` states whether a project is created with only the checked target's platform or with every platform the CLI offers, so a project missing another platform's shell and one carrying all of them are both checked.
Expected facts are keyed by kind, source style, and template, never by build system, so the application that declares both build systems expects what each single-system application does, and the integrations that build it must agree on it. Created projects name no `sameModelAs`: the CLI writes each project's name into its resources, so two created projects never share a resource package.
The file also declares the targets and hosts where the projects are created; they start with Linux, the Web, and Android on Linux hosts.
Created projects carry no probe, so their `launch` checkpoint has no facts to read.
When it creates a project, the runner records in `created.json` whether the root carries the native declaration, named by `declaration` in each `support.json`, of every build system it selected and of no other; the `build` checkpoint checks that fact as `create.declarations`.

## Checkpoints

The suite is divided into checkpoints so a run shows which part of the contract an integration meets.

| Checkpoint | Checked against `expected.json` | Compared between integrations |
| --- | --- | --- |
| `build` | `profile`, `artifact.name`, `artifact.bundle`; `rebuild`, which the contract fixes: the runner builds every project twice and the second build of unchanged inputs must succeed; for created projects, `create.declarations` | |
| `linkage` | `framework.linkage`, read from the artifact's dynamic dependencies; on Android also `android.exports`, on iOS `ios.link_inputs` | |
| `resources` | `resources.location`: the final package where the Runtime looks for it | `resources.sha256`, `resources.files` |
| `libraries` | `libraries`: the ordered library-graph record | `libraries.roots` |
| `launch` | `launch.*`: what the started application resolved | |
| `package` | `package.kind`: the distributable `huxerui package` published; `package.location` and `package.resources`, which the contract fixes: it is below `dist/<platform>` and carries the resource package the build produced; `package.dispatch`: the CLI is called as a developer calls it, with no `--build-system` for a root that declares only the checked integration or that declares several when the checked integration is CMake, and with it otherwise, and must state that it built with the checked integration; on Android `package.library_code`, the libraries' platform-package classes the APK carries, which `app_library`'s libraries contribute once by the standard layout and once by a declaration that adds a source root | |
| `package-shellless` | The same facts for `huxerui package --no-shell`, on Android and iOS, where an integration packages without the standard shell | |
| `usage` | The exit codes the contract fixes for `huxerui build`: `usage.unknown_build_system` and, on a root that does not declare another integration, `usage.undeclared_build_system` are invalid usage; `usage.product_option`, each product option the integration declares inapplicable, is invalid usage; `usage.driver_option`, each such driver option, is reported; `usage.operation_failure`, a build of a copy carrying an unknown platform directory, is an operation failure. A fact that does not apply is left out with its reason | |
| `doctor` | `doctor.ready`: `huxerui doctor <platform>` run in a built project as a developer runs it reports ready; a run that needs `--build-system` to reach the checked integration has no verdict, since the diagnosis plans for the root's default | |

The compared facts are also checked between a project and the project it names in `sameModelAs`.
A difference is accepted only when [`allowed-differences.json`](allowed-differences.json) lists it with its reason.
The list is empty.

## Coverage

Every fact a checkpoint reads names the contract [clauses](../specs/build-system-integration.md#clauses) it covers, in `CHECKS` in `cts.py`.
A clause is `checked` when some project's `expected.json`, or the contract itself, fixes the value of one of its facts, `compared only` when its facts are only compared between integrations, and `not covered` otherwise.
`python3 buildtools/cts/cts.py coverage --check` fails when this table is stale or a checkpoint names an unknown clause; `--write` regenerates it.

<!-- coverage:begin (generated by `cts.py coverage --write`) -->

| Clause | Coverage | Checkpoints |
| --- | --- | --- |
| `DEFAULT-PROFILE` | checked | `build` |
| `EXIT-CODES` | checked | `usage` |
| `ABI-SELECT` | not covered |  |
| `DISPATCH-ROOT` | checked | `package`, `package-shellless`, `usage` |
| `DISPATCH-DEFAULT` | checked | `package`, `package-shellless` |
| `DOCTOR-DISPATCH` | checked | `doctor` |
| `OPTION-INAPPLICABLE` | checked | `usage` |
| `CREATE-SELECT` | checked | `build` |
| `LAYOUT-COMMON` | not covered |  |
| `SETTINGS-PRECEDENCE` | not covered |  |
| `SETTINGS-DIAGNOSE` | not covered |  |
| `SETTINGS-EFFECTIVE` | not covered |  |
| `LINK-STATIC` | checked | `linkage` |
| `LINK-SHARED` | not covered |  |
| `NAME-FRAMEWORK` | not covered |  |
| `NAME-ARTIFACT` | checked | `build` |
| `CORE-SHELL` | checked | `linkage` |
| `ENTRY-POINTS` | checked | `linkage` |
| `LIB-GRAPH` | checked | `libraries` |
| `LIB-PLATFORM` | checked | `package`, `package-shellless` |
| `LIB-IDENTITY` | compared only | `libraries` |
| `RES-PACKAGE` | compared only | `resources` |
| `RES-LOCATION` | checked | `resources` |
| `SOURCE-SDK` | not covered |  |
| `INCREMENTAL` | checked | `build` |
| `RECORD-SCHEMA` | not covered |  |
| `RECORD-EQUIVALENCE` | compared only | `libraries` |
| `PKG-KIND` | checked | `package`, `package-shellless` |
| `SHELL-EXPLICIT` | checked | `package-shellless` |
| `PKG-PUBLISH` | checked | `package`, `package-shellless` |
| `LAUNCH` | checked | `launch` |

<!-- coverage:end -->

## Declared support

Each integration states what it supports in `buildtools/<integration>/support.json`: for every target platform, the hosts that can build it, the checkpoints it implements, and its capabilities.
It also names its native declaration in `declaration`, the arguments every CLI command it is reached through needs in `cli_arguments`, such as CMake's framework location, and the options it cannot honor in `inapplicable`, as `product` options that are invalid usage and `driver` options that are reported; `{repository}` in an argument names this checkout.
The runner reads these declarations and names no integration itself.
The suite checks an integration for exactly that.
An undeclared target, host, checkpoint, or capability is reported as `undeclared` and skipped; an environment that cannot run a check, such as a machine without a display, is reported as `unavailable`.
Neither is a pass or a failure.

## Running

```bash
python3 buildtools/cts/cts.py list
python3 buildtools/cts/cts.py create --integration cmake --target linux --cli build/cli/huxerui --out /tmp/cts
python3 buildtools/cts/cts.py build --integration cmake --target linux --cli build/cli/huxerui --out /tmp/cts
python3 buildtools/cts/cts.py build --integration mcpp --target linux --mcpp-toolchain llvm@22.1.8 --cli build/cli/huxerui --out /tmp/cts
python3 buildtools/cts/cts.py check --integration mcpp --target linux --check resources libraries --out /tmp/cts
python3 buildtools/cts/cts.py sequence --integration mcpp --targets linux,web,android --cli build/cli/huxerui --out /tmp/cts-sequence
python3 buildtools/cts/cts.py compare --check resources /tmp/cts/results
python3 buildtools/cts/cts.py matrix /tmp/cts/results
```

`create` writes the created projects declared for an integration and target below `--out` with the CLI `--cli` names, and later commands with the same `--out` include them.
`sequence` builds the suite's projects for several targets in the same project trees, one after another, as a developer on one host does: it runs the `build`, `linkage`, `resources`, and `libraries` checkpoints for each target, builds the targets without a standard shell with one `huxerui build <list>` when `--cli` is given, and finally builds the first target again, which must record the facts it recorded first.
`build` builds the projects and records their facts; `check` evaluates the named checkpoints, or all of them when none is named; `run` does both.
`--project` limits any of them to the named projects.
`compare` checks the facts integrations must agree on, and `matrix` prints the build status of every project and integration.
Every command exits non-zero when something it checked failed.
The `launch` checkpoint starts each application with `HUXERUI_CTS_PROBE` set and reads what it recorded; on the Web it serves the build with the page the build lays beside the module, `<target>.html`, and reads the facts the application prints to the console of a headless Chrome or Chromium, which `HUXERUI_CTS_BROWSER` names when it is not on `PATH`.
On Android and iOS it starts the distributable the `package` checkpoint published, and packages a project that checks no package facts the same way first: the APK on the one attached device or emulator, whose ABI the package is then built for, reading the `HUXERUI_CTS_PROBE` log tag; and the application bundle, built for the host's architecture, on a booted iOS Simulator, or the first available iPhone Simulator it boots, reading the application's standard output.
A device launch that records nothing before the timeout, or a host with no device, is reported as `unavailable`, with the device log kept in the work directory.
The `package` checkpoint runs `huxerui package <platform> --build-system <integration>` in the project, as a developer does, with the CLI `--cli` names, reads the published distributable, including the MSI a WiX setup executable carries, and removes what the command created in the project.
On Android and iOS the package comes from the standard shell, and Android is packaged for `arm64-v8a`.
The `package-shellless` checkpoint adds `--no-shell` and is declared only by an integration that offers it.
The `usage` checkpoint runs `huxerui build` with what a developer can get wrong and reads only the exit code and, for a driver option, whether the option was reported; it follows `package`, so the release build a driver option repeats has nothing left to do.
The `doctor` checkpoint is declared for the desktop platforms and the Web; on Android and iOS the diagnosis also asks for SDK components the standard shell installs itself, so it is not yet a statement about the build system.
A project without `package.kind` in `expected.json` is not packaged.

## Targets

| Target | What is checked |
| --- | --- |
| `linux`, `windows` | The program, its dynamic dependencies, and the package in `<program>.resources/`; the application is started. |
| `macos` | The application bundle, the program inside it, and the package in the bundle's resources; the application is started. |
| `web` | The ES module and WebAssembly module, and the package preloaded into the module's data file; the application is started in a headless browser. |
| `android` | The application core the platform shell consumes: `lib<target>.so` for `arm64-v8a`, its exported JNI entries, and the final resource package. |
| `ios` | The application core the Xcode shell links: `lib<target>_huxerui.a` for the Simulator with the framework inside it, its link inputs, and the final resource package. |

Which hosts build each target is stated per integration in its `support.json`, and the workflow has one job for every declared combination.

The `build`, `linkage`, and `resources` checks of Android and iOS stop at the application core; the `package` checkpoint reads the APK or the application bundle where an integration declares it, and the `launch` checkpoint starts it.
The workflow boots an Android emulator on Linux hosts and runs `launch` after `package` on every row.

`.github/workflows/build-system-conformance.yml` runs every checkpoint as its own step for each integration, then compares the integrations.
The suite checks the build systems rather than the framework, and a run takes hours, so pull requests and pushes to `main` start it only for a build system, the CLI and the tools it runs, the templates, or the suite; framework sources and Markdown start nothing.
Its plan job first runs the suite's own tests (`tests/scripts/cts_test.py`) and then selects, for each changed path, as little as the path can affect, and runs the union:

| Changed path | Runs |
| --- | --- |
| The suite: `cts.py`, `adapter_api.py`, `allowed-differences.json`, and the workflow | every job and every project |
| `buildtools/cts/projects/<project>/**` | that project, and the project its `sameModelAs` relates it to, with the integration a declaration file belongs to or both, on the target a `platform/<target>/` path belongs to or every target |
| `buildtools/cts/created-projects.json` | the created projects, where it declares them |
| A path one integration owns, such as `cmake/**`, `buildtools/mcpp/**`, `mcpp.toml`, or `templates/**` | that integration, every project |
| A path one target owns, such as `cmake/platform/IOS.cmake` or the target's CLI driver and shell templates | that target, every project |
| `tools/prebuilt/<host>/**` | the rows of that host |
| Any other path, such as the rest of the CLI | both integrations on every target, every project |
| The plan script | nothing beyond its tests |

Each selected target runs on its primary host: the target's own host, or a Linux host for the Web and Android.
On a macOS host each integration's selected targets run in turn in one job, since GitHub's macOS runners are few and a job left waiting for one is cancelled unrun.
A `sequence` job per selected integration builds Linux, the Web, and Android in turn on one Linux host when all three are selected for the suite's own projects.
The comparison runs only when both integrations ran.
A manual dispatch runs every job on every host; run one before a release.
To verify a change on a few rows before the whole matrix, a pull request's head commit names them on a `CTS-Only:` line of its message, such as `CTS-Only: mcpp/ios@macos, */android@linux`, and every other job is skipped; `<integration>/sequence@linux` names a sequence job, and a manual dispatch takes the same patterns as its `only` input. A push without the line runs the selection by path again.
`.github/workflows/rerun-unassigned.yml` starts a run's failed jobs again when every job that did not pass is one no hosted runner took, as happens when GitHub's macOS arm64 capacity runs out; a run with a job that ran and failed is left alone.
