# StereoVision — C++ / C-ObjC / Swift 三层结构

iPhone 后置双摄（广角 + 超广角）做实时立体匹配，测出每个像素的距离（`Z = f·B/d`）。
**本分支把算法从 Swift/Metal 抽成一个跨平台 C++ 核心**，iOS 只是它的一个使用者：
同一份 `.cpp` 在 Windows / Linux / iOS 上编译、结果逐位一致。

> 本分支删掉了语义分割模型（CoreML）与 Bird's-eye 视图，只用传统方法（Census + SGM）算深度。
> 重构的完整过程、取舍与踩坑记录在 [`test.md`](test.md)；本文只回答「这套代码怎么组织的、为什么」。
> **文件清单与调用链以 [`ARCHITECTURE.md`](ARCHITECTURE.md) 为准**——下面的目录树已按
> `stereovision_agent.md` 整理过（UI/Contracts/App/Camera/Analysis/Rendering/Engine），
> 若与本文件冲突，仍以 `ARCHITECTURE.md` 为准。

---

## 0. 项目结构（精简版）

```
core/                              ① C++ 核心（跨平台，零 Apple 依赖）
  include/stereo/sgm.hpp             算法函数声明（8 个，零件级，只给 C++ 看）
  src/sgm.cpp                        算法本体：census / costVolume / sgmPath / wta / toDepth
  include/stereo/stereo_c_api.h      纯 C 接口**总头**（只 include 9 个领域子头）
  include/stereo/stereo_*.h          按领域拆的自包含 C 头（types/context/match/…）
  src/capi/*.cpp                     C 门面：校验、POD 换乘、错误码（按领域分文件，无像素循环）
  src/{pipeline,color,workspace}.cpp 组合调用 / 取决于像素格式的搬运 / 跨帧缓冲
  tests/golden_main.cpp              与 numpy 孪生实现的逐位对拍
  tests/c_api_smoke.c                用纯 C 编译器验证 C 接口
  tests/golden/*.npy                 金标准向量（tiny，0.28 MB）
  CMakeLists.txt                     桌面端构建（MSVC / gcc / clang）

StereoVision/Bridge/               ② C / Objective-C++ 中间层
  StereoCoreBridge.h                 纯 ObjC 接口（只 import Foundation）
  StereoCoreBridge.mm                持有 C++ context，把 stereo_* 包成 ObjC 类

StereoVision/                      ③ Swift 前端
  StereoVision-Bridging-Header.h     编译器开关（只有一行 #import）
  StereoVisionApp.swift              @main（只接 RootView）
  UI/                                SwiftUI 界面（只依赖 Contracts）
    RootView.swift                   连线根：AppModel + 两个 sheet
    CaptureScreen.swift              主拍摄界面（HUD / SidePanel / 手势）
    SettingsScreen.swift             参数设置（草稿 + Apply）
    DiagnosticsScreen.swift          GPU/CPU 诊断（只读快照）
    OverlayView.swift                准星 / 面块框 / 点选气泡
  Contracts/                         界面与引擎的共同语言（无 UI 框架依赖）
    UIContracts.swift                CaptureUIState / SettingsDraft / DeviceInfo / Intents / 诊断快照
    PipelineContracts.swift          ViewMode / Matcher / PipelineSettings / FrameReport / FrameStore
  App/                               接线与调度
    AppModel.swift                   状态聚合 + 意图实现（主线程）
    FramePipeline.swift              每帧调度（相机队列）
  Camera/
    CameraSessionController.swift    双摄 / 系统深度两路采集
    CameraCalibrationReader.swift    第 1 步：只读系统给的 K / R / t
  Analysis/
    DepthSceneAnalyzer.swift         深度连通域 → 3D segmentation 标签
  Rendering/
    MetalSceneRenderer.swift         各视图的渲染（Metal）
    MetalView.swift                  MTKView 的 SwiftUI 包装
    MediaExportManager.swift         拍照 / 录像 / 导出
  Engine/                            双目流程编排 + 可替换的阶段后端
    StereoEngine.swift               唯一编排者（输入 → 几何 → 矫正 → 匹配 → 深度）
    StereoConfiguration.swift        后端组合：StereoMode.metal / .cppHybrid
    Input/ FrameInputProvider.swift  样本 → 纹理 + 内参 + CameraRig
    Geometry/ StereoGeometrySolver.swift  SVRectification.solve（core/src/geometry.cpp）
    Rectification/ MetalRectifier.swift   rectify kernel
    Matching/                         MetalSGMMatcher / MetalBlockMatcher / CppSGMMatcher
    Depth/                            MetalDepthProcessor / CppDepthProcessor / SystemDepthProcessor
    Calibration/ StereoCalibrator.swift   C++ 候选搜索 + GPU 打分
    Readback/ CPUImageReadback.swift     像素读回适配（C++ 匹配的输入）
    Metal/                            MetalContext / MetalPipelines / MetalFrameResources / MetalDispatch
  Shaders.metal                      GPU kernel（显示 / 点云 / 计算）
```

各阶段跑在 GPU 还是 CPU、资源谁写谁读、计时口径，见
`docs/engine-backend-baseline.md`；目录与职责的完整表在 `ARCHITECTURE.md` §3.2.5。

| 层 | 语言 | 职责 | 不知道什么 |
|---|---|---|---|
| ① `core/` | C++ / C | 算法 + 缓冲策略 | 不知道 Metal、CoreVideo、Swift |
| ② `Bridge/` | ObjC++ | 生命周期、ObjC 门面、错误转 `NSString` | 不知道 UI、不知道相机 |
| ③ `StereoVision/*.swift` | Swift | 采集、GPU 管线、渲染、交互 | 不知道算法内部 |

**语言边界只有两个**：`stereo_c_api.h`（C++ ↔ C）和 `StereoCoreBridge.h`（ObjC ↔ Swift）。
其余文件都是被这两个边界强制产生的，不是设计偏好。

编译产物：

```
core/CMakeLists.txt ──▶ libstereo_core.a   （桌面 / 非 Apple 平台的静态库）
iOS App target       ──▶ 直接编译 core/src（没有独立的 StereoCore target，见 Q10）
```

源文件清单以 `core/CMakeLists.txt` 的 `add_library(stereo_core ...)` 为准，本文不重复列。

---

# 一、为什么这样分层

## Q1. 公开 C 头与 C 门面各自负责什么？

**`stereo_c_api.h`：对外暴露的纯 C 接口契约。** 只有 C 结构体、枚举、不透明句柄声明和
`extern "C"` 函数声明，不含任何 C++ 语法与 C++ 头文件依赖，所以 C、Objective-C++（`.mm`）、
Swift 都能直接接入。

**门面的实现**：定义 `struct stereo_context` 的内部状态（按领域分组、跨帧复用缓冲区）、
校验入参与格式/步长转换，然后调用 `stereo::` 里的 C++ 算法。它负责的是**翻译**
（参数校验、POD ↔ C++ 类型、缓冲管理），算法本体不在这里。具体文件见
`core/CMakeLists.txt` 的源文件列表；跨平台能力面与 iOS 使用面的差别见
`ARCHITECTURE.md` §2。

## Q2. C 门面是不是把算法又实现了一遍？

**不是。** 两份头描述的是**粒度不同**的两组函数：

| | `sgm.hpp` | `stereo_c_api.h` |
|---|---|---|
| 一个函数干几件事 | 一件（`census` 就只是 census） | 一整段（census×2 → cost → sgm×4 → wtaR → wtaL） |
| 输入 | 已备好的 `float* luma` / `uint64_t*` 缓冲区 | 两幅**图**（5 种像素格式之一） |
| 缓冲区谁分配 | **调用方** | **库自己**（藏在 context 里跨帧复用） |
| 有无状态 | 无（纯函数） | 有（句柄 + 配置 + 错误串） |

门面里与算法有关的动作只有「按顺序把 `stereo::` 的零件函数串起来」；
算法本体（逐像素、逐视差）全在 `src/` 的算法文件里。

除匹配组合外，门面剩下的工作都与算法无关：把 `stereo_create()` 翻成 `new stereo_context`、
按尺寸复用缓冲区（「首帧之后零分配」的来源）、把像素格式统一成算法要的形状、
把 `std::bad_alloc` / C++ 异常转成错误码与错误串——这些都不能直接出现在 C ABI 上。

**类比**：`sgm.hpp` 是零件清单，`stereo_c_api.h` 是总成图纸；门面不是重造零件，
是**把零件按正确顺序装起来**。

## Q3. 为什么 `core/` 里不允许 include Apple 头？

因为 `core/` 的目标是「Windows exe / Linux / iOS 共用一份实现」。一旦引入 `simd/`、`Metal`、
`CoreGraphics`，它就只能在 Apple 平台编译，整个重构的意义就没了。这也是为什么
`core/` 只用 `float`/`int`/裸指针，几何矩阵用 `float[9]` 而不是 `simd_float3x3`。

---

# 二、语言边界：C++ / C / ObjC / Swift 怎么接

## Q4. 什么是「不透明句柄」？它和 C++ 的 private 类有什么区别？

头文件里只声明：

```c
typedef struct stereo_context stereo_context;   // 只有名字，没有成员
stereo_context *stereo_create(void);
void stereo_destroy(stereo_context *ctx);
```

对外只暴露 `stereo_context *` 指针，**隐藏结构体的成员细节**。

和 C++ 的 `private` 相比，不透明句柄连**结构体大小和内存布局**都不在头文件里暴露：
外部既不能按值创建（栈上分配）、也不能直接读写字段，还没法对成员做 `sizeof`。
这带来两个实际好处：

1. 头文件里不出现 `std::vector` 等 C++ 类型，纯 C 编译器能读；
2. 以后往 context 里加字段、换缓冲区策略，**调用方一行都不用改**（不需要重编）。

## Q5. 为什么门面里用 `stereo::` 显式限定，而不是 `using namespace stereo;`？

`sgm.hpp` 的算子都在 `namespace stereo` 下。显式写 `stereo::SgmParams`、`stereo::census(...)`
可以：

1. 避免命名冲突——本项目里就有 `stereo_context`、`stereo_image`、`stereo_match` 这些
   C 侧名字，和 `stereo` 命名空间只差一个下划线，全展开后极易混淆；
2. 一眼看出这个调用属于算法层还是本层自己；
3. 避免 `using namespace` 引入隐式重载解析和命名污染。

## Q6. `extern "C"` 和 `#ifdef __cplusplus` 分别起什么作用？

```c
#ifdef __cplusplus
extern "C" {
#endif
    int stereo_match(stereo_context *ctx, ...);
#ifdef __cplusplus
}
#endif
```

| 语法 | 作用 |
|---|---|
| `extern "C"` | 让 **C++ 编译器按 C 的链接规则导出符号**（不做名称修饰 / name mangling），C、`.mm`、Swift 链接时才能按 `stereo_match` 这个名字解析到。被包裹的函数体内部仍然照常写 C++（可以用 `std::vector`、`new`） |
| `#ifdef __cplusplus` | 只有 C++ / ObjC++ 编译时才启用 `extern "C"`；纯 C 编译器不认识 `extern "C"`，走 `#else` 分支跳过，否则直接语法报错 |

如果没有 `extern "C"`，`.a` 里的符号会变成 `_ZN6stereo12stereo_matchEPv...` 这种修饰名，
C 链接器找不到，报 `undefined symbol: stereo_match`。

用 `nm` / `dumpbin` 可以直接验证（未修饰 = 生效）：

```
$ python -c "d=open('build/core-ci/Release/stereo_core.lib','rb').read(); ..."
sgm.obj              FOUND
stereo_c_api.obj     FOUND
stereo_match         FOUND
stereo_create        FOUND
```

## Q7. 为什么不能直接把 `sgm.hpp` 给 C 用？

| `sgm.hpp` 里的 | C 编译器 |
|---|---|
| `namespace stereo { … }` | 不认 |
| `extern "C"` | 不认 |
| `#include <cstdint>` | C 里叫 `<stdint.h>` |

所以同一批函数必须另有一份 C 版声明。**但这不是「把 .hpp 逐行翻译成 .h」就完事**——
见 Q2：C 侧的函数粒度更粗、要管状态和内存，所以是「新写一个上层函数 + 在里面调 `sgm.cpp`」，
不是重写算法。

## Q8. `Bridge/*.h`、`Bridge/*.mm` 和 `StereoVision-Bridging-Header.h` 分别干什么？

| 文件 | 作用 |
|---|---|
| `Bridge/StereoCoreBridge.h` | 给 Swift 看的 **ObjC 接口**：`SVStereoCore` 类 + `SVImageFormat` 枚举。只 `import Foundation`，**不含任何 C++ 类型** |
| `Bridge/StereoCoreBridge.mm` | 唯一能同时看见 C++ 和 ObjC 的文件：持有 `stereo_context`、ARC 管生命周期、把错误转成 `NSString` |
| `StereoVision-Bridging-Header.h` | **不是一层**，是 build setting 的填充物：Xcode 给 swiftc 传 `-import-objc-header <文件>`，**有且只有一个** |

桥接头内容就一行：

```objc
#import "Bridge/StereoCoreBridge.h"
```

所以 Swift 里根本看不见 `stereo_context` / `stereo_image` / `stereo_match`——它只知道
`SVStereoCore`。这就是第二层隔离的意义。

**为什么不干脆让 Swift 直接调 C？** 可以，但 Swift 就得自己写
`deinit { stereo_destroy(ctx) }` 包装类，还要手搓 `stereo_image` 的 `UnsafePointer`。
ObjC++ 里写这段很短，Swift 里写会更长且更容易漏 release。

---

# 三、构建与产物

## Q9. 改了 `.h` / `.cpp` / `CMakeLists.txt` 之后，产物分别是什么？

**同一份 `CMakeLists.txt` 同时编译两类目标**：C++ 目标（`stereo_core` 静态库）和纯 C 目标
（`c_api_smoke`，用来证明 C 头真的能被 C 编译器读）。

```
libstereo_core.a  =  core/src 的算法目标文件（按领域分文件）
                     + C 门面（未修饰的 stereo_* 符号）
```

具体清单看 `core/CMakeLists.txt` 的 `add_library(stereo_core ...)`。

| 改动 | 影响 |
|---|---|
| 改 `sgm.cpp` / `sgm.hpp` | 只影响 C++ 核心与测试；上层 `.mm` 不需要改（它只看 C 接口） |
| 改公开 C 头（`stereo_*.h`）或 `src/capi/*.cpp` | 影响所有包含它的编译单元：`Bridge/*.mm`、`c_api_smoke.c`、`golden_test`、头自包含检查，需重编并更新 `core/tests/api_surface.txt` 基线 |
| 改 `CMakeLists.txt` | 只影响桌面构建；iOS 侧是 App target 直接编同一批源码（见 Q10） |

**上层接入时通常只需要总头 `stereo_c_api.h`（或某个领域子头）+ `.a`**；`*.hpp` 只在编译 C++ 核心和测试时用到。

## Q10. iOS 上 `core/` 是独立的 `StereoCore` target 吗？

**不是。** 现在只有一个 target：`StereoVision`（app），它的 `sources` 直接包含
`StereoVision/`、`core/src`、`core/include`（头文件 `buildPhase: none`）。

曾经试过拆成静态库（`StereoCore: type: static-library`），但：
1. XcodeGen 不认 `static-library` 这个写法（正确是 `library.static`），那个 spec 从来没生效过；
2. iOS 侧没有收益——产物最终都进 `.app`。

跨平台复用靠的是**同一份源码 + `core/CMakeLists.txt`**（Windows/Linux 出 `libstereo_core.a`），
与 Xcode target 结构无关。另外**工程文件已不再提交**：CI 每次用
`xcodegen generate --spec project.yml` 生成，`scripts/check_xcodeproj_sources.sh`
保证不漏文件。App target 的 `HEADER_SEARCH_PATHS` 仍需保留——`Bridge/*.mm` 要
`#include "stereo/stereo_c_api.h"`。

## Q11. 怎么构建和验证？

```bash
# 桌面（Windows / Linux / macOS）
cmake -S core -B build/core -DCMAKE_BUILD_TYPE=Release
cmake --build build/core --config Release --parallel
./build/core/Release/golden_test core/tests/golden        # 逐位对拍（tiny）
./build/core/Release/golden_test core/tests/golden-full   # 640×360, D=64
./build/core/Release/c_api_smoke                          # 纯 C 编译器验证

# 生成金标准向量（需要 numpy）
python tools/gen_golden.py                                # tiny 64×48 D=16（已提交）
python tools/gen_golden.py --size full --out core/tests/golden-full

# iOS（仅 macOS 本地调试；工程文件不入库）
xcodegen generate && open StereoVision.xcodeproj
```

`golden_test` 做 14 项检查（逐 kernel 6 项 + 整链 6 项 + C API 2 项），全部要求
**逐字节一致**（`memcmp`，不是 `allclose`）。

| CI 流水线 | 内容 |
|---|---|
| `Build C++ core` | ubuntu / windows / macOS 三平台编译 + 逐位对拍 + C API 冒烟 |
| `Build unsigned IPA` | Apple clang 编译 `core/` + ObjC++ 桥接 + Swift；在 iPhone 模拟器上跑 `StereoVisionTests`（桥接契约：tiny golden 与 C++ 逐位一致）；产出未签名 IPA |

```bash
./scripts/fetch_ipa.sh   # → build/StereoVision-unsigned.ipa
```

**注意**：`.xcodeproj` **不提交**。CI 每次用 `xcodegen generate --spec project.yml` 生成工程，
并用 `scripts/check_xcodeproj_sources.sh` 检查「仓库里的源文件」与「工程里参与编译的文件」一致，
漏一个就当场失败。改了 `project.yml` 只要推送即可，不要在本地生成后把工程文件一起提交。

---

# 四、运行与观测

## Q12. IPA 为什么从 6.8 MB 变成 2.4 MB？

删掉了 CoreML 分割模型。

| | 旧（带模型） | 新 |
|---|---|---|
| 解压后 | 12,790,616 B（21 个文件） | 3,235,453 B（9 个文件） |
| `lens-seg.mlmodelc/model.espresso.weights` | **9,388,480 B** | — |
| `Assets.car` | 2,132,399 B | 2,132,399 B（一样，现在是最大的一块） |
| `StereoVision` 可执行文件 | 924,248 B | 914,056 B |
| `default.metallib` | 80,048 B | 80,048 B |

模型权重占解压后体积的 **73%**，float 权重又压不太动，所以 zip 里少了约 4.3 MB。
可执行文件只小了 10 KB：删了 Detector + Vision/CoreML 链接，加进了 `sgm.cpp` +
C 门面的标量代码，基本互相抵消。算法代码比模型小两个数量级。

## Q13. 现在 GPU 用在哪？怎么确认？

点侧栏彩色徽标循环切换四条深度来源：

| 徽标 | 匹配在哪算 | GPU 在干什么 |
|---|---|---|
| `GPU`（橙） | Metal SGM-4 | 整条管线 + 显示 |
| `C++`（紫） | **C++ 核心（CPU）** | 只剩矫正、视差转深度、深度回转、显示 |
| `APPLE`（蓝） | 系统 | 显示 |
| `LIDAR`（绿） | 系统 | 显示 |

`C++` 路径的三段分工：

```
1. rectify（广角 + 超广角）              GPU   沿用现有 Metal kernel
2. 读回像素 + census/cost/sgm×4/wta      CPU   CVPixelBuffer 锁定 + MTLTexture.getBytes → C++ 核心
3. 视差上传 + toDepth + unrectify        GPU   新增 Slot.dispCPU（.shared r32Float）
```

侧栏 `waveform.path.ecg` 按钮打开**诊断页**：每帧一张表，列出每个阶段在 GPU / CPU / 系统、
各多少 ms；外加帧率、GPU/CPU 合计、`currentAllocatedSize`（已分配显存）、
`recommendedMaxWorkingSetSize`（工作集上限）。HUD 里也有一行 `GPU x.x ms · CPU y.y ms`。

**一个诚实的限制**：Metal 路径是整帧一条 command buffer，只能给出总 `gpuMs`，拆不出每个
kernel 的耗时（要拆得用 `MTLCounterSampleBuffer`）。C++ 路径天然分三段，反而更好定位瓶颈。

---

# 五、精度与性能

## Q14. 匹配精度和速度分别是什么水平？

**速度**：Metal 路径（iPhone 16，SGM + 左右一致性）约 20–22 fps；瓶颈是显存带宽
（每帧约 300 MB，下限 ≈6 ms），不是算力。

**精度**（合成场景，视差真值已知，含一个斜面、一个前景物体和一条对比度 4% 的低纹理带；
`python tools/sgm_ref.py` 可复现，纹理随机所以数字会小幅浮动）：

| 算法 | 正确（±1 px） | 错误 | 无效 |
|---|---|---|---|
| 块匹配 5×5 | 62.2% | 6.5% | 31.2% |
| SGM，4 路径 | 75.2% | 13.9% | 10.8% |
| SGM + 左右一致性 | 72.4% | 9.3% | 18.3% |

相比块匹配，SGM 找回了大片低纹理区域（无效从 31% 降到 11%）；左右一致性把错误匹配砍掉约
三分之一，代价是多判掉一些无效像素。深度误差随距离平方增长（`ΔZ ≈ Z²/(f·B)·Δd`）。

**C++ 标量路径**（x64 桌面，单线程，640×360 / D=64，不含 IO）：

| 阶段 | ms |
|---|---|
| `census` ×2 | 51.4 |
| `cost_volume` | 9.1 |
| `sgmPath` ×4 | 87.8 |
| `wta` R+L | 51.6 |
| `toDepth` | 11.6 |
| **合计** | **211.5 → 4.7 fps 上限** |

## Q15. 「逐位一致」是怎么保证的？有哪些坑？

1. **浮点写法照抄 numpy**。`rgb_to_luma` 一开始写成 `u * (1/255f) * w`，与 numpy 的
   `u / 255f` 差 **3 ULP**（769/3072 个像素不同）。改成真除法后 0 差异。
2. **不合并 FMA**。MSVC 用 `/fp:precise`，gcc/clang 用 `-ffp-contract=off`。
3. **MSVC 必须 `/utf-8`**。源码是 UTF-8 无 BOM（clang/gcc 默认），MSVC 在中文 locale 下按
   GBK 读，会把中文注释按多字节解析——轻则吞掉下一行，重则让字符串字面量「未闭合」。
4. **金标准用原始数值，不用图片**。`sgm_ref.py --dump` 出的是 8 位 PNG（已归一化），不能当
   对拍向量；所以另写了 `tools/gen_golden.py` 输出 `.npy`。
5. **Census 位序**：bit k = 第 k 个邻居（dy 外层、dx 内层、跳过中心）。Metal 把 bit0..31
   放进 `dst.x`、bit32..47 放进 `dst.y`，在 XOR+popcount 下与 uint64 等价。
6. **代价体积布局不要改**：AoS `[(y*W + x)*D + d]`，这是 GPU coalescing 与 CPU SIMD 共同的前提。

验证链条：`tools/sgm_ref.py` 是 Metal 内核的 numpy 逐行孪生实现（`--selftest` 逐元素对拍）→
`tools/gen_golden.py` 导出 `.npy` → C++ 与它逐位对拍 → 三平台 CI 各跑一遍。

---

# 六、待办与开发约定

## Q16. 还有哪些没做？

| # | 事项 | 说明 |
|---|---|---|
| 1 | 真机测 C++ 路径的 `CPU ms` | 唯一还缺的实测数据 |
| 2 | `Shaders.metal` 的 5×5 块匹配 kernel 的 C++ 对照 | C++ 侧没有这个 kernel；`stereo_calibrate` 的打分是另一套（Census + 逐视差汉明最小值、抽样），只服务非 Apple 平台。iOS 自动标定仍用 Metal 块匹配打分 |
| 3 | ~~`rectify` / `unrectifyDepth` 搬 C++~~ | **已完成**：`core/src/rectify.cpp`；`stereo_process_frame` 可整条 CPU 跑 |
| 4 | `census` / `wta` 钳位偏移表 + `sgmPath` SIMD | 见 Q14 的优化顺序 |
| 5 | Windows exe / Linux deb 的 CLI | **CLI 已落地**：`apps/cli`（只 include 公开 C 头，PNM/PFM + key=value 标定，CI 三平台冒烟）；**deb/exe 包装仍未做** |

## Q17. 开发约定

- **每次 CI 打包成功后立即** `./scripts/fetch_ipa.sh` 把 IPA 下载到
  `build/StereoVision-unsigned.ipa`，并报出大小与 sha256。
- 改了 `project.yml` → 推送；工程文件由 CI 生成，`.xcodeproj` 不入库（见 Q11）。
- 改了 `core/` → 必须重跑 `golden_test`；不是逐位一致就是 bug，不是「精度差异」。
- `core/` 不允许 include Apple 头（见 Q3）。
- 不允许 C++ 异常越过 `stereo_c_api.h`（Swift 无法展开 C++ 异常）。
- 可能失败的 C 函数返回 `stereo_status`（0 = `STEREO_OK`）；"有效样本不足"这类正常结果
  用输出参数（如 `stereo_sample_depth` 的 `out_has_value`），不占错误码。
- stride 参数/字段带单位后缀：图像 `_bytes`，深度/视差 `_px`（2.00 起）。
- 提交前自查：删掉的符号在 Swift 侧是否还有引用（本地没有 Swift 编译器，只能靠 grep + CI）。

---

# 七、打包边界

## Q18. iOS 上能不能把整条流水线打包成一个 `stereo_process_frame`？

**不能，而且在 iOS 上能打包的只有 `toDepth + unrectify`。** 判据不是"哪几步挨着"，
而是三个边界：

| 步骤 | iOS 上在哪 | 能否打包 | 原因 |
|---|---|---|---|
| 取相机参数 | Swift / AVFoundation | ❌ | 平台 API，C++ 没这个能力 |
| 极线矫正 | GPU（矩阵来自 C++） | ❌ | GPU 更快；而且**矫正后的两张纹理还被 Rectified pair / Anaglyph 视图消费** |
| 匹配 SGM | C++（`stereo_match` 内部已含 census/cost/sgm×4/wta 五小步） | ❌ 不该 | **视差的生产者可能换人**（ONNX / CoreML），焊进 C++ 打包函数就把替换入口锁死了 |
| toDepth + unrectify | C++ | ✅ | 纯算术，无平台交互，两者紧邻 |
| 连通域分析 | C++，但由 `FramePipeline.finish` 调用 | ❌ 不该 | **三条管线共用**（Metal / C++ / 系统深度）；焊进 C++ 专用路径会让另两条路再单独调一次 |

所以分界线是三条，而不是"一个全流程函数"：

```
（平台侧）产生视差  ‖  （C++）视差 → 深度 → 回转  ‖  （共用）连通域分析
        ↑ 可替换点            ↑ 唯一该打包的一段          ↑ 保持独立
   SGM / ONNX / 别的
```

- `stereo_process_frame`（core 里那条全流程）只服务"整条 CPU 跑"的平台：Windows / Linux / Qt。
- iOS 的 CPU 路径仍是四段：系统参数 → GPU 矫正 → C++ 匹配 → C++ 深度/回转 → 共用分析。
- 顺带一个便利：**Disparity 视图是从 `depth` 反算的**（`1/z` 线性映射），所以视差不必离开
  C++；将来换 ONNX 时，模型输出一个 `float* disp` 直接喂给 `stereo_disparity_to_depth`，
  下游一行都不用改。
