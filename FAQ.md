# StereoVision（`test` 分支）— C++ + C/ObjC + Swift 三层重构

iPhone 后置双摄（广角 + 超广角）做实时立体匹配，测出每个像素的距离（`Z = f·B/d`）。
**本分支把算法从 Swift/Metal 抽成一个跨平台 C++ 核心**，iOS 只是它的一个使用者：
同一份 `.cpp` 在 Windows / Linux / iOS 上编译、结果逐位一致。

> 本分支删掉了语义分割模型（CoreML）与 Bird's-eye 视图，只用传统方法（Census + SGM）算深度。
> 重构的完整过程、取舍与踩坑记录在 [`test.md`](test.md)；本文只回答「这套代码怎么组织的、为什么」。

---

## 0. 项目结构（精简版）

```
core/                              ① C++ 核心（跨平台，零 Apple 依赖）
  include/stereo/sgm.hpp             算法函数声明（8 个，零件级，只给 C++ 看）
  src/sgm.cpp                        算法本体：census / costVolume / sgmPath / wta / toDepth
  include/stereo/stereo_c_api.h      纯 C 接口（.a 的公开符号表，总成级）
  src/stereo_c_api.cpp               C 门面：句柄、缓冲区复用、像素格式、错误码
  tests/golden_main.cpp              与 numpy 孪生实现的逐位对拍
  tests/c_api_smoke.c                用纯 C 编译器验证 C 接口
  tests/golden/*.npy                 金标准向量（tiny，0.28 MB）
  CMakeLists.txt                     桌面端构建（MSVC / gcc / clang）

StereoVision/Bridge/               ② C / Objective-C++ 中间层
  StereoCoreBridge.h                 纯 ObjC 接口（只 import Foundation）
  StereoCoreBridge.mm                持有 C++ context，把 stereo_* 包成 ObjC 类

StereoVision/                      ③ Swift 前端
  StereoVision-Bridging-Header.h     编译器开关（只有一行 #import）
  StereoEngine.swift                 Metal 管线 + C++ 管线（stereoCPU）
  CameraController.swift             双摄 / 系统深度两路采集
  Renderer.swift · Shaders.metal     各视图的渲染（Metal）
  Analyzer.swift                     深度连通域 → 3D segmentation 标签
  AppModel.swift · ContentView.swift · OverlayView.swift · DiagnosticsView.swift
```

| 层 | 语言 | 职责 | 不知道什么 |
|---|---|---|---|
| ① `core/` | C++ / C | 算法 + 缓冲策略 | 不知道 Metal、CoreVideo、Swift |
| ② `Bridge/` | ObjC++ | 生命周期、ObjC 门面、错误转 `NSString` | 不知道 UI、不知道相机 |
| ③ `StereoVision/*.swift` | Swift | 采集、GPU 管线、渲染、交互 | 不知道算法内部 |

**语言边界只有两个**：`stereo_c_api.h`（C++ ↔ C）和 `StereoCoreBridge.h`（ObjC ↔ Swift）。
其余文件都是被这两个边界强制产生的，不是设计偏好。

编译产物：

```
core/CMakeLists.txt ──▶ libstereo_core.a   （含 sgm.o + stereo_c_api.o）
StereoCore (Xcode)  ──▶ libStereoCore.a    （同一批源码，Apple clang 编译）
```

---

# 一、为什么这样分层

## Q1. `stereo_c_api.h` 和 `stereo_c_api.cpp` 各自负责什么？

**`stereo_c_api.h`：对外暴露的纯 C 接口契约。** 只有 C 结构体、枚举、不透明句柄声明和
`extern "C"` 函数声明，不含任何 C++ 语法与 C++ 头文件依赖，所以 C、Objective-C++（`.mm`）、
Swift 都能直接接入。

**`stereo_c_api.cpp`：它的实现与桥接层。** 负责定义 `struct stereo_context` 的内部状态
（缓冲区、配置、错误串）、跨帧复用缓冲区、校验入参与格式/步长转换，然后在内部调用
`sgm.hpp` / `sgm.cpp` 的 C++ 算法。

## Q2. `stereo_c_api.cpp` 是不是把算法又实现了一遍？

**不是。** 两份头描述的是**粒度不同**的两组函数：

| | `sgm.hpp` | `stereo_c_api.h` |
|---|---|---|
| 函数数 | 8 个 | 2 个 |
| 一个函数干几件事 | 1 件（`census` 就只是 census） | 6 件（census×2 → cost → sgm×4 → wtaR → wtaL） |
| 输入 | 已备好的 `float* luma` / `uint64_t*` 缓冲区 | 两幅**图**（5 种像素格式之一） |
| 缓冲区谁分配 | **调用方** | **库自己**（藏在 context 里跨帧复用） |
| 有无状态 | 无（纯函数） | 有（句柄 + 配置 + 错误串） |

`stereo_c_api.cpp` 里真正的算法代码只有 6 行：

```cpp
stereo::census(ctx->luma_l.data(), W, H, ctx->census_l.data());
stereo::census(ctx->luma_r.data(), W, H, ctx->census_r.data());
stereo::cost_volume(ctx->census_l.data(), ctx->census_r.data(), p, ctx->cost.data());
stereo::sgm_all_paths(ctx->cost.data(), ctx->sum.data(), ctx->luma_l.data(), p);
stereo::wta_right(ctx->sum.data(), p, ctx->disp_r.data());
stereo::wta_left(ctx->sum.data(), ctx->disp_r.data(), p, ctx->disp_l.data());
```

剩下 250 行干的全是跟算法无关的事：

| 干什么 | 为什么必须在 C++ 里 |
|---|---|
| `stereo_create()` → `new stereo_context` | C 没有 `new`，而 context 里是 `std::vector` |
| `ensure()` 按尺寸复用缓冲区 | 要 `std::vector::assign`；这是「首帧之后零分配」的来源 |
| `to_luma()` 统一 BGRA/RGBA/RGB/GRAY | 格式适配，不是算法 |
| `try/catch(std::bad_alloc)` + `err[256]` | 异常不能越过 C ABI，必须转成错误码 |

**类比**：`sgm.hpp` 是零件清单（8 个齿轮），`stereo_c_api.h` 是总成图纸（1 个变速箱）。
`stereo_c_api.cpp` 不是重造零件，是**把零件按正确顺序装起来**。

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

## Q5. 为什么 `stereo_c_api.cpp` 里用 `stereo::` 显式限定，而不是 `using namespace stereo;`？

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
ObjC++ 里写这段是 134 行，Swift 里写会更长且更容易漏 release。

---

# 三、构建与产物

## Q9. 改了 `.h` / `.cpp` / `CMakeLists.txt` 之后，产物分别是什么？

**同一份 `CMakeLists.txt` 同时编译两类目标**：C++ 目标（`stereo_core` 静态库）和纯 C 目标
（`c_api_smoke`，用来证明 C 头真的能被 C 编译器读）。

```
libstereo_core.a  =  sgm.o  +  stereo_c_api.o
                     ↑ 算法       ↑ C 门面（未修饰的 stereo_* 符号）
```

| 改动 | 影响 |
|---|---|
| 改 `sgm.cpp` / `sgm.hpp` | 只影响 C++ 核心与测试；上层 `.mm` 不需要改（它只看 C 接口） |
| 改 `stereo_c_api.h` / `.cpp` | 影响所有包含它的编译单元：`Bridge/*.mm`、`c_api_smoke.c`、`golden_test`，需重编并检查调用方接口 |
| 改 `CMakeLists.txt` | 只影响桌面构建；iOS 侧是 `project.yml` 的 `StereoCore` target 在编同一批源码 |

**上层接入时通常只需要 `stereo_c_api.h` + `.a`**；`sgm.hpp` 只在编译 C++ 核心和测试时用到。

## Q10. 为什么 `.a` 做成独立的 `StereoCore` target？

`project.yml` 里两个 target：

```yaml
StereoCore:    type: static-library    # core/src + core/include，Apple clang 编译
StereoVision:  type: application
               dependencies: [ StereoCore ]
```

好处：`core/` 的编译设置（C++ 标准、异常、头搜索路径）与 App 解耦，将来换 XCFramework
或给别的 App 复用只改一行依赖。

注意：App target 的 `HEADER_SEARCH_PATHS` 要保留——`Bridge/*.mm` 需要
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

# iOS
xcodegen generate && open StereoVision.xcodeproj
```

`golden_test` 做 14 项检查（逐 kernel 6 项 + 整链 6 项 + C API 2 项），全部要求
**逐字节一致**（`memcmp`，不是 `allclose`）。

| CI 流水线 | 内容 |
|---|---|
| `Build C++ core` | ubuntu / windows / macOS 三平台编译 + 逐位对拍 + C API 冒烟 |
| `Build unsigned IPA` | Apple clang 编译 `core/` + ObjC++ 桥接 + Swift，产出未签名 IPA |

```bash
./scripts/fetch_ipa.sh   # → build/StereoVision-unsigned.ipa
```

**注意**：CI 直接编译仓库里提交的 `.xcodeproj`，**不跑 xcodegen**。改了 `project.yml`
必须在本地重新生成工程并提交，否则新增源码不会被编译——而且**不会报错**。

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
`stereo_c_api.cpp` 的标量代码，基本互相抵消。算法代码比模型小两个数量级。

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
| 2 | `match`（5×5 块匹配）与 `calibrate` 的 C++ 版 | 自动标定现在仍走 Metal |
| 3 | `rectify` / `unrectifyDepth` 搬 C++ | 那时 `C++` 来源才是真正「零 Metal」的后端，可直接移植桌面 |
| 4 | `census` / `wta` 钳位偏移表 + `sgmPath` SIMD | 见 Q14 的优化顺序 |
| 5 | Windows exe / Linux deb 的 CLI | `.a` 已经能出，缺一个读图的 CLI |

## Q17. 开发约定

- **每次 CI 打包成功后立即** `./scripts/fetch_ipa.sh` 把 IPA 下载到
  `build/StereoVision-unsigned.ipa`，并报出大小与 sha256。
- 改了 `project.yml` → 本地 `xcodegen generate` → 工程文件一起提交。
- 改了 `core/` → 必须重跑 `golden_test`；不是逐位一致就是 bug，不是「精度差异」。
- `core/` 不允许 include Apple 头（见 Q3）。
- 不允许 C++ 异常越过 `stereo_c_api.h`（Swift 无法展开 C++ 异常）。
- 提交前自查：删掉的符号在 Swift 侧是否还有引用（本地没有 Swift 编译器，只能靠 grep + CI）。
