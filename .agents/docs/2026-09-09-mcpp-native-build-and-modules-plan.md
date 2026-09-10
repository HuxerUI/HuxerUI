# 2026-09-09 — mcpp 原生构建与 C++20 模块风格适配方案

> **状态**：已实现（PR #1，待 review）。实现过程中有三个决策与设计稿不同，见 §0。
> **依赖**：mcpp ≥ 2026.9.10.2（CI 钉的版本）；分析报告 `docs/design/mcpp-dual-build-analysis.md`
> **目标读者**：HuxerUI 维护者
> **约束**：CMake 构建体系不做结构性改动；mcpp 侧的一切新增物集中在可删除的新增路径下

> **实现与本文的差异**：本文是 2026-09-09 的方案稿。落地时 parity 检查与导出清单
> 生成器都改用 mcpp 构建的 C++ 程序（`mcpp/huxerui-tools`）而非 Python，规则包
> 定名 `huxerui-build-rules`，共享的扫描逻辑拆到 `huxerui-source-select` 并加了
> 单测。下文的路径已按实现更新，设计论证保持原样。


---

## 0. 实现过程中翻转的三个决策

设计稿是在只读分析的基础上写的；真正构建之后，有三处的答案变了。都记在这里，因为"当初为什么这么写"比结论本身更容易丢。

### 0.1 Linux 依赖：从 host pkg-config 改为 xlings payload

设计稿 §5.3 推荐用 host 的 pkg-config，理由是"与 CMake 读同一份 `.pc`，这类元数据由构造保证不漂移"。**这个理由是对的，但代价没算到。**

mcpp 是 payload-first 的：它用自己的工具链和自己的 glibc（`xim:glibc`）。`pkg-config --cflags gtk4` 在 Debian/Ubuntu 上会发 `-I/usr/include/x86_64-linux-gnu`——系统 glibc 的 multiarch 目录——于是 payload glibc 的 `<time.h>` 拉进了**系统的** `<bits/time.h>`：

```
.../xim-x-glibc/2.44/include/time.h:37 -> /usr/include/x86_64-linux-gnu/bits/time.h:73
error: 'time' has not been declared in '::'
error: 'struct timespec' has no member named 'tv_sec'
error: cannot convert '<brace-enclosed initializer list>' to 'unsigned int'
```

**每个核心 TU 都失败**，不只是平台文件，因为构建程序的 include 目录着色整个包。CI 上实测到这一组错误（run 34342551784）。

`-idirafter` 能让它重新编译过，但那只是治症状：**链接时仍然是"对着 glibc 2.39 编的库"和"对着 2.44 编的目标文件"拼在一起**。

所以整个 GTK 栈改为 `[target.'cfg(linux)'.xlings.workspace]` 声明的 36 个 payload，`build.mcpp` 把 `PKG_CONFIG_LIBDIR` 只指向这些 payload 的 `pkgconfig` 目录（`PKG_CONFIG_LIBDIR` 是**替换**默认搜索路径，不是 `PKG_CONFIG_PATH` 那样前插），host 的头和库一条也进不来。

CMake 侧继续用发行版的包，如 `cmake/platform/Linux.cmake` 与 AGENTS.md 要求的那样。**两套构建对"GTK 从哪来"给出不同答案是刻意的**——它们用不同的 C 库编译。

### 0.2 宿主工具：从 `tools/prebuilt/` 改为源码构建

设计稿 §2.2 倾向 B（直接用已提交的 prebuilt），理由是"两侧跑字节相同的二进制，零漂移"。**去宏化（§5.2）推翻了它**：改了 `transform.cpp` 之后，已提交的 prebuilt 按定义就是陈旧的，要等 `update-host-tools.yml` 在 main 上跑完才追上。

而且本机无法正确重建它：payload 工具链产出的二进制链接 glibc 2.44，在 CI 的 Ubuntu 24.04（glibc 2.39）上根本跑不起来——prebuilt 的构建策略（GLIBC 2.28 + 静态 libstdc++）由那条 workflow 拥有，不是随手能复现的。

改为 A：`tools/{codegen,resource_compiler}/mcpp.toml` 把两个工具声明为 `kind = "bin"` 包，框架用 `tools = [...]` + `reexport = true` 请求，mcpp 为宿主构建并全局缓存。这同时也更符合"mcpp 不依赖 host"——prebuilt 是为 host 的 C 库构建的。

**CMake 侧一行不改，继续用 `tools/prebuilt/`。**

### 0.3 C++20 模块：从 P5（另开 PR）改为本 PR 内完成

设计稿把模块外壳排在最后一个阶段，理由是去宏化会改变既有生成输出、需要重建 6 份 prebuilt。0.2 消解了后半个理由（mcpp 侧不再用 prebuilt），所以模块外壳一并做了：

- `huxerui-module-gen`（`mcpp/huxerui-tools`）生成 `modules/huxerui.cppm`，552 个 `export using`
- `tools/codegen/transform.h` 把注入文本提为 `kScopeOpenText` / `kScopeCloseText` 两个常量，实现与测试共用，不会各写一份
- `examples/mcpp_demo` 改用 `import huxerui;`，用法与 `#include` 完全一致

**`tools/prebuilt/` 仍是陈旧的**（仍注入宏名），CMake 的 `#include` 路径不受影响，但合入 main 后应由 `update-host-tools.yml` 重建——它正好在 `tools/codegen/**` 变更时触发。

### 0.4 依赖表用 `[build-dependencies]`，规则包目录改名

两处 review 意见，都在实现后修正：

- **`hcg` / `hrc` / 规则模块声明在 `[build-dependencies]`，不是 `[dependencies]`。** 三者都不进产物：两个工具在构建机上运行并产出源，规则模块被编译进 `build.mcpp` 本身。写成普通依赖等于声称它们是 HuxerUI 链接的一部分，那是假的。
- **`mcpp/rules/` 改名 `mcpp/huxerui-build-rules/`**，包名同步。一个叫 `rules` 的目录不说明任何事情；名字应当自带含义。

---

## 1. 目标与非目标

### 目标

1. **框架自身可由 mcpp 原生构建** —— Linux / Windows / macOS 三平台，`mcpp build` 产出 `libhuxerui`。
2. **基于框架的应用可由 mcpp 原生构建** —— 应用的 `mcpp.toml` 只需一行 `huxerui = "…"` 依赖，`build.mcpp` 只需一行 `import huxerui.rules;`，不再手写 SDK 路径、链接行、codegen 与资源调度。
3. **C++20 模块风格的消费入口** —— 应用可写 `import huxerui;` 而非 `#include <huxerui/huxerui.h>`，且 `#include` 路径行为完全不变。

### 非目标

- **不迁移 CMake。** CMake 保持唯一的全平台构建路径，并且是 Android / iOS / Web 的唯一路径。
- **不接管测试。** 105 个 `.cpp` 链成 Catch2 套件的模型与 mcpp「每个 `tests/**/*.cpp` 是独立程序」的契约不兼容，测试留在 CTest。
- **不原生模块化公共头。** `include/huxerui/*.h` 保持头文件形态与 SDK 契约不变；模块只是**额外的**一层前门。
- **不做 Android / iOS / Web。** `modules/toolchain-model/src/triple.cppm:783-785` 显式拒绝 `androideabi` / `wasi`，需要 mcpp 上游引擎改动，见分析报告 §3.1。

---

## 2. 目录布局

### 2.1 新增（全部是加法，删掉即回到今天）

```
mcpp.toml                              框架包 + workspace 根
build.mcpp                             pkg-config 探测；不做 codegen（框架无 composable）
mcpp/                                  mcpp 侧的一切，CMake 完全不感知
  README.md                            这套东西是什么、怎么用
  rules/
    mcpp.toml                          包 huxerui-build-rules（host-module）
    src/rules.cppm                     export module huxerui.rules;
  huxerui-source-select/
    src/sources.cppm                   纯扫描逻辑（rules 与工具共用，有单测）
  huxerui-tools/
    src/build_check.cpp                与 CMake 的源集合 / 链接集合比对
    src/module_gen.cpp                 模块外壳与 scope prelude 生成器
tools/codegen/mcpp.toml                hcg 作为 kind="bin" 包（与现有 CMakeLists.txt 并存）
tools/resource_compiler/mcpp.toml      hrc 同上
```

P5（模块）另加：

```
modules/huxerui.cppm                   模块外壳（生成物，入库）
```

### 2.2 工具的供给：两个方案，需要 review 定夺

**先回答"能不能不要这两个工具"：不能。** `mcpp::action` 的 command 是一个 **argv**，由 ninja 执行 —— 要成为构建图的一条边（增量、并行、失败可归因到具体文件），就必须有一个可执行文件。唯一的替代是在 `build.mcpp` / 规则模块里**进程内**做变换，而 mcpp 文档明确把这条路标为错误：

> Generating a source by writing it *here* is the easy path and the wrong one past a certain size: it happens once per prepare, for the whole set, serially, and a failure is reported as "build.mcpp exited 1". **Declare** the work and it becomes an edge in the build graph — incremental, parallel, and attributable to the edge that failed.（`docs/30-build-mcpp.md:433-440`）

`hcg` 的 888 行变换逻辑放进规则模块技术上可行，但会退化成"整包一次、串行、失败只说 build.mcpp exited 1"。对 36 个 examples 而言这是实质退步。**所以工具必须以可执行文件存在** —— 而它们本来就是。

真正的选择是**怎么把这个可执行文件交到构建手里**：

| | **A. `tools = [...]` 源码构建包** | **B. 直接用 `dep_dir()/tools/prebuilt/`** |
|---|---|---|
| 新增清单 | `tools/{codegen,resource_compiler}/mcpp.toml` 各一份 | **0** |
| 谁提供二进制 | mcpp 为宿主构建，按"包版本 × 宿主工具链 × features × 依赖闭包"全局缓存 | 仓库已提交的 6 份预编译二进制（linux x86_64/aarch64、macos x86_64/arm64、windows x86_64、android arm64-v8a） |
| 宿主覆盖 | 任意宿主 | 仅这 6 个组合（**CMake 侧同样受限** —— `HuxerUICodegen.cmake:106,116` 找不到就 `FATAL_ERROR`） |
| **与 CMake 的一致性** | 两侧可能跑不同二进制（源码构建 vs 已提交的 prebuilt）—— 若 prebuilt 落后于源码，**两套构建产出不同的生成代码** | **两侧跑字节相同的二进制，零漂移** |
| prebuilt 维护负担 | mcpp 侧可摆脱（长期看能减负） | 保持现状（每次改 `transform.cpp` 要重建 6 份） |
| 缓存键 gap | **受影响**：`path` 依赖的工具缓存键不含源码内容（`docs/30:940-948`），改了源码不升版本会静默复用旧二进制 | 不受影响 |
| 消费者怎么拿到 | `mcpp::dep_bin("huxerui-codegen", "hcg")` | `mcpp::dep_dir("huxerui") + "/tools/prebuilt/" + host + "/" + arch + "/hcg"`（规则包需复刻 `_huxerui_resolve_host` 的宿主/架构解析） |

**初稿只写了 A，那是个遗漏。** 按本方案自己的主张（§6：漂移是最大风险），**B 在最关键的一轴上更好** —— 两套构建跑同一个二进制，连"生成代码是否一致"这个问题都不存在，也不受缓存键 gap 影响。

**倾向 B 作为默认，A 作为无 prebuilt 宿主的可选逃生口**，但这是一个真实的取舍分叉，列入 §9 待决策。若选 B，则 §2.1 布局中的两份 `tools/*/mcpp.toml` 不需要，`[dependencies]` 里两条 `tools = [...]` 边也去掉，只保留 `huxerui-build-rules` 一条。

补充一点关于清单位置（若选 A）：mcpp 包的 `sources` glob 相对包根解析，把清单放在工具源码旁边，包根就是源码目录，不需要 `../../` 逃逸；这两个目录已经各自是独立的 CMake `project()`，多一个 `mcpp.toml` 与既有结构同构。

### 2.3 CMake 侧的改动清单（全部是加法）

| 改动 | 文件 | 阶段 | 性质 |
|---|---|---|---|
| 忽略 mcpp 产物 | `.gitignore` += `target/`、`.mcpp/` | P1 | 一行 |
| parity 测试 | `tests/build_parity/CMakeLists.txt` + `tests/CMakeLists.txt` 一行 `add_subdirectory` | P1 | 新增 |
| **`hcg` 去宏化** | `tools/codegen/transform.cpp:862,869` 两个字符串字面量 | P5 | **唯一一处实质修改** |
| 模块外壳的 CMake 对等 target（可选，默认 OFF） | `cmake/HuxerUIModules.cmake` + `CMakeLists.txt` 一行 `include` + 一个 `option` | P5b | 新增，门控 |

**现有 `.cmake` 文件与 `CMakeLists.txt` 的既有逻辑一行不动。**

---

## 3. 框架包（P1）

### 3.1 `mcpp.toml`

```toml
[workspace]
members = ["tools/codegen", "tools/resource_compiler", "mcpp/huxerui-build-rules"]

[package]
name        = "huxerui"
namespace   = "huxerui"
version     = "0.3.0"
standard    = "c++20"                 # SDK ABI 基线，不要提高
description = "HuxerUI declarative cross-platform UI framework"
license     = "Apache-2.0"
platforms   = ["linux", "windows", "macos"]

[targets.huxerui]
kind = "lib"                          # 静/动态形态由消费者 dependency_linkage 决定

[build]
sources                 = ["src/*/*.cpp"]     # 对齐 CMake 的 file(GLOB src/*/*.cpp)
include_dirs            = ["include", "src"]
private_include_dirs    = ["src"]             # src 是私有实现，不传递给消费者
cxxflags                = ["-Wall", "-Wextra", "-Wpedantic"]
macos_deployment_target = "12.0"

[features]
profiling = []                        # ≙ HUXERUI_ENABLE_PROFILING

# hcg / hrc：零第三方依赖的独立 C++20 程序，由 mcpp 为宿主构建并全局缓存。
# reexport 把它们连同规则模块一起交给本包的消费者。
[dependencies]
huxerui-codegen  = { path = "tools/codegen",            tools = ["hcg"], reexport = true }
huxerui-resource = { path = "tools/resource_compiler",  tools = ["hrc"], reexport = true }
huxerui-build-rules    = { path = "mcpp/huxerui-build-rules", host-module = true,            reexport = true }

# ---------------- Linux ----------------
[target.'cfg(linux)'.build]
sources = ["platform/linux/*.cpp"]
# GTK4 / epoxy / gio / libsoup 由 build.mcpp 经 pkg-config 探测

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
sources  = ["platform/macos/*.mm"]    # 注意：.mm 不在默认 glob 中，必须显式列出
cxxflags = ["-fobjc-arc"]
ldflags  = ["-weak_framework", "UniformTypeIdentifiers"]

# frameworks 是 TOP-LEVEL [runtime] 的键，不是 [target.macos.runtime] 的。
# per-target runtime 的词汇表只有 libraries / link_library_dirs
# （docs/04:1008、docs/22:392），写在 target 下会被"报告并忽略"。
# 无条件写在这里是安全的：引擎只在 Mach-O 上渲染它
# （src/build/flags.cppm:395-401 的 `if (flavor == LinkIntentFlavor::MachO)`），
# 与 [resources] 只在 PE 上编译是同一种形状 —— 写一次，不需要 cfg 谓词。
[runtime]
frameworks = ["AppKit", "AVFoundation", "Carbon", "CoreGraphics", "CoreImage",
              "CoreText", "CoreVideo", "ImageIO", "Foundation", "Metal",
              "MetalPerformanceShaders", "QuartzCore", "UserNotifications"]
```

### 3.2 `build.mcpp`

框架侧**不需要 `hcg`** —— `src/` 与 `platform/` 中 `[[huxerui::composable]]` 命中数为 0，`huxerui_enable_codegen()` 只被 `HuxerUIApp.cmake:249`（应用）与 `HuxerUILibraries.cmake:198`（用户库）调用。

框架侧也**不编译内置资源**（理由见 §4.3）。所以框架的 `build.mcpp` 只做一件事：

```cpp
import std;
import mcpp;

// pkg-config 的输出逐条转成 mcpp 指令。
// 与 CMake 的 pkg_check_modules 读同一份 .pc —— 这类元数据由构造保证不漂移。
bool probe(std::string_view mod);   // -I → include_dir，-l → link_lib，-L → link_search，其余 → cxxflag

int main() {
    if (std::string_view(mcpp::target_os()) != "linux") return 0;

    mcpp::rerun_if_env_changed("PKG_CONFIG_PATH");
    mcpp::rerun_if_env_changed("PKG_CONFIG_LIBDIR");

    // 版本下限与 cmake/platform/Linux.cmake 一致
    for (auto m : {"gtk4 >= 4.14", "epoxy >= 1.5", "gio-2.0", "libsoup-3.0 >= 3.0"})
        if (!probe(m)) {
            std::println(std::cerr, "huxerui: pkg-config module not found: {}", m);
            return 1;
        }
    return 0;
}
```

**关键性质**（`mcpp/docs/30-build-mcpp.md:835-845`）：依赖的 `build.mcpp` 发射的 `link-lib` / `link-search` **直达最终链接**。所以框架包探测出来的 GTK 链接行，天然就是应用的链接行 —— 应用侧不需要复述任何一条。

---

## 4. 规则包 `huxerui.rules`（P2）

### 4.1 模块 API

沿用 mcpp 自身规则包的 `plan` / `submit` 分离约定（`mcpp/examples/08-build-rules/rules-embed/`）：*"A rule without this pair has a cliff: past its last knob the only way out is to hand-write the action, and that copy then drifts."*

```cpp
// mcpp/huxerui-build-rules/src/rules.cppm
export module huxerui.rules;
import std;
import mcpp;

export namespace huxerui::rules {

// 字段与 CMake 的 huxerui_add_app() 参数一一对应，便于两侧对照 review
struct options {
    std::vector<std::string> sources;              // 默认 {"src/**/*.cpp"}
    bool                     codegen = true;       // ≙ huxerui_enable_codegen()
    std::string              resources;            // ≙ RESOURCES（资源根，空=无）
    std::string              resource_namespace;   // ≙ RESOURCE_NAMESPACE
    std::string              bundle_name;          // ≙ BUNDLE_NAME
    std::string              bundle_identifier;    // ≙ BUNDLE_IDENTIFIER
    std::string              target;               // 默认取包唯一的 bin target
};

struct edge {                                       // 与 rules-embed 同形
    std::string              id, role, description;
    std::vector<std::string> command, inputs, outputs;
};

std::vector<edge> plan(const options&);             // 只计划，不提交
bool              submit(std::span<const edge>);    // 交给 mcpp

inline bool configure(options o = {}) { auto e = plan(o); return submit(e); }

} // namespace huxerui::rules
```

### 4.2 `hcg` 的调度（对齐 `cmake/HuxerUICodegen.cmake`）

三个必须复刻的细节，遗漏任何一个都会产生"能构建但行为不同"的偏差：

1. **预筛**。CMake 用 `file(READ)` + `string(FIND)` 只对含 `[[huxerui::composable]]` **或** `Use` 字样的源建边。规则包必须做同样的预筛，否则会为每个源都建一条无用的变换边。
2. **变换后的源被移出原目录** → 必须为每个被变换源发射 `mcpp::include_dir(dirname(src))`，否则该文件里的相对 `#include` 会断掉。CMake 侧对应 `target_include_directories(${target} PRIVATE ${HUXERUI_CODEGEN_SOURCE_DIRECTORY})`。
3. **属性告警抑制**。CMake 末尾发射 `-Wno-attributes`（GNU）/ `-Wno-unknown-attributes`（Clang）/ `/wd5030`（MSVC），因为 `[[huxerui::composable]]` 是编译器不认识的属性。规则包必须按 `mcpp::target_os()` 与工具链发射等价 flag。

```cpp
mcpp::action a;
a.id   = "hcg:" + stem;
a.role = "source";                                   // 产物进入编译集
a.arg(mcpp::dep_bin("huxerui-codegen", "hcg"))
 .arg("--input").arg(src).arg("--output").arg(out)
 .input(src).output(out)
 .submit();
mcpp::include_dir(dirname(src));                     // 细节 2
```

### 4.3 `hrc` 的调度：框架生成头，应用**再编一次**取数据

> **本节为修正版。** 初稿结论是"内置资源改在应用侧编译"，那是错的：`grep -rn 'huxerui_builtin_resources' src/` 命中 **11 个框架源文件**（`src/text/validation.cpp`、`src/components/{tree,toggle,select,combo_box,presentation,refresh_box,progress,date_time_picker}.cpp`、`src/runtime/runtime_text_selection_overlay.cpp`、`src/application/window.cpp`）。框架**自身的编译**就依赖那个生成头，内置资源不可能挪到应用侧。

内置资源有两个产物，去向不同：

| 产物 | 谁需要 | 何时生成 |
|---|---|---|
| `huxerui_builtin_resources.h` | **框架自己的 11 个 TU** | 框架构建时，必须 |
| `package/huxerui/resources.bin` | **应用**（与自己的资源 merge） | 应用构建时 |

框架侧（`build.mcpp`，一条 `role = "source"` 边）：

```
hrc --root resources --output $MCPP_OUT_DIR --namespace huxerui     --header-name huxerui_builtin_resources.h
→ mcpp::include_dir($MCPP_OUT_DIR/include)
```

应用侧（规则包）：**再跑一次同样的命令**，因为框架生成的 `package/` 在框架的 `MCPP_OUT_DIR` 里，消费者够不着 —— `mcpp::dep_dir("huxerui")` 给的是依赖的**安装目录（源码根）**。而 `resources/` 本身是源码，就在 `dep_dir` 下：

```
hrc --root $(dep_dir huxerui)/resources --namespace huxerui  --output <out>/builtin
hrc --root <app resources>              --namespace <app ns> --output <out>/app
hrc merge --input <out>/builtin/package --input <out>/app/package --output <out>/final
```

后两步与 `cmake/HuxerUIResourceBuild.cmake` 的两步（逐 root 编译 → merge）结构相同。

**所以准确的说法是"重复"而不是"迁移"** —— 同一份内置资源被编译两次（框架一次取头，应用一次取数据）。两次输入相同、工具相同，因此输出确定相同，是可预测的重复劳动而非分歧来源。

**成本实测**：`resources/` 是 44 个文件 / 196 KB。一次 `hrc` 是毫秒级，且在每个应用项目内被 mcpp 增量缓存（只在 `resources/` 变化时重跑）。可接受。

**为什么不绕开**：mcpp 刻意不提供"依赖的构建产物 → 消费者的构建输入"这条通道。`include-dir` 显式是私有的（*"Cargo discipline: they color only this package's own TUs and are never propagated to consumers"*），`dep_dir` 给的是安装目录，`MCPP_OUT_DIR` 每包私有。唯一跨包的通道是链接行。三条被否决的替代：

| 方案 | 否决理由 |
|---|---|
| 框架把 `package/` 写进自己的源码根 | mcpp 明确禁止（*"the package root may be read-only"*） |
| 把编译好的 `package/` 提交入库 | CMake 侧仍会自己编译 → 出现一份 CMake 忽略、mcpp 使用的产物，正是 §6 要防的那类漂移 |
| 拆成独立的"内置资源包" | 同一问题递归：包的构建输出照样够不着 |

三条边的角色分配：

| 角色 | 内容 | 排序保证 |
|---|---|---|
| `role = "source"` | 生成的 `<ns>_resources.h` + `resources.bin` | 声明包的每条编译边等待它 |
| `role = "artifact"` | staging：把 `package/` 复制到 `${mcpp.target_file:<t>}` 旁的 `<name>.resources/` | 输入是链接产物，故排在链接之后 |
| — | `app.json` 集成计划：`build.mcpp` 直接写文件即可，不需要建边 | — |

### 4.4 应用侧最终形态

```toml
# 用户 App 的 mcpp.toml —— 全文
[package]
name     = "my-app"
version  = "0.1.0"
standard = "c++20"

[dependencies]
huxerui = "0.3.0"          # 或 { path = "../HuxerUI" }

[targets.my-app]
kind = "bin"
main = "src/main.cpp"
```

```cpp
// build.mcpp —— 全文
import mcpp;
import huxerui.rules;
int main() {
    return huxerui::rules::configure({
        .resources          = "resources",
        .resource_namespace = "my_app",
        .bundle_identifier  = "com.example.my_app",
    }) ? 0 : 1;
}
```

对照今天 `examples/mcpp_demo/mcpp.toml` 的 20 行 `/lib64/*.so` 绝对路径 + `[runtime] library_dirs = ["/lib64"]`。**P2 完成的验收标准就是把那 20 行删掉且 demo 仍然构建运行。**

---

## 5. C++20 模块风格适配（P5）

### 5.1 策略：模块外壳，不是原生模块化

```cpp
// modules/huxerui.cppm —— 生成物，入库，不手写
module;
#include <huxerui/huxerui.h>      // 全局模块片段：实体附着于全局模块
export module huxerui;

export namespace huxerui {
    using huxerui::View;
    using huxerui::State;
    using huxerui::UseState;
    // …
}
```

**库的二进制形态、ABI、链接接口完全不变。** `.cppm` 是消费者侧编译的一层薄封装。

**绝不分发 BMI。** BMI 与标准等级、工具链身份强绑定（`mcpp/docs/04-mcpp-toml.md:78-88`：标准是模块图全局的、不同等级永不共享缓存、GCC 对跨等级 BMI 报 `language dialect differs`）。而 Linux release SDK 是 gcc-14 / C++20 构建的。分发 BMI 会把 SDK 的适用范围从"任何 ABI 兼容的编译器"缩到"完全同一个编译器同一个标准等级"。**分发 `.cppm` 源。**

**而且这条约束几乎不要钱。** mcpp 自带**跨项目 BMI 缓存**（`README.md:240` *"Fingerprinted BMI cache: hashed by compiler/flags/standard library, shared across projects"*；`docs/90-build-from-source.md:70` 的 `bmi_cache/`）。分发源码之后，`huxerui.cppm` 的 BMI 在每台机器上按 (编译器 × flags × 标准库) 构建一次、**被该机器上所有项目共享**。所以"不分发 BMI"换来的不是"每个项目重编一次"，而是"每台机器一次" —— 与分发 BMI 的收益差距只剩首次构建的那一次。

这不是妥协，是把一个 ABI 耦合换成了一次本机缓存未命中。

### 5.2 ★ 前置阻塞：`hcg` 注入的是宏

`tools/codegen/transform.cpp:862,869` 注入 `HUXERUI_SCOPE_BEGIN` / `HUXERUI_SCOPE_END`，而这两个是定义在 `include/huxerui/view.h:1877-1885` 的**预处理器宏**。

**宏不跨模块边界。** 只写 `import huxerui;` 的应用，其 `hcg` 生成代码里这两个标识符未声明 → 编译失败。

**修复**：让 `hcg` 注入宏的展开式而非宏名。

```cpp
// transform.cpp:862  →  "\n  return ::huxerui::Scope([=]() -> ::huxerui::View {\n"
// transform.cpp:869  →  "\n  });\n"
```

**不要动检测路径。** `kScopeBegin` / `kScopeEnd`（`transform.cpp:16-17`）还被用于识别**输入源中手写的**显式作用域 —— `transform.cpp:357`（"composable function already contains an explicit HuxerUI scope"）与 `:561-584`。`HUXERUI_SCOPE` 系列仍是公开 API，手写代码继续使用。改的只是"注入什么"，不是"识别什么"。三个宏本身保留。

**连带工作**（AGENTS.md：*"Codegen: transform、生成的 Runtime 行为、common build、必需宿主工具一起更新"*）：
- `tests/codegen/` 的期望输出更新
- `tools/prebuilt/{linux/x86_64,linux/aarch64,macos/x86_64,macos/arm64,windows/x86_64,android/arm64-v8a}/hcg` 六份重建

**附带收益**：消除了"生成代码隐式依赖一个公共宏"这一耦合，这本身是独立的设计改进。

#### 三个替代方案，以及为什么仍推荐去宏化

| 方案 | 做法 | 评价 |
|---|---|---|
| **B. `--no-macro` 开关** | `hcg` 加一个 flag，规则包在模块场景下传入；默认行为不变。mcpp 有 `MCPP_LANGUAGE_MODULES`（2026.9.7.1+，*"A rule that GENERATES a consumer-facing declaration reads it to choose between a module interface and a header"*）正好用来决定传不传 | **保守变体**：不改变既有生成输出，`tests/codegen/` 的期望值不用动。代价是 `hcg` 多一条代码路径要测，且**仍然要重建 6 份 prebuilt**（加 flag 也是改二进制）。若 review 认为"不改既有生成输出"值这个代价，这是可选项 |
| **C. `import` 与 `#include` 并用** | 应用写 `import huxerui;` 之后再 `#include <huxerui/view.h>` 取宏 | **零改动，但自我否定**：包含了头就等于把 55 个公共头的解析又拉回来，模块的编译收益大部分消失。只适合作为过渡期的临时出口 |
| **D. 用 RAII/函数替代宏** | 把作用域包装改成不需要宏的形式 | **不可行**：宏展开是 `return ::huxerui::Scope([=]() -> ::huxerui::View {` —— 它跨越了 `return` 语句和 lambda 捕获的边界，没有等价的函数形式。这会变成对 composable 语言设计的改动，远超本方案范围 |

**A（去宏化）与 B（加开关）的真正区别只有一条**：A 改变既有生成输出（因此要更新 `tests/codegen/`），B 不改。**两者都必须重建 6 份 prebuilt**，所以"避免重建 prebuilt"不是选 B 的理由。A 之后 `hcg` 只有一条代码路径，生成代码在 `#include` 与 `import` 下逐字节一致；B 留下两条路径和一个必须靠规则包正确传参才能成立的隐式约定。**推荐 A。**

### 5.3 导出清单：生成，入库，廉价校验

导出清单必须生成 —— 手写会漂移，而且规模不小（`include/huxerui/` 55 个公共头）。

> **待实测**：命名空间作用域的顶层公开名的**确切数量**尚未测定。两次启发式扫描给出的是含成员函数误计的上界（O(10³)），不可直接引用。P5 第一步应用一次性的 clang AST 提取给出准确值。无论结果是 200 还是 900，"生成 + 入库 + 校验"的结论都不变；这个数只影响生成器的实现选型。

两层设计，把昂贵的部分做成低频：

| 层 | 时机 | 手段 | 作用 |
|---|---|---|---|
| **生成** | authoring-time，人工触发 | `huxerui-module-gen`，扫描公共头 | 产出 `modules/huxerui.cppm`，**入库** |
| **校验** | 每次 CI | 轻量正则扫描公共头的名字集合，与 `.cppm` 中已导出的集合求差 | 新增公共名未导出 → 测试失败并指名 |

昂贵的生成器只在校验报警时才跑。这与 §6 的防漂移思路一致：**重叠由机器校验，不由文档约定**。

### 5.4 落地顺序：mcpp 先行，CMake 对等是独立一步

- **P5a（mcpp 侧）**：`modules/huxerui.cppm` + 生成器 + 校验。mcpp 天然支持 `.cppm`，只需在应用包的 `sources` 里带上它。**CMake 零改动。**
- **P5b（CMake 对等，可选）**：新增 `cmake/HuxerUIModules.cmake`，`CMakeLists.txt` 加一行 `include()` 和一个默认 OFF 的 `option`。硬约束：
  - CMake ≥ 3.28（`FILE_SET CXX_MODULES`）；本机 3.28.3 恰好达线，但 `cmake_minimum_required(VERSION 3.20)` **不动**，改为在 target 处做版本门控
  - 生成器仅 Ninja ≥ 1.11 / Ninja Multi-Config / VS 2022。**Makefiles 与 Xcode 生成器不支持 C++20 模块**
  - Emscripten / Android NDK clang 的模块支持不成熟

把 P5b 拆出来并默认 OFF，是"CMake 不做大改动"这条约束的具体兑现。

### 5.5 `import std` 的边界

- 框架自身**不使用** `import std;` —— 那会把"必须有 `std` 模块的工具链"变成 SDK 的硬约束。
- 应用侧**可以**：`standard = "c++23"` + `import std;` + `import huxerui;`，链接的仍是 C++20 构建的库。`-std` 等级不改变 ABI。`examples/mcpp_demo` 已用 `c++23` 与 `c++26` 各验证过一次。
- `skills/huxerui-app-development/references/mcpp-build.md` 的既有 ABI 纪律全部继续适用：更新的 `-std` 不是 ABI 兼容方案；不得跨边界混用 libstdc++/libc++、MSVC/MinGW。

---

## 6. 防漂移：parity 校验

两套构建各自维护同一份事实，会在平台源清单、平台链接库、编译宏、警告等级、C++ 标准五处漂移。新增一个 `platform/windows/win32_foo.cpp` 要改两处，忘掉第二处 → Windows 的 mcpp 构建以 `undefined reference` 失败，且失败点离原因很远。

**复用已存在的机制**：`HUXERUI_LIBRARY_GRAPH_ONLY=ON` + `HUXERUI_LIBRARY_GRAPH_OUTPUT`（`cmake/HuxerUILibraries.cmake:58` 的 `_huxerui_write_library_graph`），`LANGUAGES NONE` 下纯配置即可产出 JSON，秒级。

```
mcpp/huxerui-tools/src/build_check.cpp
  1. cmake -S . -B <tmp> -DHUXERUI_LIBRARY_GRAPH_ONLY=ON -DHUXERUI_LIBRARY_GRAPH_OUTPUT=graph.json
  2. 解析 mcpp.toml 的 [build] sources + [target.*.build] sources
  3. 断言当前宿主平台的：源集合、平台链接库集合、编译宏集合 两侧相等
  4. 不等 → 失败，指名缺失的文件与应改的那一行
```

CI 增加**一个** job：Linux 上 `mcpp build`。一个 job 覆盖绝大多数漂移，代价约等于一次普通构建。

**Linux 依赖之所以选 pkg-config 而非 xlings 载荷**，正是这个理由：两侧读同一份 `.pc` 文件，这类元数据由构造保证不漂移，压根不需要校验。

---

## 7. 分阶段与验收

| 阶段 | 产出 | 验收标准 |
|---|---|---|
| **P0** | parity 测试骨架；`examples/mcpp_demo` 进 CI（Linux） | demo 在 CI 上可重复构建 |
| **P1** | 根 `mcpp.toml` + `build.mcpp`；两个工具包；parity 上线 | `mcpp build` 产出 `libhuxerui.a`；parity 通过；mcpp 侧不再依赖 `tools/prebuilt/` |
| **P2** | `mcpp/huxerui-build-rules/`；`reexport` 打通 | 一个用 composable + 资源的示例应用，`mcpp.toml` 依赖段**只有一行**；`mcpp_demo` 的 20 行绝对路径删除 |
| **P3** | Windows / macOS 段 | 三平台 `mcpp build` 通过；`otool -L` / 依赖检查符合 release 策略 |
| **P4** | 发布到 index | 干净机器上 `mcpp new` + 一行依赖即可构建运行 |
| **P5a** | `hcg` 去宏化 + 模块外壳 + 生成器 + 校验（mcpp 侧） | `import huxerui;` 的应用在 mcpp 下构建通过；`#include` 路径行为不变；codegen 测试更新 |
| **P5b** | CMake 对等 target（默认 OFF，门控） | Ninja + CMake ≥ 3.28 下 `import huxerui;` 可构建；其余生成器不受影响 |

**P1 必须在 P2 之前**，理由见分析报告 §0 修订说明：框架成为包之后，应用侧最难的两块（SDK 发现、链接接口翻译）自动消失。反过来做等于在没有包的情况下手写包管理。

---

## 8. 已知风险与坑

| 风险 | 说明 | 对策 |
|---|---|---|
| **`path` 依赖的工具缓存键不含源码内容** | `mcpp/docs/30-build-mcpp.md:940-948` 记录的已知 gap：改了 `hcg`/`hrc` 源码但没升版本，缓存的旧二进制继续被使用，构建"成功"但产物是旧的 | 开发期升版本或 `mcpp cache clean`；在 `mcpp/README.md` 里写明 |
| **`hrc` 的真实输入集合无法在 `build.mcpp` 运行时完整列出** | `input()` 在 `build.mcpp` 运行时固定边的输入，而 `hrc` 递归读取资源根 | `rerun_if_changed_glob("resources/**")` + 逐文件 `input()`；或让 `hrc` 输出 depfile 并设 `a.depfile`。**增量行为需实测确认** |
| **`.mm` 不在默认 glob 中** | 默认 `src/**/*.{cppm,cpp,cc,c,S,s,asm}`，macOS 的 25 个 `.mm` 会被静默漏掉 | `[target.macos.build] sources` 显式列出；parity 测试覆盖 |
| **`-weak_framework` 无中立表达** | `[runtime] frameworks` 表达常规 framework，弱链接没有对应键 | 落到 `[target.macos.build] ldflags` |
| **per-target `runtime` 词汇表只有两个键** | `[target.<sel>.runtime]` 只认 `libraries` / `link_library_dirs`；`frameworks` 写在那里会被静默忽略（报告 + 忽略，构建照常进行，链接期才炸） | `frameworks` 一律写顶层 `[runtime]`，引擎自行按 Mach-O 门控 |
| **`mcpp test` 与 Catch2 套件不兼容** | mcpp 认为每个 `tests/**/*.cpp` 是独立程序 | 明确不接管测试；`[build] sources` 不含 `tests/` |
| **模块外壳的导出清单规模未测定** | 两次启发式扫描均含成员函数误计 | P5 第一步做一次性 clang AST 提取，给出准确值 |

---

## 9. 待 review 的决策点

1. ★ **工具供给方式**（§2.2）—— **A. `tools = [...]` 源码构建包** 还是 **B. 直接用 `dep_dir()/tools/prebuilt/`**？初稿只写了 A，是遗漏。按本方案自己的防漂移主张，**B 更好**（两侧跑字节相同的二进制，且不受 `path` 依赖缓存键 gap 影响），代价是宿主覆盖受限于已提交的 6 个组合 —— 但 CMake 侧本来就受同样的限制（`HuxerUICodegen.cmake:106,116` 找不到即 `FATAL_ERROR`）。**倾向 B 为默认、A 为逃生口。**
2. **内置资源重复编译**（§4.3，已修正）—— 框架侧编一次取头（11 个 TU 依赖它，不可省），应用侧再编一次取数据。44 个文件 / 196 KB，毫秒级且被增量缓存。是否接受这份可预测的重复劳动？三条替代路径已在 §4.3 逐条否决。
3. **`hcg` 去宏化**（§5.2）—— 唯一的实质性既有代码修改。注意 **A（去宏化）与 B（加 `--no-macro` 开关）都要重建 6 份 prebuilt**，区别只在 A 会改变既有生成输出（需更新 `tests/codegen/`）而 B 不会。推荐 A（单一代码路径，`#include` 与 `import` 下生成结果逐字节一致）。是否批准？
4. **P4 是否要做** —— 发布到 index 决定 mcpp 侧是"仓库内可用"还是"外部用户可用"。若要做，Linux 依赖策略需重新审视：外部用户机器上未必有 GTK4 开发包，届时 `xim:gtk4` 等 xlings 载荷会从"可选"变成默认答案。
5. **P5b 是否要做** —— CMake 侧的模块对等。默认 OFF 且门控，但仍是 `CMakeLists.txt` 的一行改动。
6. **Android / iOS / Web 是否推上游** —— 给 mcpp 提三元组 PR 是独立投入（引擎四处改动点见分析报告 §3.1）。建议先与 mcpp 上游确认是否在其路线图内。
