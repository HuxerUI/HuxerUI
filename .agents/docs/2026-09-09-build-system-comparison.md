# 2026-09-09 — 构建体系三方对比:原 CMake / 现 CMake / mcpp

> **状态**：对照报告。所有数字来自本机与 CI 的实测,不是估算。
> **对应 PR**：[#1](https://github.com/Sunrisepeak/HuxerUI/pull/1)(16 commits,CI 五 job 全绿)
> **相关**：设计方案 `2026-09-09-mcpp-native-build-and-modules-plan.md`;可行性分析 `docs/design/mcpp-dual-build-analysis.md`

---

## 0. 三列的关系

**"原 CMake"和"现 CMake"几乎是同一列。** `CMakeLists.txt` 与 `cmake/` 一字未动:

```
git diff --stat main..HEAD -- CMakeLists.txt cmake/    →   (空)
```

本 PR 改动的既有文件 9 个,其中 CMake 会编译/执行的只有 4 个,且都是**源码**而非构建规则:

| 文件 | 改动 | 对 CMake 构建的影响 |
|---|---|---|
| `tools/codegen/transform.h` | 新增两个注入文本常量 | 无(实现与测试共用一处定义) |
| `tools/codegen/transform.cpp` | 注入宏名 → 注入宏的展开式 | 生成代码文本变化,**语义相同** |
| `tests/codegen/transform.cpp` | 断言改用同一常量 | 无 |
| `src/io/file.cpp` | `constexpr` → `const`(Windows 分支) | 无(MSVC 产物一致) |

所以下文的"CMake"一列同时代表原状与现状,差异单独标注。

---

## 1. 库本身

| | **CMake** | **mcpp** |
|---|---|---|
| 平台 | **6**:Linux / Windows / macOS / Android / iOS / Web | **3**:Linux / Windows / macOS |
| 构建描述 | 18 个 `.cmake` 模块,**3371 行** | `mcpp.toml` 199 行 + `build.mcpp` 118 行 + 规则包 477 行 + 纯逻辑包 122 行 = **916 行** |
| 产物 | `huxerui`(shared)+ `huxerui_static`,作者决定形态 | `kind = "lib"` 一个包,**消费者**用 `dependency_linkage` 决定 |
| 平台依赖来源 | **发行版**(`pkg_check_modules` + apt) | **xlings payload**,36 个包的传递 `.pc` 闭包 |
| C 库 | 系统的(Linux release 锁 GLIBC 2.38) | payload 的 `xim:glibc`(本机实测 2.44) |
| 编译器 | gcc-14(Linux)/ MSVC(Windows)/ Xcode 26.2(macOS)/ NDK 29(Android)/ emcc 4.0.19(Web) | `gcc@16.1.0`(Linux)/ `llvm@20.1.7`(macOS、Windows,目标 `x86_64-windows-msvc`) |
| C++20 模块 | 无 | `modules/huxerui.cppm`,547 个 `export using` |
| 代码生成 | `add_custom_command` + `cmake_language(DEFER)` 重写 target SOURCES | `mcpp::action{role="source"}`,构建图的边 |

### 值得单独说的三点

**依赖来源的差异是刻意的,不是疏漏。** mcpp 用自己的工具链和自己的 glibc 编译,伸手去拿 host 的 GTK 会把两个 C 库混进同一个二进制。实测:`pkg-config --cflags gtk4` 发 `-I/usr/include/x86_64-linux-gnu`,payload glibc 的 `<time.h>` 于是拉进系统的 `<bits/time.h>`,**每个核心 TU** 都报 `'time' has not been declared in '::'`。`-idirafter` 能编过但链接时仍是"对 2.39 编的库 + 对 2.44 编的目标文件"。

**Windows 上两侧编译器不同,但 ABI 相同。** CMake 用 MSVC `cl.exe`,mcpp 用 clang 打 `x86_64-windows-msvc` —— 那是 Microsoft C++ ABI + Windows SDK,不是 MinGW,产物互相兼容。想统一到 MSVC 试过,被 mcpp 2026.9.8.1 的一个上游 bug 挡住(把宿主模块 BMI 以 clang 的 `name=path` 拼法交给 `cl.exe`,`LNK1104`)。

**行数少 3.7 倍不等于能力等价** —— mcpp 少的 2455 行里,绝大部分是 CMake 独占的三个平台(Android 的 Gradle/JNI、iOS 的 Xcode、Web 的 Emscripten 预载),以及 SDK 打包、WiX 安装器、运行时依赖收集。

---

## 2. SDK 工具侧

| | **CMake** | **mcpp** |
|---|---|---|
| `hcg` / `hrc` 来源 | `tools/prebuilt/<host>/<arch>/`,**6 份随仓提交的二进制** | 从源码构建(`tools = ["hcg","hrc"]`),按"包版本 × 宿主工具链"全局缓存 |
| 谁重建 prebuilt | `update-host-tools.yml`(main 推送 + `tools/codegen/**` 变更) | 不需要 |
| 工程创建 | `huxerui create app <name>` | `huxerui create app <name> --build mcpp` |
| 模板来源 | `tools/huxerui_cli/templates/project/{app,application}`(cmrc 内嵌) | 仓库根 `templates/app/`,**同一棵树**也被 `mcpp new --template` 读 |
| 模板渲染 | CLI 的 `@TOKEN@` | mcpp 的 `{{token}}`,CLI 与 mcpp 各有渲染器但**共用一份模板** |
| 依赖注入 | 模板写死 `HuxerUIProject.cmake` | **mcpp 原生** `inject_self_dependency` 自动注入模板自身包的依赖 |
| 打包 | `HuxerUISdk.cmake` / `package_sdk.sh` / WiX 安装器 | `mcpp pack`(未接入) |

**mcpp 侧不用 `tools/prebuilt/` 是被迫也是更正确的**:去宏化之后那 6 份二进制按定义就是陈旧的,而本机无法正确重建 —— payload 工具链产出的二进制链接 glibc 2.44,在 CI 的 2.39 上起不来。从源码构建同时也符合"不依赖 host"。

**当前限制**:`--build mcpp` 生成的工程写的是 `huxerui.huxerui = "0.3.0"`,需要 HuxerUI 发布到 index 才能解析 —— 和 `mcpp new --template` 是同一个前提。

---

## 3. 应用开发侧

用 CLI 各生成一个工程,实测:

| | **CMake** | **mcpp** |
|---|---|---|
| 文件数 | **93** | **6** |
| 行数 | **3632** | **96** |
| 构成 | 5 个根文件 + 88 个平台 shell(iOS 32 / Android 21 / Windows 20 / Web 5 / macOS 5 / Linux 5) | 全部是项目自己的代码 |
| 消费框架 | `#include <huxerui/huxerui.h>` | `import huxerui;` |
| 头文件 | 有 | **零** —— composable 在 `src/counter.cppm` |
| 构建描述 | `CMakeLists.txt` + `HuxerUIProject.cmake` | `mcpp.toml` 14 行 + `build.mcpp` 3 行 |
| 平台依赖 | 用户机器要装 GTK4 开发包 | 无需安装,mcpp 自己 provision |

生成的 mcpp 工程全文:

```toml
# mcpp.toml
[dependencies]
huxerui.huxerui = "0.3.0"
```
```cpp
// build.mcpp
import mcpp; import huxerui.rules;
int main() { return huxerui::rules::configure({ .resources = "resources" }) ? 0 : 1; }
```
```cpp
// src/counter.cppm  —— 模块接口单元,无头文件
module;
#include <typeinfo>
export module counter;
import huxerui;
[[huxerui::composable]] export View Counter() { auto count = UseState(0); ... }
```

对比之前 `examples/mcpp_demo/mcpp.toml` 的 20 行 `/lib64/*.so` 绝对路径 —— GTK 现在通过依赖边到达链接行,应用一个字都不用写。

### 一个必须知道的约束

**强制包含与模块接口单元不能共存。** 规则包给消费者发 `-include typeinfo`(GCC 的 typeid 检查是 TU 级预处理事实,导出 `std::type_info` 不管用),而 `-include` 插在 TU 第一行之前,模块接口必须以 `module;` 或 `export module` 开头 —— 即使源码已以 GMF 开头也报 `module-declaration only permitted as first declaration`。

所以规则的行为是**按包形态分叉**:包里有模块单元就不强制包含(单元自己在 GMF 里写,那本来就是模块单元放 include 的地方);没有就保持干净形式。

---

## 4. 各自不能做什么

| | **CMake 不能** | **mcpp 不能** |
|---|---|---|
| 平台 | — | Android / iOS / Web。不是配置问题:`triple.cppm:783-785` 显式声明 `androideabi` / `wasi` 不在 mcpp 的 target 语言里,BSP/adapter/payload 都补不了 |
| 模块 | 现状无模块 target(P5b,门控且默认 OFF,未做) | — |
| 测试 | — | 接管 HuxerUI 的测试:mcpp 认为每个 `tests/**/*.cpp` 是独立程序,而 HuxerUI 是 105 个文件链成 Catch2 套件 |
| 源替换 | — | `mcpp::action{role="source"}` **只增不替**,所以消费者要写 `sources = []` 让规则接管源选择,且 target 入口不参与变换 |

---

## 5. 测试与 CI

| | **本 PR 之前** | **现在** |
|---|---|---|
| PR 触发的 CI | **没有**(`sdk-release.yml` 只在 tag,`update-host-tools.yml` 只在 main) | 5 个 job,每次 PR |
| 库构建验证 | 无(tag 时才构建) | Linux / macOS / Windows 三平台 `mcpp build` |
| codegen 测试 | 无人跑(`update-host-tools.yml` 跑的是 `host_tools_test.py`,不是 Catch2 套件) | `HuxerUICodegenTests` |
| CLI 测试 | 同上 | `HuxerUICliTests` |
| 构建规则的单元测试 | — | `mcpp test`(`huxerui-source-select`)+ `HuxerUIMcppToolingTests`(13 个) |
| 两套构建的一致性 | — | `huxerui-build-check`(mcpp 构建的 C++ 程序),秒级,第一个 job |

**parity 校验已经抓到三类真缺陷**:裸 `platform/windows/*.cpp` 把 WiX 自定义动作 DLL 扫进 libhuxerui;`"-framework AppKit"` 被拆成两个 token;模板把 composable 放进 target 入口(永远不会被变换)。

**规则包的单元测试当场抓到两个**:`collect()` 的 `**` 与 `*` 两个分支逐字相同(两种 glob 选出同一集合);默认资源命名空间直接用包名,而包名不是 C++ 标识符(`mod-proj` → hrc 拒绝)。

---

## 6. 结论

**不是替代关系。**

- **CMake 是唯一的全平台路径**,且是 Android / iOS / Web 的唯一路径。发布、打包、安装器、SDK 分发都在它这边。
- **mcpp 在它覆盖的三个平台上,应用开发侧的差距是数量级的**:93 文件 / 3632 行 → 6 文件 / 96 行,零头文件,零系统依赖安装。
- **两者对"依赖从哪来"的回答不同是刻意的** —— 它们用不同的 C 库编译。这一条不能"统一",只能各自正确。

真正的风险不是能力,是**漂移**:同一份事实由两套构建各自维护。所以 parity 校验是这套体系里最不该省的部分。
