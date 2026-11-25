# core/ — 跨平台立体匹配核心（C++）

从 `StereoVision/Shaders.metal` 移植出来的立体匹配算法，目标是 **Windows / Linux / iOS 共用同一份代码**。

零第三方依赖、零 Apple 依赖：只吃裸指针 + 宽高 + 行跨度。OpenCV 只在 CLI 和测试里用。

## 现状

| 已实现 | 未实现 |
|---|---|
| `rgb_to_luma` / `census` / `cost_volume` / `sgm_path` / `sgm_all_paths` / `wta_right` / `wta_left` / `to_depth` | `match`（5×5 块匹配，自动标定要用）、`rectify` / `unrectifyDepth`、`geometry()`（单应）、`calibrate()`、C API |

标量版本已与 numpy 孪生实现（`tools/sgm_ref.py`）**逐位一致**，覆盖 64×48/D=16、320×180/D=32、
640×360/D=64 三个尺寸。

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
