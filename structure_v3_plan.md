# 第三版结构优化任务书（给本地 agent）

> 依据：`ARCHITECTURE.md`（重组后版本）和 `FAQ.md`。**评审者没有读过任何源码**，所以下面所有关于代码内部结构的判断都是"文档推断"，动手前必须先核对（见 §1 的核对清单），发现与事实不符就先停下来改本文档再继续。
> 范围：只谈**结构**——文件怎么切、边界在哪、什么由 CI 强制。不涉及任何算法或实现细节的优化。

---

## 0. 工作规则（先读）

1. **一个阶段一个分支或一组提交**，阶段之间不要混做。尤其是 §4（拆分，零行为变化）和 §6（API 约定，破坏性变化）**绝对不能合并成一次改动**。
2. 用户**没有 Mac**。本地只能跑 C++ 测试；Swift 和 ObjC++ 只能靠 CI 的 macOS runner 编译。凡是会碰 `StereoVision/` 的改动，每个提交都让 CI 跑一次，不要攒。
3. 不修改任何算法源文件（`sgm.cpp`、`geometry.cpp`、`rectify.cpp`、`analyze.cpp`、`cloud.cpp`、`calibrate.cpp`、`sample.cpp`）的逻辑，不修改 `Shaders.metal`。只有 §4.3 里点名的"按位置搬家"例外。
4. 每个阶段结束必须满足该阶段的**验收标准**，并把结果写进汇报。
5. 遇到与本文档假设不符的事实：**停下来汇报，不要自作主张**。

---

## 1. 评审结论（按优先级）

整体评价：分层规则可强制执行、参数表是唯一真源、测试与算法文件一一对应，这些是好的，**不要动**。下面是值得改的地方。

| 优先级 | 发现 | 为什么重要 |
|---|---|---|
| **P0** | **`FAQ.md` 里有与 `ARCHITECTURE.md` 直接矛盾的陈述，其中一条会诱导人去提交 `.xcodeproj`** | 用户没有 Mac，提交生成物是被明确禁止的；而 FAQ 的 Q11 末尾和 Q17 第二条还在说"改了 project.yml 要本地生成并提交"。这会误导以后读文档的 agent |
| **P1** | **C API 层没有按领域拆分**：1 个头（478 行、24 个符号）+ 1 个实现（903 行），而它下面的算法层早已按领域分成 8 个 `.cpp` | 变更理由混在一起；任何一个领域的改动都会让所有包含它的编译单元重编；`stereo_context` 变成"上帝结构体" |
| **P1** | **缺少第二个前端**：`stereo_process_frame` 目前只被测试调用，iOS 又不用它（FAQ Q18 解释了原因）。FAQ Q16 也承认 Windows/Linux 的 CLI 还没做 | 一个"跨平台"库如果只有一个真实使用者，就没人验证它真的跨平台。CLI 是整套重构的验收件 |
| **P2** | **C API 层不够薄**：文档说 `stereo_c_api.cpp` 里包含 `process_frame` 的组合调用，且 FAQ 说它还含像素格式统一（`to_luma`）。这两件事是"行为"，不是"翻译" | 边界层应只做翻译；行为放在 `namespace stereo` 里才能被 C++ 调用者直接复用 |
| **P2** | **无状态函数也带 `ctx`**：文档写"不用 ctx，但内部需要 scratch；这里收了 ctx 只为复用缓冲"（分析与抽样组）。iOS 为此在 `AppModel` 里多养了一个主线程专用的 `queryCore` | ctx 不是线程安全的，是这套设计里最大的使用成本 |
| **P2** | **API 约定不统一**：返回码有 `0/非0`、`0/1/-1` 两种风格；无 ctx 的函数没有错误串；图像 `stride` 是字节而深度 `stride` 是像素；参数表里的范围只是"UI 建议范围"，库的硬限制是另一套 | 每个约定不一致的地方都是一个潜在的跨语言 bug |
| **P3** | **Bridge 层镜像了 C API 的"上帝"形状**：`SVStereoCore` 一个类管 context、匹配、深度、分析、标定、processFrame；`-processFrame:` 与 `SVFrameRequest`/`SVFrameTimings` 约 350 行 Swift 零调用 | 桥接层应按 iOS **实际使用面**收缩，core 按**跨平台能力面**保持完整 |
| **P3** | **`Engine/` 的"可替换后端"名不副实**：`MatchingBackend`、`DepthBackend` 的形状都是"往 command buffer 里编码"，只有 Metal 实现；C++ 路径是并行的另一条同步路径，没有实现这些协议 | 文档宣称"阶段可替换"，实际可替换的是**整条管线**（Metal / C++ 混合 / 系统深度）。名字会让下一个人误以为可以随意换某一段 |
| **P3** | **iOS 侧没有任何测试**（ARCHITECTURE §6 第 7 条自己也承认） | 桥接契约（跨语言 ABI）只能靠真机发现 |
| **P3** | **文档中带数字的地方会腐烂**：行数、符号数、文件列表都靠人工同步，FAQ 已经腐烂了一半 | 应该由脚本生成或在 CI 里校验 |
| P4 | 仓库顶层布局没有表达"core 是产品、iOS 是使用者" | 收益是语义清晰，代价是路径大改；建议只在 CLI 落地之后再考虑 |

---

## 2. 核对清单（动手前必须做，只读）

回答下列问题并写进汇报。这些决定 §4 的拆分边界。

1. `core/src/stereo_c_api.cpp` 的 903 行大致分成哪几段？每段对应哪些导出符号？有多少行属于"共享辅助"（错误处理、参数校验、POD↔C++ 转换、像素格式统一）？
2. `struct stereo_context` 里有哪些成员？分别被哪些导出函数读写？画一张"成员 × 函数"的使用表（哪怕粗糙），找出**只被一个领域使用**的成员。
3. 其中有没有**逐像素循环**（`for … width/height`）？列出位置。这些就是"行为"，不属于边界层。
4. `stereo_disparity_to_depth`、`stereo_sample_depth` 是否真的需要 `ctx`（只为报错？只为 scratch？）？
5. `stereo_c_api.h` 里 6 个领域各占多少行？有没有跨领域共享的类型（`stereo_image`、`stereo_intrinsics`、`stereo_rectify_input` 等）？
6. `Bridge/*.mm` 各自 `#include` 了什么？是否有任何 `.mm` 直接 `#include "stereo/*.hpp"`？（ARCHITECTURE 规矩 2 说 hpp "只给 C++ 与桥接层看"，需要确认桥接层是否真的用到 hpp。）
7. `scripts/check_xcodeproj_sources.sh` 是否覆盖 `core/src` 的**子目录**？（§4 会新增 `core/src/capi/`。）
8. `project.yml` 里 `core/src` 和 `core/include` 是递归包含吗？

---

## 3. 阶段总览

| 阶段 | 内容 | 是否改行为 | 是否破坏 API |
|---|---|---|---|
| **0** | 建立基线与护栏（API 表面快照、分层检查脚本化） | 否 | 否 |
| **1** | 修文档矛盾（P0） | 否 | 否 |
| **2** | 拆分 C API（P1、P2 的一部分） | **否**（零行为变化） | **否**（保留总头） |
| **3** | 第二个前端：CLI | 否 | 否 |
| **4** | API 约定统一（版本 2.00） | 否 | **是**，需用户确认 |
| **5** | iOS 侧收敛：Bridge、Engine 命名、测试 target | 否 | 否 |
| **6** | 可选：仓库布局、安装与打包 | 否 | 否 |

**顺序理由**：阶段 3（CLI）放在阶段 4（破坏性改动）之前，是因为 CLI 是第一个真正"站在外面"使用公开 C 头的程序，它会暴露 API 哪里难用，用这些证据来决定阶段 4 改什么，比凭空设计更可靠。

---

## 4. 阶段 2 的详细规格：拆分 C API

### 4.1 设计原则

1. **按领域拆，不按行数拆。** 一个领域 = 一个公开头 + 一个 `capi` 实现文件，并与已有的算法文件和测试一一对应。
2. **`capi` 层只做四件事：校验参数 → 把 POD 翻译成 C++ 类型 → 调用 `namespace stereo` 里的函数 → 捕获异常转成返回码。** 里面不应该有逐像素循环。
3. **所有行为放在 `namespace stereo` 里。** C 接口只是一层可以被 Swift 和 C 调用的皮。
4. **保持向后兼容**：`stereo_c_api.h` 保留为**总头**，只 `#include` 各子头。`Bridge/*.mm`、`tests/`、Swift 桥接头一行都不用改。
5. 公开头必须**各自能被纯 C 编译器单独包含**（自包含）。

### 4.2 目标布局

```
core/
  include/stereo/
    stereo_c_api.h          ← 总头：只 #include 下面各个，保持旧路径可用
    stereo_types.h          ← 版本宏、导出宏、stereo_format、stereo_image、stereo_intrinsics
    stereo_context.h        ← stereo_context 生命周期 + stereo_config
    stereo_match.h          ← stereo_match、stereo_disparity_to_depth
    stereo_rectify.h        ← rectify_geometry / rectify_image / unrectify_depth（含其 POD）
    stereo_analyze.h        ← analyze_grid / analyze / sample_depth（含 blob、analysis POD）
    stereo_export.h         ← point_cloud(+capacity) / depth_filter / depth_to_u16_mm / copy_depth
    stereo_calibrate.h      ← calibrate / calibrate_search（含参数与结果 POD）
    stereo_pipeline.h       ← process_frame（以及阶段 4 之后的 process_disparity）
    stereo_params.h         ← param_count / param_table / param_find
    *.hpp                   ← C++ 头，位置不动
  src/
    *.cpp                   ← 算法实现，不动
    capi/
      capi_internal.hpp     ← struct stereo_context 的**唯一**定义 + 共用小工具
      context.cpp           ← create / destroy / config_default / configure / last_error
      match.cpp             ← stereo_match、stereo_disparity_to_depth
      rectify.cpp           ← 三个矫正函数
      analyze.cpp           ← analyze_grid / analyze / sample_depth
      export.cpp            ← 五个导出函数
      calibrate.cpp         ← calibrate / calibrate_search
      pipeline.cpp          ← process_frame
      params.cpp            ← 三个参数表函数
```

头文件依赖关系（必须是无环的）：

```
stereo_types.h
  ├─ stereo_context.h
  │    ├─ stereo_match.h
  │    ├─ stereo_analyze.h
  │    └─ stereo_export.h        （只在需要 ctx 时才依赖）
  ├─ stereo_rectify.h
  │    └─ stereo_calibrate.h
  ├─ stereo_params.h
  └─ stereo_pipeline.h          （依赖 context / match / rectify / analyze）
```

### 4.3 领域对应表（这是整个拆分的"地图"）

| 领域 | 公开 C 头 | `capi` 实现 | 算法实现 | 测试 |
|---|---|---|---|---|
| 基础类型 | `stereo_types.h` | — | — | `c_api_smoke.c` |
| 上下文与配置 | `stereo_context.h` | `capi/context.cpp` | `params.cpp`（默认值） | `c_api_smoke.c`、`frame_test` |
| 匹配与深度 | `stereo_match.h` | `capi/match.cpp` | `sgm.cpp` | `golden_main` |
| 矫正 | `stereo_rectify.h` | `capi/rectify.cpp` | `geometry.cpp`、`rectify.cpp` | `geometry_test` |
| 分析与抽样 | `stereo_analyze.h` | `capi/analyze.cpp` | `analyze.cpp`、`sample.cpp` | `analyze_test` |
| 导出 | `stereo_export.h` | `capi/export.cpp` | `cloud.cpp` | `cloud_test` |
| 标定 | `stereo_calibrate.h` | `capi/calibrate.cpp` | `calibrate.cpp` | `calibrate_test` |
| 全流水线 | `stereo_pipeline.h` | `capi/pipeline.cpp` | **新增 `src/pipeline.cpp`**（见下） | `frame_test` |
| 参数表 | `stereo_params.h` | `capi/params.cpp` | `params.cpp` | `frame_test`（自洽性） |

**两处需要"搬家"的行为代码**（这是 §0 第 3 条唯一的例外，且只搬不改）：

1. **`process_frame` 的组合调用** → 新建 `src/pipeline.cpp`，提供 `stereo::process_frame(...)` 的 C++ 版本，缓冲由一个 `stereo::FrameWorkspace` 之类的 C++ 结构持有；`capi/pipeline.cpp` 只做翻译。这样 C++ 调用者（测试、未来的 CLI）不用经过 C 边界也能用它。
2. **像素格式统一（`to_luma` 一类）** → 搬到算法层（例如 `src/color.cpp` 或并入 `rectify.cpp` 的格式辅助），`capi` 里不再出现任何像素循环。

### 4.4 `stereo_context` 的内部整理

不改对外句柄，只在 `capi_internal.hpp` 里把成员按领域分组成子结构，由 `stereo_context` 组合：

```
stereo_context
  ├─ MatchState        （luma / census / cost / sum / 视差缓冲）
  ├─ AnalyzeState      （网格 / 标签 / 栈 / 连通域缓冲 / 抽样 scratch）
  ├─ CalibrateState    （重采样 / 灰度 / census scratch）
  ├─ FrameState        （process_frame 需要的中间缓冲）
  └─ last_error
```

规则：

- 每个 `capi/*.cpp` **只能碰自己领域的子结构**（用代码评审或简单的 grep 约束）。
- 子结构**延迟分配**：只用到分析功能的前端（例如以后的 Qt 工具）不应该为一个用不到的 SGM 付 22 MB + 44 MB 的内存。

### 4.5 无状态函数的整理（只写进文档，实现放阶段 4）

先在每个公开头里把函数分成两类，在注释里写明：

- **无状态（不需要 ctx）**：矫正三件套、导出五件套、`analyze_grid`、参数表三件套。
- **有状态（需要 ctx 的 scratch）**：`match`、`analyze`、`calibrate`、`process_frame`。
- **待核对**：`disparity_to_depth` 与 `sample_depth` 是否真的需要 ctx（核对清单第 4 条）。如果 `sample_depth` 的 scratch 只有 2048 个 float，完全可以放栈上，那它不该带 ctx；这会让 iOS 的 `queryCore` 整个消失。**这个改动改变签名，放到阶段 4。**

### 4.6 CMake 与 XcodeGen 的配合

- `core/CMakeLists.txt`：`stereo_core` 的源文件列表加入 `src/capi/*.cpp` 与 `src/pipeline.cpp`；`target_include_directories` 不变。
- `project.yml`：确认 `core/src` 是**递归**包含（核对清单第 8 条）。若不是，补上 `core/src/capi`。头文件目录不变，`HEADER_SEARCH_PATHS` 不用改。
- `scripts/check_xcodeproj_sources.sh`：确认它递归扫描 `core/src`（核对清单第 7 条），否则新子目录里的文件会被漏检。
- `StereoVision/Bridge/*.mm`：**不改**。它们 include 的仍是 `stereo/stereo_c_api.h`。如果某个 `.mm` 因此需要改，说明总头没保持兼容，回去修总头。

### 4.7 阶段 2 的验收标准（全部满足才算完成）

1. **公开符号表与阶段 0 记录的基线逐字一致**（24 个，名字和参数个数都不变）。
2. 本地 7 个 ctest 套件全绿；GitHub Actions 的三平台 C++ 构建全绿；**未签名 IPA 的构建全绿**，且 `Bridge/`、`StereoVision/`、`tests/` 下**零文件被修改**。
3. 每个公开头都有一个**自包含检查**：CMake 里为每个头生成一个只包含它的 `.c` 编译单元并编译（纯 C 模式）。
4. `src/capi/*.cpp` 每个文件 **≤ 250 行**；`src/capi/` 里没有逐像素循环（用 grep 检查，白名单必须写明理由）。
5. `stereo_context` 只在 `capi_internal.hpp` 里定义一次。
6. 文档已更新（见 §9）。

### 4.8 阶段 2 的主要风险

- 多个 `capi` 文件各自定义同名的静态辅助函数导致重复；共用的放进 `capi_internal.hpp`。
- 总头漏包含某个子头，导致旧代码编译失败——靠验收第 2 条的 IPA 构建兜底。
- MSVC 要保持 `/utf-8` 和 `/fp:precise`；新增的每个 `.cpp` 都必须受 `stereo_core` 的编译选项约束（把选项挂在 target 上而不是文件上）。

---

## 5. 阶段 0：基线与护栏（在阶段 2 之前做）

目的：把"约定"变成"会失败的脚本"。全部是新增文件，不改现有代码。

1. **API 表面快照**：新增 `tools/check_api_surface.py`，用正则从所有公开 C 头里提取函数声明（名字和参数个数），与 `core/tests/api_surface.txt` 对比；不一致就失败。在阶段 2 开始**之前**，先用现有的单一总头生成基线并提交。
2. **分层检查脚本化**：把 ARCHITECTURE §6 里"检查用的命令"（目录红线的 grep、Swift 里逐像素循环的 grep）整理成 `scripts/check_layering.sh`，在 CI 里作为构建前的一步运行。
3. **`core/` 不许 include Apple 头**：同一个脚本里加一条 grep。
4. 把这些脚本接进 `.github/workflows/` 的 C++ 与 IPA 两条流水线（只是新增步骤，不改现有步骤）。

验收：故意引入一个违规（例如在 `UI/` 里 `import AVFoundation`），脚本会失败；撤销后通过。

---

## 6. 阶段 1：修文档矛盾

**这一步最先做**（在阶段 0 之后立即做，不依赖任何代码改动）。

| 位置 | 问题 | 改法 |
|---|---|---|
| `FAQ.md` §0 的"编译产物" | 写着 `StereoCore (Xcode) ──▶ libStereoCore.a`，但 Q10 明确说没有这个 target | 删除这两行，改成"iOS 由 App target 直接编译 `core/src`" |
| `FAQ.md` Q9 | `libstereo_core.a = sgm.o + stereo_c_api.o`，早已不符（CMake 里有 8 个源文件） | 改成按领域描述，或只写"见 `core/CMakeLists.txt`" |
| `FAQ.md` Q11 末尾的"注意" | 说 CI 编译提交的 `.xcodeproj`、不跑 xcodegen、改了 project.yml 要本地生成并提交 | **整段删除**，改成与 ARCHITECTURE 规矩 4 一致的说法 |
| `FAQ.md` Q17 第二条 | "改了 `project.yml` → 本地 `xcodegen generate` → 工程文件一起提交" | 改为"改了 `project.yml` → 推送，CI 生成工程" |
| `FAQ.md` Q1 / Q2 / Q8 | "2 个函数 / 250 行 / Bridge 134 行" 都是旧数字 | 删掉具体数字，只保留**角色描述**（见 §8 的原则） |
| `FAQ.md` Q16 第 2 项 | 说 `match` 的 C++ 版没做，但 CPU 打分版标定已存在，两者是否同一回事不清楚 | 核对后改成准确说法 |
| `FAQ.md` 标题 | 写着"`test` 分支" | 文档不该绑分支名，改成项目名 |
| `ARCHITECTURE.md` §0 | 写"四条硬规矩"，但列出了五条 | 改成"五条"，或把第 5 条单独成段 |
| `ARCHITECTURE.md` §1.1 | `stereo_process_frame` 一行写"iOS 不走它（见 §6.1）"，但原因在 FAQ Q18，不在 §6.1 | 改成指向 FAQ Q18 |
| `ARCHITECTURE.md` 规矩 2 | "`*.hpp` 只给 C++ 与桥接层看"，与 Bridge 实际只用 C 头的描述不一致 | 核对清单第 6 条之后改成事实 |

验收：全仓库 `grep -rn "xcodegen" *.md docs/` 的每一处，说法都一致（CI 生成，不提交）。

---

## 7. 阶段 3：第二个前端（CLI）

目的：让 `stereo_process_frame` 有一个真实的使用者，并验证 core 真的"不依赖 Apple"。

**约束**：

- 位置：`apps/cli/`（阶段 6 之前不动仓库顶层布局，所以先放这里）。
- **只能 `#include` 公开的 C 头**（`stereo_c_api.h`），**不能** include 任何 `.hpp`。这条自动约束了"公开面是否够用"。
- **零第三方依赖**：图像读写用最简单的 PNM（PPM / PGM / PFM）格式自己解析；标定参数用一个最简单的文本格式（建议 `key = value`，避免引入 JSON 库）。需要 PNG 的转换交给 `tools/` 下的 Python 脚本。
- 输入：一对**原始**（未矫正）图 + 标定文件；输出：深度图（PFM）、可选的 PLY 点云、一份耗时报告。

**验收**：

1. 在 Windows 和 Linux 上都能用 CMake 构建，并进入 CI 矩阵。
2. 把同一帧数据（左右原始图 + 标定）分别喂给 CLI 和 iOS 的 C++ 路径，**匹配阶段的整数中间量逐位一致**；最终深度在浮点容差内一致。
3. CLI 的使用过程中遇到的每一个"API 不好用"的地方，都记进汇报，作为阶段 4 的输入。

---

## 8. 阶段 4：API 约定统一（破坏性，**开始前必须让用户确认**）

这一步会改函数签名，Bridge 和所有测试都要跟着改，所以版本号升到 `STEREO_API_VERSION 200`，并且一次性做完，不留兼容层。

| 项目 | 现状 | 建议 |
|---|---|---|
| 返回码 | `0/非0` 与 `0/1/-1` 并存 | 统一成一个 `stereo_status` 枚举（成功、参数非法、缓冲不足、格式不支持、内存不足、内部错误）；"有效样本不足"这种**正常的业务结果**不要占用错误码，单独用输出参数表达 |
| 错误信息 | 有 ctx 的函数有 `stereo_last_error`，无 ctx 的函数没有 | 加 `stereo_status_string(status)`，无状态函数也能给出可读错误 |
| 跨度单位 | 图像字节、深度像素 | 参数与字段名里带单位后缀（`_bytes` / `_px`），文档里写明 |
| 无状态函数 | `sample_depth` 等带 `ctx` 只为 scratch | 去掉 `ctx`；这样 iOS 的 `queryCore` 可以删除 |
| 版本 | 只有一个编译期宏 | 加 `stereo_api_version()` 运行时函数，静态库之外的分发形态（`.so` / `.dll`）需要 |
| 导出宏 | 无 | 在 `stereo_types.h` 里加 `STEREO_API` 导出宏，为 Linux deb 与 Windows DLL 做准备；默认隐藏符号可见性 |
| 参数表范围 | "UI 建议范围"与"库硬限制"是两套 | 表里同时带两组范围，或者由库校验并导出硬限制，界面不再自己猜 |
| 流水线 | `process_frame` 一条龙；iOS 的 C++ 路径又单独调 `to_depth` 与 `unrectify` | 拆成两层：`stereo_process_disparity`（视差→深度→回转，iOS 与所有前端共用）+ `stereo_process_frame`（在其上再加矫正与匹配，给非 Apple 前端）。ARCHITECTURE §6 第 6 条已经指出了这个重复 |

**不在本阶段做的**：不改算法、不改参数表的条目、不改 `Shaders.metal`。

验收：C++ 测试全绿；IPA 构建全绿；真机回归（用户手测）一遍；`tools/check_api_surface.py` 的基线按新 API 重新生成并提交。

---

## 9. 阶段 5：iOS 侧收敛（每一步都让 CI 跑）

按风险从低到高：

1. **Bridge 按 iOS 实际使用面收缩**：`-processFrame:`、`SVFrameRequest`、`SVFrameTimings`（Swift 零调用，约 350 行）从 Bridge 删除；core 里的 `stereo_process_frame` **保留**（CLI 在用）。原则写进 ARCHITECTURE：**core 按跨平台能力面完整，Bridge 按 iOS 实际使用面最小**。
2. **Bridge 与 C API 的领域对齐**：把"上帝类" `SVStereoCore` 按领域拆开（匹配与深度、分析与抽样、标定各一个类），每个类只持有自己领域的状态。阶段 4 去掉无状态函数的 `ctx` 之后，这一步才有意义，所以放在它之后。
3. **`Engine/` 的命名诚实化**：`MatchingBackend`、`DepthBackend` 实际是"Metal 阶段"的窄接口。要么改名为 `MetalMatchingStage` 之类以免误导，要么在文档里明确"可替换的单位是整条管线（`StereoMode`），不是单个阶段"。**不要为了"以后可能换成 ONNX"提前造抽象**；FAQ Q18 已经指出真正的未来接缝只有两个：（矫正后的左右图 → 视差）与（视差 → 深度与分析）。等第三种视差来源真的出现再抽。
4. **iOS 测试 target**：在 `project.yml` 里新增一个测试 target，只测桥接契约：①用 `core/tests/golden` 的 tiny 向量经 `SVStereoCore` 跑一遍，结果与 C++ 测试逐位一致（验证跨语言 ABI）；②参数表自洽（默认值在范围内）。让 CI 的 macOS runner 在模拟器上跑它。
5. **FramePipeline 与 AppModel 的诊断统计**：本阶段不动，只记录在 ARCHITECTURE 的"后续"一节。

每一步验收：CI 构建全绿；真机回归清单（`docs/engine-backend-refactor.md` §6）里与改动相关的条目通过。

---

## 10. 阶段 6：可选项（只在用户明确要求时做）

- **仓库顶层布局**：`core/`（产品）、`apps/ios/`（`StereoVision` + `project.yml` + `Config` + `Bridge`）、`apps/cli/`、`tools/`、`docs/`。**收益是语义清晰，代价是 `project.yml`、workflow、脚本、文档里的路径全要改**，并且没有 Mac 的情况下只能靠 CI 验证。判断标准：只有当 CLI 已经落地、两个前端并存时，这个不对称才真正碍事。
- **安装与打包**：为 `stereo_core` 增加 `install()` 规则、CMake 配置文件、CPack 的 DEB 生成器，作为 Linux deb 的交付物。
- **文档组织**：把 `test.md`（981 行过程记录）和 `stereovision_agent.md`（一次性的目录整理方案）移进 `docs/history/`；FAQ 里"为什么这样设计"的条目改写成 `docs/decisions/` 下的短决策记录，每条只记**决定、理由、否决的方案**。

---

## 11. 文档的长期维护规则（阶段 1 起生效）

1. **带数字的内容要么由脚本生成，要么由 CI 校验**：行数、符号数、文件清单。至少做到：文档里列出的每个文件路径必须存在，被删除过的符号名在文档里零出现。
2. **FAQ 只写角色和理由，不写数字**。"这个文件管什么、为什么这样分"不会腐烂；"134 行""2 个函数"一定会。
3. **一件事只在一处说清楚**，其余地方写链接。例如"工程文件不提交"只在 ARCHITECTURE 规矩 4 里完整陈述，别处引用。
4. 文档里的行号引用一律用**符号名**代替。

---

## 12. 明确**不要**做的事

- 不要再拆分算法层的 `.cpp`（它们已经按领域分好了）。
- 不要在阶段 2 里顺手改函数签名或返回码。
- 不要为"将来可能换视差来源"提前建立抽象层。
- 不要往 `core/` 里引入任何第三方依赖。
- 不要在 CLI 落地之前搬动 `StereoVision/` 目录。
- 不要把阶段 2 和阶段 4 放进同一个分支。
- 不要在没有 Mac 的前提下依赖本地 Xcode 步骤。

---

## 13. 汇报格式

每个阶段结束时给出：

1. 修改或新增的文件清单（路径 + 一句话）。
2. 验收标准逐条的结果（附 CI 运行链接或日志片段）。
3. 与本文档假设不符的发现，以及你的处理。
4. 对下一阶段的建议（尤其是阶段 3 结束后，对阶段 4 的输入）。
