# HuxerUI × mcpp 双构建体系可行性分析报告

**分析对象**：HuxerUI（`/home/speak/workspace/scode/HuxerUI`）作为被构建方，mcpp（`/home/speak/workspace/github/mcpp-community/mcpp`，本机已安装 `2026.9.8.1`）作为原生构建工具。

**约束**：在**不影响现有 CMake 构建体系**的前提下，支持 CMake + mcpp 双构建；并评估 C++20 modules / `import` 的落地路径。

**本机环境基线**：CMake 3.28.3、Ninja 1.13、GCC 16.1.0、mcpp 2026.9.8.1。

---

## 0. 结论摘要

| 问题 | 结论 |
|---|---|
| mcpp 能否构建 HuxerUI **应用**（桌面三平台）？ | **能，且已有可运行的原型**（`examples/mcpp_demo`）。缺的是产品化，不是可行性。 |
| mcpp 能否构建 HuxerUI **框架库本身**？ | **Linux / Windows / macOS 能**。所有必需机制（平台条件源、平台库、`hcg`/`hrc` 代码生成、静/动态库）在 mcpp 中都有对应表达。 |
| Android / iOS / Web？ | **上游引擎改动**，不是配置也不是包能补的。`modules/toolchain-model/src/triple.cppm:783-785` 显式声明 `androideabi` / `wasi` "not in mcpp's target language"。研究方向见 §3.1。 |
| 除此之外，mcpp + build.mcpp + mcpp-index + xlings 能否覆盖本项目全部需求？ | **能**。逐项核对见 §2 与 §5.1；唯一不建议交给 mcpp 的是测试（§3.2），那是模型差异不是能力差异。 |
| C++20 modules / `import huxerui;`？ | **可行，但必须走"模块外壳（wrapper）"路线，不能原生模块化**。存在一个具体的、当前会直接编译失败的阻塞点（见 §6.3），修复成本很低。 |
| 对 CMake 体系的影响 | **可以做到严格为零**（新增文件，不改任何现有 `.cmake` / `CMakeLists.txt`）。唯一例外是 §6.3 的 codegen 修复，那是一个 2 行改动 + 测试更新。 |
| 最大的真实风险 | 不是"能不能构建"，而是**两套构建体系的漂移**。报告的核心建议是把重叠部分做成**机器校验**而非文档约定（§5.4）。 |

**推荐落地顺序**：**框架包优先** —— Linux 框架包 → 应用侧（几乎白拿）→ Windows/macOS → 发布到 index → modules 外壳。

> **修订说明（本节为第二版）**：初版把"应用侧"排在"框架侧"之前，理由是应用侧风险低。核实后这个排序是错的，三条事实推翻了它：
> 1. **框架核心不需要 `hcg`** —— `src/` + `platform/` 中 `[[huxerui::composable]]` 命中数为 **0**。`huxerui_enable_codegen()` 只在 `HuxerUIApp.cmake:249`（应用）与 `HuxerUILibraries.cmake:198`（用户库）被调用。框架侧所需机制**严格少于**应用侧。
> 2. **框架成为 mcpp 包之后，应用侧最难的两块自动消失** —— 依赖的 `build.mcpp` 发射的 `link-lib`/`link-search` **直达最终链接**（`docs/30-build-mcpp.md:835-845`），`reexport = true` 把 `tools`/`host-module` 交给消费者（`docs/30:1103-1140`）。初版 §5.1 设计的"rules-app 做 SDK 发现 + 链接接口翻译"是在**手工重造包管理**，那是"框架不是包"这一缺失状态下的权宜之计。
> 3. **`hcg`/`hrc` 是零第三方依赖的独立 C++20 程序**（只用标准库，各自独立 `project()`，不链接 HuxerUI），可直接作为 mcpp 的 `kind = "bin"` target 被 `tools = [...]` 请求、为宿主构建、全局缓存 —— 连 `tools/prebuilt/` 的 6 份随仓二进制在 mcpp 侧都不需要。

---

## 1. 现状盘点

### 1.1 HuxerUI 当前构建体系

```
CMakeLists.txt              (~90 行, cmake_minimum_required 3.20, C++20 / extensions OFF)
cmake/*.cmake               12 个模块
cmake/platform/*.cmake      6 个平台模块        合计 3371 行
```

构建图：

```
src/*/*.cpp (67)  ─┐
platform/<os>/*   ─┼─> huxerui_core_objects (OBJECT, PIC)
内置资源头         ─┘        │
                             ├─> huxerui        (SHARED)
                             └─> huxerui_static (STATIC)
                                      │
                          huxerui_add_app() ─> 可执行 / SHARED(Android) / STATIC(iOS)
```

规模统计：

| 项目 | 数量 |
|---|---|
| `src/*/*.cpp` 共享核心 | 67 |
| `src/**/*.h` 私有头 | 33 |
| `include/huxerui/**` 公共头 | 55 |
| `platform/` 源文件 | linux 20 / windows 26 / macos 23 / android 73 / ios 55 / web 23 |
| `platform/` 扩展名分布 | 47 `.cpp`、25 `.mm`、26 `.java`、59 `.h` |
| 测试 `.cpp` | 105（Catch2，多文件链接为单个测试二进制） |
| `examples/` | 36 个应用 |

两个宿主工具（`tools/prebuilt/<host>/<arch>/`，随仓库分发的预编译二进制）：

| 工具 | 输入 → 输出 | 在 CMake 中的接入点 |
|---|---|---|
| `hcg` | `--input <src> --output <src>`，纯文本源到源变换，为 `[[huxerui::composable]]` 函数注入作用域 | `huxerui_enable_codegen()`（`cmake/HuxerUICodegen.cmake`，`cmake_language(DEFER)` 重写 target SOURCES） |
| `hrc` | `--root <dir> --output <dir> --namespace <ns>`，产出 `resources.bin` + `<ns>_resources.h`，另有 `merge` 子命令 | `cmake/HuxerUIResources.cmake` + `HuxerUIResourceBuild.cmake` 驱动脚本 |

**已存在的 mcpp 接触点**（重要——这不是从零开始）：

1. `tools/huxerui_cli/mcpp/mcpp.cpp`（147 行）——`huxerui mcpp build` 前端。目前是**纯转发**：校验 `mcpp.toml` 存在，然后 exec `mcpp build`。不做 SDK 发现、不注入编译选项、不翻译 CMake 链接接口、不跑 `hcg`/`hrc`。
2. `examples/mcpp_demo/` ——可运行的宿主特定 PoC。手写了 20 行 `/lib64/*.so` 绝对路径，`README.md` 自述为"delegation path 的宿主特定证明，不含打包与跨平台集成"。
3. `skills/huxerui-app-development/references/mcpp-build.md` ——已经把"独立 mcpp 项目消费 HuxerUI SDK"的 ABI 纪律写清楚了（编译器族、标准库、linkage 必须与 SDK release 对齐）。这份文档的结论在本报告中继续有效。

**当前 modules 使用量：0。** 全仓库 `src/`、`include/`、`platform/`、`examples/` 中没有任何 `export module` / `import` 声明。

### 1.2 mcpp 能力基线（2026.9.8.1）

mcpp 把通常分散在五个工具里的职责合成一个命令：构建系统（≈CMake+Ninja）、构建插件（≈CMake modules / xmake rules）、包管理（≈vcpkg/Conan）、工具链管理（≈rustup）、环境与运行时（≈conda/Nix）。

与本次评估直接相关的机制：

| 机制 | 用途 | 文档 |
|---|---|---|
| `[build] sources` / `include_dirs` / `private_include_dirs` / `cxxflags` / `ldflags` / `defines` | 声明式构建输入 | 04 §2.3 |
| `[target.<sel>]`，`<sel>` = 裸别名 / `cfg(...)` 谓词 / 精确 triple | 平台条件的**源、flags、include、依赖、runtime 库** | 22 §`[target.*]` |
| `[target.<sel>].runtime` 的 `libraries` / `link_library_dirs` | **方言中立**的链接行（自动渲染为 `-lfoo` 或 `foo.lib`） | 22 |
| `build.mcpp` + `import mcpp;` | 构建期程序，可探测机器、发射 `cxxflag`/`link-lib`/`link-search`/`include-dir` 等指令 | 30 |
| `mcpp::action{role = "source"｜"object"｜"check"｜"artifact"}` | **把外部工具声明为构建图的一条边**（增量、并行、可归因） | 30 |
| `[xlings.workspace]` + `mcpp::xpkg_dir()` | 声明并定位非编译器的依赖载荷 | 23 |
| `[workspace] members` | 单仓库多包 | 07 |
| `module_extensions` / `.cppm` / `import std` | C++20/23 模块 | 04 §2.3 |
| `dependency_linkage = static｜shared` | **静态还是动态由消费者决定**，而非库作者 | 04 §2.3 |
| `--message-format json` | 机器可读输出（`schemaVersion` + `kind` 信封） | 50 |

**源文件类型识别**（`modules/source-kind/src/source_kind.cppm`）：

- C++ 侧：`.cpp` `.cc` `.cxx` **`.mm`**
- C 侧：`.c` **`.m`**
- 汇编：`.S` `.s` `.asm`
- 模块接口：`.cppm`（可通过 `module_extensions` 扩展 `.ixx` / `.ccm`）

即 **Objective-C++ 是一等公民**——这是 macOS 后端（25 个 `.mm`）能落地的前提。注意默认 glob 是 `src/**/*.{cppm,cpp,cc,c,S,s,asm}`，**不含 `.mm`**，必须在 `sources` 中显式列出。

**已可用的系统库包**（`mcpp search` 实测，Linux GTK 栈完整）：

```
xim:gtk4  xim:cairo  xim:gdk-pixbuf  xim:libsoup  xim:libepoxy
xim:harfbuzz  xim:graphene  gnome:pango  gnome:pangocairo
gnome:glib  gnome:gobject  freedesktop:fontconfig
compat:catch2 (3.15.2)  compat:vulkan
```

---

## 2. 能力对照矩阵

逐项把 HuxerUI 的构建需求映射到 mcpp 机制。

| # | HuxerUI 需求 | 当前 CMake 实现 | mcpp 对应机制 | 判定 |
|---|---|---|---|---|
| 1 | 共享核心源集合 | `file(GLOB src/*/*.cpp)` | `[build] sources = ["src/*/*.cpp"]` | ✅ 直接等价 |
| 2 | 平台源选择 | `cmake/platform/*.cmake` 显式枚举 | `[target.'cfg(linux)'.build] sources = [...]` | ✅ |
| 3 | 公共头 / 私有头分离 | `PUBLIC include` + `PRIVATE src` | `include_dirs` + `private_include_dirs` | ✅ 语义完全对应 |
| 4 | 静态库 + 动态库同时产出 | `huxerui_static` + `huxerui` 两 target 复用 OBJECT 库 | `kind = "lib"` 一个包，消费者用 `dependency_linkage` 选形态 | ⚠️ 模型不同但更好；见 §4.4 |
| 5 | 警告等级 | `-Wall -Wextra -Wpedantic` / `/W4 /permissive-` | `[build] cxxflags` + `[target.windows.build] cxxflags` | ✅ |
| 6 | Linux GTK4 依赖 | `pkg_check_modules(REQUIRED IMPORTED_TARGET gtk4>=4.14 ...)` | `build.mcpp` 调 `pkg-config` 发射 `include-dir`/`link-lib`/`link-search`；或 `[xlings.workspace]` 载荷 + `mcpp::xpkg_dir()` | ✅ 两条路，见 §5.3 |
| 7 | Windows 系统库（18 个） | `target_link_libraries(advapi32 d2d1 ...)` | `[target.windows.runtime] libraries = [...]`（方言中立） | ✅ |
| 8 | Windows 版本宏 / `HUXERUI_WINDOWS_7_COMPAT` | `target_compile_definitions` + option | `[target.windows.build] defines` + `[features]` | ✅ feature 是 CMake option 的对等物 |
| 9 | macOS 14 个 framework | `"-framework AppKit"` 等 | `[runtime] frameworks = [...]`；`-weak_framework` 走 `ldflags` | ✅（weak 需 ldflags，见注） |
| 10 | macOS ARC | `-fobjc-arc` | `[target.macos.build] cxxflags` | ✅ |
| 11 | macOS 部署目标 12.0 | `CMAKE_OSX_DEPLOYMENT_TARGET` | `[build] macos_deployment_target` | ✅ |
| 12 | `hcg` composable 变换 | `add_custom_command` + 重写 target SOURCES | `mcpp::action{role="source"}`，输入 `.cpp`、输出改写后的 `.cpp` | ✅ **契合度极高**，见 §5.2 |
| 13 | `hrc` 资源编译 | `add_custom_command` + 驱动脚本 | `mcpp::action{role="source"}`（头 + bin）+ `role="artifact"`（staging） | ✅ |
| 14 | 资源 staging（可执行文件旁 `.resources/`） | `add_custom_command` copy_directory | `role = "artifact"`（其输入是链接产物，故排在链接之后） | ✅ |
| 15 | Web 资源 `--preload-file` | `target_link_options` | — | ❌ Web 平台整体不支持 |
| 16 | 测试 | Catch2，多 `.cpp` 链接成一个二进制 | mcpp：**每个 `tests/**/*.cpp` 各自编译成一个独立程序** | ❌ 模型不匹配，见 §4.2 |
| 17 | 应用集成计划 `app.json` | `file(GENERATE)` | `build.mcpp` 直接写文件，或 `role="artifact"` | ✅ |
| 18 | SDK 打包 / 安装 | `cmake/HuxerUISdk.cmake` + install/export | `mcpp pack` / `mcpp publish` | ⚠️ 语义不同，非阻塞 |
| 19 | Android / iOS / Web | `cmake/platform/{Android,IOS,Web}.cmake` + Gradle/Xcode/Emscripten | — | ❌ **上游阻塞**，见 §4.1 |

> **注（#9）**：mcpp 的 `[runtime] frameworks` 表达常规 framework 链接。`-weak_framework UniformTypeIdentifiers` 是弱链接，需要落到 `[target.macos.build] ldflags = ["-weak_framework", "UniformTypeIdentifiers"]`。

---

## 3. 三个真实缺口

### 3.1 平台覆盖（硬缺口）

mcpp 的 target 表（`docs/21-the-target-triple.md`）当前行：

```
x86_64-linux-gnu (verified)      x86_64/aarch64-linux-musl (verified)
x86_64-windows-gnu (verified)    x86_64-windows-msvc (verified)   x86_64-windows-musl (preview)
aarch64-macos (verified)         x86_64-macos (planned)
riscv64/riscv32-none-elf, thumbv6m/v7m/v7em/v8m-none-eabi(hf)  —— 裸机
```

**没有 android、ios、wasm32 行**，而且这不是"索引里缺包"，是**引擎的 target 语言里没有这些词**。三元组解析器在 `modules/toolchain-model/src/triple.cppm:783-785` 显式写道：

```cpp
// Unrecognized segment (androideabi, wasi, …): not in mcpp's target
// language — treat as unparseable rather than guessing.
return std::nullopt;
```

对照 `docs/22-target-side.md` 的"Layer Names Are Fixed, Implementations Are Not"——**五个层名是编译进引擎的闭集，实现可以来自包**。`os` 恰好落在闭集这一侧：`triple.cppm:34` 的注释就是 `// "linux" | "macos" | "windows"`。所以 BSP（`docs/34`）、adapter（`docs/33`）、payload（`docs/32`）**都补不了这个洞** —— 它们填的是层的实现，不是层的名字。

要加 Android，至少要动 mcpp 引擎的四处：

| 位置 | 内容 |
|---|---|
| `triple.cppm:739-785` | 解析器识别 `android` / `androideabi` / `wasi` 段 |
| `triple.cppm:158-186` | 对象格式映射（`elf64`/`macho64`/`win64` → 需要 android=elf、wasm=wasm） |
| `triple.cppm:104-120` | LLVM 三元组渲染 |
| `triple.cppm:645-663` | 工具链 / sysroot 解析（NDK 的 sysroot 模型与 glibc/musl 都不同） |

**但即便补齐 triple，问题只解决了一半，而且这一半 CMake 也没解决。** 实测本仓库的集成形状：

- Android：`platform/android/huxerui/build.gradle:32,62` 用 `externalNativeBuild { cmake { } }` —— **Gradle 是驱动方，CMake 只是被调用的 C++ 库生产者**。26 个 Java 文件、JNI 绑定、AAR 打包全在 Gradle 手里。
- iOS：`platform/ios/example_runner/HuxerUIExamples.xcodeproj` —— Xcode 工程 + storyboard + plist + 签名。
- Web：`platform/web/example.html.in` + JS 胶水 + `--preload-file` 资源预载。

所以真正的研究问题是**两段独立的**：

1. **上游**：mcpp 引擎补 android / ios / wasm32 三元组 —— 这是给 mcpp 提 PR，不是 HuxerUI 侧的配置工作。
2. **集成**：Gradle / Xcode 调用 `mcpp build` 而非 `cmake` 的可行性。这在原理上**是可行的**（Gradle 可以调任意外部构建；CMake 在这里也只是"被调用者"），但要重做 ABI/变体矩阵（`ANDROID_ABI` × build type）、`app.json` 集成计划的产出、以及 `HUXERUI_ANDROID_APP_INTEGRATION_ROOT` 那套按变体分目录的约定。

**结论修订**：不是"永久由 CMake 独占"，而是"**在 mcpp 上游补齐三元组之前无法开始，补齐之后仍有一段独立的集成工作**"。在此之前这三个平台由 CMake 独占，是事实约束而非架构选择。

### 3.2 测试模型不匹配

mcpp 的契约（`docs/08-testing.md`）：

> Every `tests/**/*.cpp` is a test: mcpp compiles each one into its own program and runs it. A test passes when its program exits zero.

HuxerUI 的 `tests/CMakeLists.txt` 用 `huxerui_add_test_suite()` 把 `tests_main.cpp` + 多个 `.cpp` 链接成**一个** Catch2 二进制，按 `tests/{unit,runtime,platform,codegen,resource_compiler,cli,cmake,scripts}` 分组，105 个源文件。

直接把 `tests/` 交给 `mcpp test` 会产生 105 个各自缺少 Catch2 `main` 的程序。

**可选解法**（按推荐度）：

1. **不让 mcpp 接管测试。** 测试继续 100% 由 CMake+CTest 拥有。mcpp 侧只做 `mcpp build`。这符合"不影响 CMake 体系"，且测试是 CMake 侧最成熟的资产。**推荐。**
2. 在 mcpp 包中用 `[build] sources` 显式排除 `tests/`，另建一个 workspace member 承载少量 mcpp 专属冒烟测试（例如"用 mcpp 构建出的库能链接并跑起一个窗口"）。
3. 重构 Catch2 套件为单文件自注册程序——代价高、收益低，不推荐。

### 3.3 双体系漂移（最大的长期成本）

两套构建各自维护同一份事实，会在以下位置漂移：

| 事实 | CMake 中的位置 | mcpp 中的位置 |
|---|---|---|
| 平台源清单 | `cmake/platform/Linux.cmake` 显式 10 项 | `[target.'cfg(linux)'.build] sources` |
| 平台链接库 | `HUXERUI_PLATFORM_LINK_LIBRARIES` | `[target.windows.runtime] libraries` / `ldflags` |
| 编译宏 | `HUXERUI_PLATFORM_COMPILE_DEFINITIONS` | `[target.*.build] defines` |
| 警告等级 | `huxerui_configure_compile_target()` | `[build] cxxflags` |
| C++ 标准 | `set(CMAKE_CXX_STANDARD 20)` | `[package] standard = "c++20"` |

新增一个 `platform/windows/win32_foo.cpp` 时，CMake 侧改一行、mcpp 侧改一行——**忘记后者，Windows 的 mcpp 构建会以 `undefined reference` 失败，且失败点离原因很远。**

这是本报告的核心工程建议所在，解法见 §5.4。

---

## 4. 关于几个模型差异的判断

### 4.1 mcpp 不需要 `HUXERUI_LIBRARY_GRAPH_ONLY` 那类反射机制吗

HuxerUI 已经有一个"把构建图导出为数据"的机制：`HUXERUI_LIBRARY_GRAPH_ONLY=ON` + `HUXERUI_LIBRARY_GRAPH_OUTPUT`（`cmake/HuxerUILibraries.cmake` 的 `_huxerui_write_library_graph`），`enable_language(NONE)` 下纯配置即可产出 JSON。

**这是双体系防漂移的最佳杠杆**，因为它已经存在且已被 `tests/cmake/` 覆盖。见 §5.4。

### 4.2 静态/动态库模型

CMake：作者建两个 target（`huxerui` SHARED + `huxerui_static` STATIC），消费者选一个链接。

mcpp：`kind = "lib"` 一个包，消费者写 `[build] dependency_linkage = "shared"` 或 `"static"`；`kind = "shared"` 只在"进程中必须只有一份"（会被 `dlopen` 的场景）时使用。

mcpp 的模型更正确（"是否独立文件是被构建程序的属性，不是库作者的属性"），且**不需要 HuxerUI 做任何取舍**——mcpp 包声明 `kind = "lib"` 即可，两种形态都能产出。

注意约束：musl 目标默认静态链接 C 库，此时 `dependency_linkage = "shared"` 会被拒绝。HuxerUI 的 Linux release 是 glibc（GLIBC 2.38 上限），不受影响。

---

## 5. 推荐架构：三层双构建

```
┌─────────────────────────────────────────────────────────────┐
│ L3  应用层     examples/ 36 个 · 用户 App                    │
│     CMake: huxerui_add_app()      mcpp: mcpp.toml + build.mcpp│
│     ← 两者都消费 L2 产出的库，互不知晓                        │
├─────────────────────────────────────────────────────────────┤
│ L2  框架库     huxerui / huxerui_static                      │
│     CMake: 6 平台全覆盖（唯一能构建 Android/iOS/Web 的路径）  │
│     mcpp:  Linux / Windows / macOS                           │
├─────────────────────────────────────────────────────────────┤
│ L1  宿主工具   hcg · hrc （tools/prebuilt 预编译二进制）      │
│     ← 两套构建共用同一批二进制，天然无漂移                    │
├─────────────────────────────────────────────────────────────┤
│ L0  单一事实源  src/ · include/ · platform/ · resources/      │
│     ← 一份源码。重叠的构建元数据由 §5.4 机器校验              │
└─────────────────────────────────────────────────────────────┘
```

关键性质一：**L1 是两套体系天然共享的**。`hcg`/`hrc` 接口是命令行（`--input/--output`、`--root/--output/--namespace`），零第三方依赖，与构建系统无关。HuxerUI 最特殊的两条构建规则（composable 变换、资源编译）在 mcpp 侧**不需要重新实现，只需要重新调度**。

关键性质二（第二版更正）：**L2 与 L3 之间不存在难度台阶，方向反而是反的。**

| | 框架库（L2） | 应用（L3） |
|---|---|---|
| 平台条件源 | 需要 | 不需要 |
| 平台系统库 | 需要 | **由 L2 的依赖边自动提供** |
| `hrc` 资源编译 | 需要（仅 `resources/` 内置包） | 需要（应用资源 + staging） |
| `hcg` composable 变换 | **不需要**（`src/`+`platform/` 命中 0） | 需要 |
| SDK 发现 / 链接接口翻译 | 不存在这个问题 | **L2 成为包之后消失** |
| bundle / app.json / 安装器 | 不需要 | 需要 |

在 mcpp 的模型里 L2 和 L3 都只是"包"，差别仅在 `kind`。把 L3 排在前面做，等于在没有包的情况下手写包管理 —— 这正是今天 `examples/mcpp_demo` 那 20 行 `/lib64/*.so` 的由来。

### 5.1 框架包优先：让包管理去做包管理的事 ★（第二版重写）

初版在这里设计了一个 `huxerui.rules-app` 规则包，让它去做 **SDK 发现**（读 `HUXERUI_HOME` / 探测 `huxerui` 可执行文件旁的布局）和 **平台链接接口翻译**（Linux pkg-config、Windows 18 个 lib、macOS 14 个 framework）。

**那是在手工重造包管理。** 它之所以看起来必要，只是因为 HuxerUI 当时不是一个 mcpp 包。一旦它是，这两块全部消失：

| 问题 | 框架**不是** mcpp 包时 | 框架**是** mcpp 包时 |
|---|---|---|
| 应用怎么找到 SDK | rules 包读 `HUXERUI_HOME` / 走四步发现顺序 / 猜编译器 provenance | `[dependencies] huxerui = "0.3.0"` |
| GTK4 / epoxy / gio / libsoup 怎么进链接行 | 应用手写 `/lib64/*.so` 绝对路径（今天 `mcpp_demo` 就是这样） | 框架包 `build.mcpp` 的 pkg-config 探测发射 `link-lib`/`link-search`，**直达最终链接** |
| Windows 18 个系统库 | 应用侧重复一遍 | 框架包 `[target.windows.runtime] libraries`，随依赖边传递 |
| macOS 14 个 framework | 应用侧重复一遍 | 框架包 `[runtime] frameworks` |
| `hcg` / `hrc` 从哪来 | 应用要知道 `tools/prebuilt/<host>/<arch>/` 布局 | `tools = ["hcg", "hrc"], reexport = true` → `mcpp::dep_bin("huxerui", "hcg")` |
| composable / 资源怎么调度 | 每个应用的 `build.mcpp` 复制粘贴 | `host-module = true, reexport = true` → `import huxerui.rules;` |
| ABI 一致性 | 靠文档纪律（`skills/.../mcpp-build.md` 整篇都在讲这个） | 由构建图保证：同一次 build，同一 toolchain、同一 `standard` |

**依据**（`mcpp/docs/30-build-mcpp.md:835-845`）：

> A dependency that ships a `build.mcpp` gets it compiled and run too … `cxxflag`/`cflag`/`cfg` directives color **only that package's own TUs**; **`link-lib`/`link-search` reach the final link**.

这一句就消掉了整个"翻译 CMake 链接接口"的命题：框架包自己探测出来的链接行，天然是应用的链接行。

#### 框架包的形状

```toml
# 根 mcpp.toml —— 框架 + 两个宿主工具，一个 workspace
[workspace]
members = ["tools/codegen", "tools/resource_compiler"]

[package]
name = "huxerui"
# ... 见 §9.1

# hcg / hrc 是零第三方依赖的独立 C++20 程序（只用标准库，不链接 HuxerUI），
# 因此可以直接作为 bin target 由 mcpp 为宿主构建、全局缓存。
[dependencies]
huxerui-tools = { path = "tools", tools = ["hcg", "hrc"], reexport = true }
huxerui-build-rules = { path = "tools/mcpp-rules", host-module = true, reexport = true }
```

`reexport = true` 的语义正是所需（`docs/30:1103-1140`）：

> hands an edge's build-time provisions — its `tools`, its `host-module`, and the dependency's directory — to **this package's own consumers**.

#### 应用侧于是收敛成这样

```toml
# 用户 App 的 mcpp.toml —— 全文
[package]
name     = "my-app"
version  = "0.1.0"
standard = "c++20"

[dependencies]
huxerui = "0.3.0"

[targets.my-app]
kind = "bin"
main = "src/main.cpp"
```

```cpp
// build.mcpp —— 全文
import mcpp;
import huxerui.rules;
int main() { return huxerui::rules::configure({.resources = "resources"}) ? 0 : 1; }
```

对比今天 `examples/mcpp_demo/mcpp.toml` 的 20 行 `/lib64/*.so` 绝对路径 + `[runtime] library_dirs = ["/lib64"]`。**这就是"应用侧几乎白拿"的含义**：应用侧的产品化不是一项独立工作，而是框架包做完之后的推论。

#### 一个仍需实测确认的点

`tools = [...]` 要求"Each name must be a `kind = "bin"` target of that package"，即 mcpp **从源码构建**该工具。HuxerUI 现在分发的是 `tools/prebuilt/` 的预编译二进制。两条路：

- **(a) 源码构建**（推荐）：`hcg`/`hrc` 无第三方依赖，mcpp 为宿主构建一次、按"包版本 × 宿主工具链 × features × 依赖闭包"全局缓存。CMake 侧继续用 `tools/prebuilt/`，互不干扰。
- **(b) 覆盖**：`[tools.overrides]` 或 `MCPP_TOOL_HUXERUI_HCG=<path>` 指向已有的 prebuilt 二进制，**跳过构建**。CI / 发行版打包场景的逃生口。

注意 `docs/30:940-948` 记录的一个已知 gap：`path` 依赖的工具缓存键**不含源码内容**，改了工具源码但没升版本时，缓存的旧二进制会继续被使用。开发期需 `mcpp cache clean` 或升版本。

### 5.2 `hcg` 的 mcpp 映射（契合度最高的一块）

`hcg` 的接口是 `--input <path> --output <path>`，`tools/codegen/main.cpp` **不检查扩展名**（扩展名过滤只存在于 `cmake/HuxerUICodegen.cmake`）。它是纯文本变换。

映射到 mcpp：

```cpp
// build.mcpp 片段
for (auto const& src : composable_sources) {
    const std::string out = std::string(mcpp::out_dir()) + "/hcg/" + stem(src) + ".cpp";
    mcpp::action a;
    a.id          = "hcg:" + stem(src);
    a.role        = "source";                    // 产物进入编译集
    a.description = "huxerui composable " + src;
    a.arg(mcpp::dep_bin("huxerui", "hcg"))       // 或绝对路径
     .arg("--input").arg(src.c_str())
     .arg("--output").arg(out.c_str())
     .input(src.c_str())
     .output(out.c_str())
     .submit();
}
```

`role = "source"` 的语义正是所需：产物加入编译集，且**声明包的每条编译边都等待它们**。相比 CMake 侧靠 `cmake_language(DEFER)` + 重写 `SOURCES` 属性实现同样效果，mcpp 的表达更直接。

一个需要处理的细节：CMake 侧只对**含 `[[huxerui::composable]]` 或 `Use` 字样**的源文件安排变换（`file(READ)` + `string(FIND)` 预筛）。mcpp 侧同样应在 `build.mcpp` 里做这个预筛，避免为 67 个核心源全部生成变换边。

### 5.3 Linux 平台依赖的两条路

**路线 A：`build.mcpp` 调 pkg-config（推荐用于框架库）**

```cpp
import std;
import mcpp;

bool probe(std::string_view mod) {
    // popen("pkg-config --cflags --libs " + mod)，解析后逐条发射
    // -I<dir>   -> mcpp::include_dir(dir)
    // -l<name>  -> mcpp::link_lib(name)
    // -L<dir>   -> mcpp::link_search(dir)
    // 其余      -> mcpp::cxxflag(flag)
}

int main() {
    mcpp::rerun_if_env_changed("PKG_CONFIG_PATH");
    for (auto m : {"gtk4", "epoxy", "gio-2.0", "libsoup-3.0"})
        if (!probe(m)) return 1;
}
```

**优点**：与 CMake 的 `pkg_check_modules` **读同一份 `.pc` 文件**——两套构建的 Linux 链接行由构造保证一致，这是唯一不会漂移的一类元数据。符合 AGENTS.md "GTK 4、libepoxy、GIO、libsoup 3 通过 pkg-config 解析，保持发行版拥有"的既定策略。

**路线 B：`[xlings.workspace]` 载荷**

```toml
[target.'cfg(linux)'.xlings.workspace]
"xim:gtk4"     = "latest"   # 生产中应 pin 具体版本
"xim:libepoxy" = "latest"
"xim:libsoup"  = "latest"
```

配合 `mcpp::xpkg_dir("xim", "gtk4")` 定位。**优点**：可复现、与机器无关、CI 上无需 `apt install`。**缺点**：偏离 AGENTS.md "发行版拥有依赖"的策略，且引入与 CMake 侧不同的 GTK 版本 → ABI 分歧风险。

**建议**：框架库用 A（与 CMake 同源）；若将来需要一个自包含的 CI 镜像或跨机器可复现的发布构建，再引入 B 作为可选 feature。

### 5.4 防漂移：把重叠做成机器校验 ★

这是本报告最重要的一条建议。

复用**已存在**的 `HUXERUI_LIBRARY_GRAPH_ONLY` 机制，新增一个 parity 测试（放在 `tests/cmake/` 或新建 `tests/build_parity/`）：

```
1. cmake -S . -B <tmp> -DHUXERUI_LIBRARY_GRAPH_ONLY=ON \
        -DHUXERUI_LIBRARY_GRAPH_OUTPUT=graph.json     (LANGUAGES NONE, 秒级)
2. 解析 mcpp.toml 的 [build] sources + [target.*.build] sources
3. 断言：当前宿主平台的源集合、平台链接库集合、编译宏集合两侧相等
4. 不等 → 测试失败，错误信息指名缺失的文件与应改的那一行
```

同时在 CI 增加**一个** job：Linux 上 `mcpp build`。一个 job 即可覆盖绝大多数漂移（新增源文件、改宏、改警告等级），代价约等于一次普通构建。

**为什么这是关键**：双构建体系的成本不在初次搭建，而在此后每一次改动的纪律要求。文档约定会被遗忘，测试不会。

---

## 6. C++20 Modules / `import` 路径

### 6.1 现状与前置条件

HuxerUI 当前 **0 个模块**。但 AGENTS.md 对公共头的既有要求恰好是模块化的理想前置条件：

- 每个公共头 `#pragma once`、**能被单独包含即编译**、直接包含其声明所需的全部依赖
- 不依赖 `<huxerui/huxerui.h>`、不依赖传递包含、不依赖私有头
- 公共声明在 `huxerui`，内部在 `huxerui::detail`
- **无 `using namespace` 指令**

这四条使"头文件 → 全局模块片段（GMF）"的封装几乎无摩擦。

### 6.2 三种策略对比

| 策略 | 做法 | 对 CMake 体系的影响 | 对消费者的影响 | 判定 |
|---|---|---|---|---|
| **A. 原生模块化** | `include/huxerui/*.h` → `.cppm` 分区，`export module huxerui;` | **破坏性**：SDK 不再是头文件包；`tests/support/header_checks.cmake` 全部失效；install/export 契约重写 | 破坏性：所有现有代码必须改 | ❌ 与约束冲突 |
| **B. 头单元（header units）** | `import <huxerui/huxerui.h>;` | 小 | 小 | ❌ GCC/Clang 支持不成熟，6 平台不可行 |
| **C. 模块外壳（wrapper）** | 新增 `modules/huxerui.cppm`：GMF 里 `#include` 现有公共头，然后 `export using` 公共符号 | **零**（新增文件，现有 target 不变） | 零（`#include` 继续工作，`import` 是**额外**入口） | ✅ **推荐** |

策略 C 的形状：

```cpp
// modules/huxerui.cppm  —— 与现有头共存，不替代
module;
#include <huxerui/huxerui.h>
export module huxerui;

export namespace huxerui {
    using huxerui::View;
    using huxerui::State;
    using huxerui::UseState;
    using huxerui::Column;
    // ... 约 900 个公共实体
}
```

**关键性质**：库的二进制形态、ABI、链接接口完全不变。`.cppm` 是消费者侧编译的一层薄封装，**不随 SDK 分发 BMI**。这一点至关重要——BMI 与标准等级、工具链版本强绑定（mcpp 文档：`标准是模块图全局的`、`不同等级永不共享缓存`、GCC 对跨等级 BMI 报 `language dialect differs`），而 HuxerUI 的 Linux release SDK 是 gcc-14 / C++20 构建的。分发 BMI 会把 SDK 的适用范围从"任何 ABI 兼容的编译器"缩到"完全同一个编译器同一个标准等级"。**分发 `.cppm` 源，不分发 BMI。**

约 900 个 `export using` 应当**生成而非手写**——HuxerUI 已有代码生成工具文化（`hcg`），再加一个从公共头提取导出清单的小工具，并把生成结果纳入 §5.4 式的 parity 校验（新增公共符号未出现在模块外壳 → 测试失败）。

### 6.3 ★ 阻塞点：宏不跨模块边界

**这是一个当前会直接编译失败的具体问题，而非理论顾虑。**

`hcg` 为每个 `[[huxerui::composable]]` 函数注入（`tools/codegen/transform.cpp:858-870`）：

```cpp
edits.push_back({composable.opening_brace + 1, 0, "\n  HUXERUI_SCOPE_BEGIN\n" + ...});
edits.push_back({composable.closing_brace,     0, "\n  HUXERUI_SCOPE_END\n"   + ...});
```

而 `HUXERUI_SCOPE_BEGIN` / `HUXERUI_SCOPE_END` 是**预处理器宏**，定义在 `include/huxerui/view.h:1877-1885`：

```cpp
#define HUXERUI_SCOPE_BEGIN \
  return ::huxerui::Scope([=]() -> ::huxerui::View {
#define HUXERUI_SCOPE_END \
  });
```

**宏不通过模块导出。** 一个只写 `import huxerui;` 的应用，其 `hcg` 生成的代码里 `HUXERUI_SCOPE_BEGIN` 是未声明标识符 → 编译失败。这使 modules 路线与 HuxerUI 最核心的语言特性（composable）直接冲突。

**修复（推荐）**：让 `hcg` 直接注入宏的展开式，而非宏名：

```cpp
// 注入 "\n  return ::huxerui::Scope([=]() -> ::huxerui::View {\n"
// 与   "\n  });\n"
```

- 改动量：`tools/codegen/transform.cpp:862` 与 `:869` 两个字符串字面量。
- **不要动检测路径**：`kScopeBegin`/`kScopeEnd`（`transform.cpp:16-17`）还被用于识别**输入源中手写的**显式作用域——`transform.cpp:357`（"composable function already contains an explicit HuxerUI scope"）与 `:561-584`。`HUXERUI_SCOPE` 系列仍是公开 API，手写代码继续使用，因此这些常量与检测逻辑必须保留。改的只是"注入什么"，不是"识别什么"。
- 效果：生成代码变为**无宏**，在 `#include` 与 `import` 下行为完全一致。同时消除了"生成代码隐式依赖一个公共宏"这一耦合——这本身就是一个独立的设计改进。
- 代价：生成输出变化 → 按 AGENTS.md「Codegen: transform、生成的 Runtime 行为、common build、必需宿主工具一起更新」，需同步更新 `tests/codegen/`、重新构建并提交 `tools/prebuilt/<6 个 host×arch>/hcg`。
- 三个宏本身应**保留**（`HUXERUI_SCOPE` 是公开 API，手写代码在用）。

**备选**：随 SDK 提供一个 `huxerui_prelude.h`，`import huxerui;` 的用户额外 `#include` 它取得宏。零风险但把丑陋留给了用户。

### 6.4 CMake 侧的对等支持

双体系要求 modules 在两侧都可用，CMake 侧的约束是硬性的：

| 约束 | 状态 |
|---|---|
| CMake ≥ 3.28（`FILE_SET CXX_MODULES`） | 本机 3.28.3 恰好达线；但 `cmake_minimum_required(VERSION 3.20)` 需为模块 target 单独抬到 3.28 |
| 生成器 | **仅 Ninja ≥ 1.11 / Ninja Multi-Config / VS 2022**。**Makefiles 生成器不支持 C++20 modules** |
| Xcode 生成器 | 不支持 → iOS/macOS 的 Xcode 路径无法用模块 |
| Emscripten / Android NDK clang | 模块支持不成熟 |

**建议**：模块外壳 target 在 CMake 侧**按版本与生成器门控**（`if (CMAKE_VERSION VERSION_GREATER_EQUAL 3.28 AND CMAKE_GENERATOR MATCHES "Ninja")`），默认 `OFF`。基础构建保持 `cmake_minimum_required(VERSION 3.20)` 不变——**这是"零影响"的具体保证**。

### 6.5 `import std` 与标准等级

- HuxerUI SDK ABI 基线是 **C++20**，不应改变。
- `import std;` 在 C++20 下可用（GCC ≥ 15、Clang+libc++ ≥ 17、MSVC STL ≥ VS 17.8 均在 C++20 模式提供 `std` 模块），但 C++23 库设施（`std::print`、`std::expected`）不可用。
- 框架自身**不应**使用 `import std;`——它会把"必须有 `std` 模块的工具链"变成 SDK 的硬约束。
- **应用侧可以**：`mcpp.toml` 里 `standard = "c++23"` + `import std;` + `import huxerui;`，链接的仍是 C++20 构建的库。这是安全的（`-std` 等级不改变 Itanium/MSVC ABI），也正是 `examples/mcpp_demo` 已经验证过的形态（该 demo 用 `c++26` 和 `c++23` 均编译通过）。
- **`skills/.../mcpp-build.md` 的既有告诫在此完全适用**：更新的 `-std` 不是 ABI 兼容方案；不得跨边界混用 libstdc++/libc++、MSVC/MinGW。

### 6.6 modules 的收益落点

不要期待框架内部编译提速——策略 C 下框架仍是头文件编译。真实收益在**应用侧**：

- 36 个 `examples/` 每个都 `#include <huxerui/huxerui.h>`（拉入 55 个公共头，约 900 个实体）。换成 `import huxerui;` 后，该前端解析在整个构建中只发生一次。
- 这也正是 mcpp 的强项所在（BMI 缓存、`bmi_schedule`、跨项目 BMI 缓存），因此 **modules 落地应当先在 mcpp 侧做，CMake 侧跟进**——与 §5 的分层一致。

---

## 7. 分阶段路线图（第二版重排）

| 阶段 | 产出 | 对 CMake 的影响 | 验收标准 |
|---|---|---|---|
| **P0 基线固化** | parity 测试骨架（§5.4）；`examples/mcpp_demo` 进 CI（Linux） | 零（新增测试） | mcpp_demo 在 CI 上可重复构建 |
| **P1 框架包 · Linux** ★ | 根 `mcpp.toml`；`build.mcpp`（pkg-config 探测 + `hrc` 内置资源）；`tools/{codegen,resource_compiler}` 成为 workspace 的 `kind = "bin"` member；parity 测试上线 | 零 | `mcpp build` 产出 `libhuxerui.a`；parity 测试通过；`mcpp` 侧不再依赖 `tools/prebuilt/` |
| **P2 规则包 + 应用侧** | `huxerui.rules`（`host-module`）调度 `hcg`/`hrc`/staging/app.json；`tools`+`host-module` 打上 `reexport = true` | 零（新增目录） | 一个用 composable + 资源的示例应用，`mcpp.toml` 里**只有** `huxerui = "0.3.0"` 一行依赖；`mcpp_demo` 的 20 行绝对路径删除 |
| **P3 Windows / macOS** | `[target.windows.*]` / `[target.macos.*]`；`.mm` + `-fobjc-arc` 验证 | 零 | 三平台 `mcpp build` 通过；`otool -L` / 依赖检查符合 release 策略 |
| **P4 发布到 index** | `mcpp publish`；版本与 SemVer 策略；`[runtime]` artifacts | 零 | 一台干净机器上 `mcpp new` + `huxerui = "0.3.0"` 即可构建运行 |
| **P5 modules 外壳** | `hcg` 去宏化（§6.3）+ 测试与 prebuilt 更新；导出清单生成器；`modules/huxerui.cppm`；CMake 侧门控 target | **仅 §6.3 一处**：`transform.cpp:862,869` + codegen 测试 + 6 份 prebuilt 二进制 | `import huxerui;` 在 mcpp 与 CMake(Ninja) 下均可构建；`#include` 路径行为不变 |
| **待研究** | Android / iOS / Web：(1) 给 mcpp 上游提三元组 PR；(2) Gradle/Xcode 调用 mcpp 的集成可行性 | — | 见 §3.1；两段工作互相独立，(1) 是 (2) 的前提 |

**P1 与初版的 P1 互换了位置，理由在 §0 修订说明与 §5.1。** P2 的工作量因为 P1 的存在而大幅缩小 —— 它不再包含 SDK 发现与链接接口翻译，只剩 `hcg`/`hrc`/staging 的调度。

## 8. 对 CMake 体系的零影响论证

逐项核对"不影响 CMake 构建体系"这一约束：

| 变更 | 位置 | 是否触及现有 CMake |
|---|---|---|
| 根 `mcpp.toml` | 仓库根新增文件 | 否。CMake 不读 `.toml` |
| `build.mcpp` | 仓库根新增文件 | 否。扩展名 `.mcpp` 不匹配任何 CMake glob（现有 glob 为 `src/*/*.cpp`、`resources/*`） |
| `modules/huxerui.cppm` | 新增目录 | 否。`file(GLOB src/*/*.cpp)` 不匹配 `modules/` |
| rule package | 新增目录（如 `tools/mcpp-rules/`） | 否 |
| parity 测试 | `tests/` 新增 | 是——但只**新增** `add_subdirectory` / 测试注册，不改现有测试 |
| **`hcg` 去宏化** | `tools/codegen/transform.cpp:862,869` | **是**。这是唯一一处实质改动，且仅在 P4 需要 |
| `.gitignore` | 需忽略 mcpp 的 `target/`、`mcpp.lock`（视策略） | 否 |

**唯一需要留意的交互**：`target/` 目录。mcpp 默认输出到 `target/<triple>/<fingerprint>/`。HuxerUI 的 CMake 输出在 `build/`。两者不冲突，但 `.gitignore` 需增加 `target/`。另需确认 `file(GLOB_RECURSE resources/*)`、`CONFIGURE_DEPENDS` 等不会扫到 mcpp 产物——按现有 glob 模式（`src/*/*.cpp`、`resources/*`）不会。

---

## 9. 代码骨架

### 9.1 根 `mcpp.toml`（框架库，草案）

```toml
[package]
name        = "huxerui"
namespace   = "huxerui"
version     = "0.3.0"
standard    = "c++20"                    # SDK ABI 基线，不要提高
description = "HuxerUI declarative cross-platform UI framework"
platforms   = ["linux", "windows", "macos"]   # 显式声明覆盖面

[targets.huxerui]
kind = "lib"                             # 静/动态形态由消费者 dependency_linkage 决定

[build]
sources              = ["src/*/*.cpp"]   # 对齐 CMake 的 file(GLOB src/*/*.cpp)
include_dirs         = ["include", "src"]
private_include_dirs = ["src"]           # src 是私有实现，不传递给消费者
cxxflags             = ["-Wall", "-Wextra", "-Wpedantic"]

[features]
profiling = []                           # ≙ HUXERUI_ENABLE_PROFILING

# ---------------- Linux ----------------
[target.'cfg(linux)'.build]
sources = ["platform/linux/*.cpp"]
# GTK4 / epoxy / gio / libsoup 由 build.mcpp 经 pkg-config 探测（§5.3 路线 A）

# ---------------- Windows ----------------
[target.windows.build]
sources  = ["platform/windows/*.cpp"]
defines  = ["UNICODE", "_UNICODE", "NOMINMAX", "WIN32_LEAN_AND_MEAN",
            "WINVER=0x0A00", "_WIN32_WINNT=0x0A00"]
cxxflags = ["/W4", "/permissive-", "/utf-8"]

[target.windows.runtime]
libraries = ["advapi32", "d2d1", "d3d11", "dwrite", "dwmapi", "dxguid", "dxgi",
             "imm32", "ole32", "oleaut32", "psapi", "shell32", "uiautomationcore",
             "user32", "winhttp", "windowscodecs",
             "dcomp", "windowsapp", "crypt32", "propsys"]

# ---------------- macOS ----------------
[target.macos.build]
sources  = ["platform/macos/*.mm"]       # 注意：.mm 不在默认 glob 中，必须显式列出
cxxflags = ["-fobjc-arc"]
ldflags  = ["-weak_framework", "UniformTypeIdentifiers"]

[build]
macos_deployment_target = "12.0"

[runtime]
frameworks = ["AppKit", "AVFoundation", "Carbon", "CoreGraphics", "CoreImage",
              "CoreText", "CoreVideo", "ImageIO", "Foundation", "Metal",
              "MetalPerformanceShaders", "QuartzCore", "UserNotifications"]
```

> 注：`[build]` 出现两次是为了行文分组，实际 TOML 中须合并为一个表。

### 9.2 `build.mcpp`（框架库，草案）

```cpp
import std;
import mcpp;

// ── Linux 平台依赖：与 CMake 的 pkg_check_modules 读同一份 .pc ──────────
bool probe_pkgconfig(std::string_view mod);   // 见 §5.3

// ── hcg：为含 composable 标记的源声明变换边 ──────────────────────────
void schedule_codegen(std::span<const std::string> sources);

// ── hrc：内置资源 ────────────────────────────────────────────────────
void schedule_builtin_resources();

int main() {
    mcpp::rerun_if_changed_glob("platform/**/*.cpp");
    mcpp::rerun_if_changed_glob("platform/**/*.mm");
    mcpp::rerun_if_changed_glob("resources/**");

#if defined(__linux__)
    mcpp::rerun_if_env_changed("PKG_CONFIG_PATH");
    for (auto m : {"gtk4", "epoxy", "gio-2.0", "libsoup-3.0"})
        if (!probe_pkgconfig(m)) {
            std::println(std::cerr, "huxerui: pkg-config module not found: {}", m);
            return 1;
        }
#endif

    schedule_builtin_resources();
    schedule_codegen(/* 预筛出含 [[huxerui::composable]] / Use 的源 */);
    return 0;
}
```

### 9.3 `hrc` 的 action 形状

```cpp
// 内置资源：一条 source 边产出 头 + bin
mcpp::action r;
r.id   = "hrc:builtin";
r.role = "source";                        // 头进入编译集；bin 被产出但不编译
r.arg(hrc).arg("--root").arg("resources")
          .arg("--output").arg(out_dir)
          .arg("--namespace").arg("huxerui")
          .arg("--header-name").arg("huxerui_builtin_resources.h")
   .output((out_dir + "/include/huxerui_builtin_resources.h").c_str())
   .output((out_dir + "/package/huxerui/resources.bin").c_str())
   .submit();
mcpp::include_dir(out_dir + "/include");   // 私有 -I，不传递给消费者

// staging：输入是链接产物，故自动排在链接之后
mcpp::action s;
s.id   = "hrc:stage";
s.role = "artifact";
// ... 把 package/ 复制到 ${mcpp.target_file:huxerui} 旁的 <name>.resources/
```

> `role = "source"` 且输出全为头文件时，mcpp 2026.8.30.2+ 会为声明包的编译边加 order-only 边，确保头在编译前存在。本仓库输出含头 + bin，落在同一规则内。
>
> `input()` 在 `build.mcpp` 运行时固定边的输入。`hrc` 递归读取资源根，其真实输入集合无法在此刻完整列出——应使用 `rerun_if_changed_glob("resources/**")`（重跑 build.mcpp）配合把资源文件逐一 `input()`，或让 `hrc` 输出 depfile 并设置 `a.depfile`。**这一点在实现时需要实测确认增量行为正确**。

---

## 10. 待决策问题

1. ~~**mcpp 侧的定位**：一等公民 vs 应用侧入口？~~ **已在第二版中回答：一等公民的第二套构建，且框架包优先。** "框架侧只是 dogfooding"的判断是错的——框架包是应用侧能变好的**前提**，不是它的副产品。剩下的定位问题只有一个：**是否发布到 index**（P4），这决定 mcpp 侧是"仓库内可用"还是"外部用户可用"。
2. **`hcg` 去宏化（§6.3）** 是否接受？它是 modules 路线的前置，且本身是设计改进，但要求重新生成并提交 6 份 prebuilt 二进制。
3. **Linux 依赖策略**：坚持 pkg-config（与 CMake 同源、无漂移）还是引入 `[xlings.workspace]` 载荷（可复现、CI 友好）？建议先 pkg-config。**注意这个选择在 P4 会再次浮现**：发布到 index 之后，外部用户的机器上未必有 GTK4 开发包，届时 xlings 载荷（`xim:gtk4` 等已在索引中）会从"可选"变成"发布路径的默认答案"。
4. **模块外壳的导出清单**：手写（约 900 个 `export using`，会漂移）还是生成（新增一个工具，进 parity 校验）？建议生成。
5. **测试归属**：确认测试 100% 留在 CMake+CTest（本报告的建议），还是要为 mcpp 建独立的冒烟测试集？
6. **版本与发布**：mcpp 侧是否需要产出可发布的包（`mcpp pack` / 发到 index），还是仅供源码构建？这影响是否需要处理 `[runtime]` artifacts 与 SBOM。
7. **`hcg`/`hrc` 的供给方式**：mcpp 侧从源码构建为 `kind = "bin"` target（§5.1 路线 a，推荐），还是 `[tools.overrides]` 覆盖到现有 `tools/prebuilt/`（路线 b）？注意路线 a 下 mcpp 侧不再需要 `tools/prebuilt/`，但 CMake 侧仍需要——两者并存不冲突。
8. **Android / iOS / Web 是否要推上游**：给 mcpp 提三元组 PR 是一项独立投入（§3.1 列了引擎四处改动点）。要做的话，值得先和 mcpp 上游确认这是否在其路线图内，避免 PR 与其目标冲突。

---

## 附：关键事实索引

| 事实 | 出处 |
|---|---|
| mcpp target 表无 android/ios/wasm | `mcpp/docs/21-the-target-triple.md:424-446` |
| `.mm`/`.m` 为一等源类型 | `mcpp/modules/source-kind/src/source_kind.cppm:253-254` |
| 默认源 glob 不含 `.mm` | `mcpp/docs/04-mcpp-toml.md` §2.3 |
| `mcpp::action` 四种 role 语义 | `mcpp/docs/30-build-mcpp.md:433-470` |
| `[target.*]` 谓词词汇与 `runtime` 键 | `mcpp/docs/22-target-side.md:346-460` |
| 每个 `tests/**/*.cpp` 是一个独立测试程序 | `mcpp/docs/08-testing.md` |
| 标准等级是模块图全局的；BMI 不跨等级 | `mcpp/docs/04-mcpp-toml.md:78-88` |
| `hcg` 注入宏名而非展开式 | `tools/codegen/transform.cpp:858-870` |
| `HUXERUI_SCOPE_BEGIN/END` 宏定义 | `include/huxerui/view.h:1877-1885` |
| 手写显式作用域的检测路径（去宏化时须保留） | `tools/codegen/transform.cpp:16-17, 357, 561-584` |
| `hcg` CLI 不检查扩展名 | `tools/codegen/main.cpp:17-35` |
| CMake 侧扩展名过滤位置 | `cmake/HuxerUICodegen.cmake`（`.cpp/.cc/.cxx` 之外跳过） |
| 库图导出机制（防漂移杠杆） | `cmake/HuxerUILibraries.cmake:58` `_huxerui_write_library_graph` |
| 现有 mcpp 前端为纯转发 | `tools/huxerui_cli/mcpp/mcpp.cpp:92-110` |
| **框架核心不含 composable（命中 0）** | `grep -r 'huxerui::composable' src/ platform/` |
| **`enable_codegen` 只被消费者侧调用** | `cmake/HuxerUIApp.cmake:249`、`cmake/HuxerUILibraries.cmake:198` |
| **依赖的 build.mcpp 其 link-lib/link-search 直达最终链接** | `mcpp/docs/30-build-mcpp.md:835-845` |
| **`reexport = true` 把 tools / host-module 交给消费者** | `mcpp/docs/30-build-mcpp.md:1103-1140` |
| **`tools = [...]` 要求工具是该包的 bin target；`[tools.overrides]` 是逃生口** | `mcpp/docs/30-build-mcpp.md:899-968` |
| **path 依赖的工具缓存键不含源码内容（已知 gap）** | `mcpp/docs/30-build-mcpp.md:940-948` |
| **`hcg`/`hrc` 零第三方依赖、独立 project** | `tools/codegen/CMakeLists.txt`、`tools/resource_compiler/CMakeLists.txt` |
| **引擎显式拒绝 androideabi / wasi** | `mcpp/modules/toolchain-model/src/triple.cppm:783-785` |
| **os 是引擎内的闭集** | `mcpp/modules/toolchain-model/src/triple.cppm:34`（`// "linux" \| "macos" \| "windows"`） |
| **Gradle 通过 externalNativeBuild 驱动 CMake** | `platform/android/huxerui/build.gradle:32,62` |
| SDK ABI 纪律（编译器族/标准库/linkage） | `skills/huxerui-app-development/references/mcpp-build.md` |
