# core/ — 跨平台立体匹配核心（C++）

从 `StereoVision/Shaders.metal` 移植出来的立体匹配算法，目标是 **Windows / Linux / iOS 共用同一份代码**。

零第三方依赖、零 Apple 依赖：只吃裸指针 + 宽高 + 行跨度。OpenCV 只在 CLI 和测试里用。

## 文件都是干什么的（先读这个）

`core/` 里是**三对**文件，每对解决一个语言边界问题。搞混它们是最常见的困惑来源。

```
        ┌──────────── libstereo_core.a（Apple clang / MSVC / gcc 编译）────────────┐
        │                                                                          │
        │   sgm.hpp  ──▶  sgm.cpp              算法本体（C++）                      │
        │   ↑ 只有 C++ 能读                    零平台依赖、零缓冲区管理            │
        │                                                                          │
        │   stereo_*.h ──▶ src/capi/*.cpp          C 门面（C++ 写的、导出 C 符号）  │
        │   ↑ C 编译器也能读                    参数校验 + 结构体翻译 + 错误码        │
        └──────────────────────────────────────────────────────────────────────────┘
                 ▲                                        ▲
        core/CMakeLists.txt 产出的 .a            StereoVision/Bridge/StereoCoreBridge.mm
        （Windows exe / Linux / Python ctypes）    （iOS：把 stereo_* 包成 ObjC 类给 Swift）
```

| 文件 | 语言 | 是什么 | 不是什么 |
|---|---|---|---|
| `include/stereo/sgm.hpp` | C++ | 算法函数声明（内部用） | 不给 C/Swift 看 |
| `src/sgm.cpp` | C++ | **算法本体**；连 `malloc` 都不做，缓冲区全靠调用方传入 | 不是对外接口 |
| `include/stereo/geometry.hpp` | C++ | 第 2 步的几何：3x3 小工具 + `rectify_geometry()`（MW/MU/MUp/K/Pl/Pr/Q） | 不做重采样 |
| `src/geometry.cpp` | C++ | 由内参+外参解极线矫正（基线对齐旋转 + 单应） | 不碰像素 |
| `include/stereo/rectify.hpp` | C++ | 第 2 步的重采样 + 第 4 步的深度回转（`rectify_image` / `unrectify_depth`） | 不是对外接口 |
| `src/rectify.cpp` | C++ | `rectify` / `unrectifyDepth` 两个 Metal kernel 的 CPU 孪生 | — |
| `include/stereo/stereo_c_api.h` | **C** | `.a` 的**公开符号总头**：只 include 9 个领域子头（types/context/match/rectify/analyze/export/calibrate/params/pipeline） | 不是算法头 |
| `include/stereo/stereo_*.h` | **C** | 按领域拆的自包含子头；每个都能被纯 C 编译器单独包含（CMake 的 `stereo_header_selfcheck` 保证） | 不互相依赖成环 |
| `src/capi/*.cpp` | C++ | 上面那批 `stereo_*` 符号的**实现**：参数校验、POD ↔ C++ 换乘、把失败转成错误码；`capi_internal.hpp` 是 `struct stereo_context` 的唯一定义 | 不是算法；门面里没有像素循环 |
| `src/{pipeline,color,workspace}.cpp` | C++ | 门面与 C++ 调用者共用的行为：`process_frame` 组合、像素格式搬运、跨帧缓冲 | 不是新的算法 |

**为什么不能直接把 `sgm.hpp` 给 C 用**：

| `sgm.hpp` 里的 | C 编译器 |
|---|---|
| `namespace stereo { … }` | 不认 |
| `extern "C"` | 不认 |
| `#include <cstdint>` | C 里叫 `<stdint.h>` |

所以同一批函数必须有一份 C 版声明 —— 那就是 `stereo_c_api.h`。
两份头描述的是**同一个 `.a`**：`src/` 的算法文件出 C++ 符号，`src/capi/*.cpp` 出未修饰的
C 符号（`stereo_create` / `stereo_match` / …），`nm` / `dumpbin` 能直接看到。
符号面由 `tools/check_api_surface.py` 对比 `core/tests/api_surface.txt` 锁定。

**为什么 `stereo_c_api.*` 放在 `core/` 而不是 `Bridge/`**：它是 `.a` 的导出符号，属于
**库的一部分**。放这儿，Windows exe / Linux / Python ctypes 都能直接调；放到 `Bridge/`
就只有 iOS 能用了（而那层的存在理由正是「跨平台共用一份实现」）。

## 现状

| 已实现 | 未实现 |
|---|---|
| `rgb_to_luma` / `census` / `cost_volume` / `sgm_path` / `sgm_all_paths` / `wta_right` / `wta_left` / `to_depth`（第 3、4 步）<br>`rectify_geometry`（第 2 步的几何）、`rectify_image` / `unrectify_depth`（第 2 步的重采样、第 4 步的回转） | `match`（5×5 块匹配）、`calibrate()`（自动标定搜索）、第 1 步的取参数（那一步在 Swift：`StereoVision/Calibration.swift`） |

标量版本已与 numpy 孪生实现（`tools/sgm_ref.py`）**逐位一致**，覆盖 64×48/D=16、320×180/D=32、
640×360/D=64 三个尺寸。

极线矫正与深度回转没有外部金标准，用 `core/tests/geometry_test.cpp` 验**数学不变量**：
极线同行、近视差 = fx·b/Z、MW/MU 与显式针孔模型逐点相等、Q 能还原 3D、
反向外参 + flip 能复原几何等（`ctest -R geometry`）。

## 构建

```bash
# Windows（VS 18 / MSVC 14.51）
cmake -S core -B build/core -G "Visual Studio 18 2026" -A x64
cmake --build build/core --config Release

# Linux / macOS
cmake -S core -B build/core -DCMAKE_BUILD_TYPE=Release
cmake --build build/core
```

## 对拍

```bash
# 1) 生成金标准向量（需要 numpy；不带参数就是 tiny 那档）
python tools/gen_golden.py
python tools/gen_golden.py --size small --out core/tests/golden-small
python tools/gen_golden.py --size full  --out core/tests/golden-full

# 2) 跑对拍（12 项：逐 kernel 6 项 + 从 uint8 输入整链 6 项）
./build/core/Release/golden_test.exe core/tests/golden-full
```

任何一项不是逐位一致都会以非 0 退出。`ctest` 也能跑（默认那一档）。

## 约定（改代码前请读）

1. **布局不要动**：代价体积是 AoS `[(y*W + x)*D + d]`，这是 GPU 端 coalescing 与 CPU 端 SIMD 共同的前提。
2. **浮点写法要抄 numpy**：`rgb_to_luma` 用 `u / 255.0f` 而不是 `u * (1/255f)`——后者差 3 ULP，
   足以让逐位对拍失败。改任何浮点表达式后必须重跑 `golden_test`。
3. **不允许抛异常跨模块**，不依赖 RTTI。缓冲区由调用方传入，核心不做动态分配（现在 `sgm_path`
   有两个小 `std::vector`，是唯一的例外，后面会挪到调用方）。
4. MSVC 必须开 `/utf-8`（源码是 UTF-8 无 BOM），否则中文注释在 GBK locale 下会吞掉下一行。

## 性能现状（x64 桌面，单线程，640×360/D=64）

| 阶段 | ms |
|---|---|
| census ×2 | 51.4 |
| costVolume | 9.1 |
| sgmPath ×4 | 87.8 |
| wta R+L | 51.6 |
| toDepth | 11.6 |
| 合计 | **211.5（4.7 fps 上限）** |

优化顺序：钳位偏移表（census/wta）→ sgmPath 的视差维 SIMD → 多线程。每一步都用 `golden_test` 守逐位一致。
