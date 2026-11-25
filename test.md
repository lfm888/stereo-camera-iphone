# StereoVision → 跨平台 C++ 立体匹配库：阶段结论与后续检查清单

> 本文档交给本地 agent 继续工作。
> 标记约定：**[已确认]** = 直接读过源码或 README；**[推断]** = 根据文件名、大小或上下文推测，必须核对；**[估算]** = 粗略数量级估计，必须实测。
> 本文档作者没有读过 `Analyzer.swift`、`sgm_ref.py`、`Renderer.swift`、`AppModel.swift` 等文件（见第 12 节），凡涉及它们的内容都是推断。

---

## 0. 给 agent 的一句话任务

把 `E:\lfm\stereo-camera-iphone`（lfm888/stereo-camera-iphone，fork 自 francescopeluso/stereo-camera-iphone）里的**立体匹配管线**（矫正、Census、SGM、左右检查、深度）写成**独立的、跨平台的 C++ 库**，供 Windows exe、Linux deb、iOS app 共用同一份算法；iOS 端只是它的一个使用者。先在新分支里清理掉不需要的模块，再逐步移植，不要一开始就改动大量 Swift 代码。

---

## 1. 背景与目标

- 用户熟练掌握 Python 和 C++，Swift 刚起步，**没有 Mac**。开发环境是 Windows（PowerShell，路径 `E:\lfm\...`），iOS 的构建靠 **GitHub Actions 和 Codemagic** 的 macOS runner，用 XcodeGen 的 `project.yml` 生成工程。
- 改造目的（用户明确说明）：**跨平台复用**。用户在另一个项目里已经用 Python 实现过 SGM 双目视差，想写成 C++ 供 Windows exe、Linux deb、iOS app 共用。
- 用户已决定：拉分支后**删除 Bird's-eye（俯视图剖面）和 3D segmentation 两个模块**，并且**先删除分割模型的转换与推理**（只用传统方法算深度）。
- 用户之前有过的原计划是"C++ + Objective-C + Swift"。本文档的建议是：对外接口用**纯 C**，这样 Swift 可以直接 import，Objective-C++ 这层基本可以省掉（需要更地道的 Swift 类型时再加薄壳）。

---

## 2. 仓库概况

### 2.1 目录（用户贴出的本地列表）[已确认]

```
.github/  .vscode/  build/  Config/  docs/  models/  scripts/  StereoVision/  StereoVision.xcodeproj/  tools/
.gitignore  codemagic.yaml  project.yml  README.md

Config/        Base.xcconfig, Local.xcconfig.example
models/        lens-seg.pth (9.5 MB), README.md
scripts/       export_lens_seg.py, export_lens_seg.sh, fetch_ipa.sh, make_icon.swift, verify_stereo.swift
tools/         make_demo_export.py, README.md, sgm_ref.py (35 KB), viewer.html (48 KB)
StereoVision/  Assets.xcassets/, Models/, Analyzer.swift (6.4K), AppModel.swift (16K), CameraController.swift (8.7K),
               Capture.swift (11.8K), ContentView.swift (11.6K), Detector.swift (14.7K), OverlayView.swift (7.7K),
               Renderer.swift (9.4K), Shaders.metal (17K), StereoEngine.swift (21.5K)
```

整个 Swift 加 Metal 代码总量很小（约 125 KB），移植范围可控。

### 2.2 已读 / 未读

| 文件 | 状态 |
|---|---|
| `README.md`（用户上传的最新版） | 已读全文 |
| `StereoVision/Shaders.metal` | 已读全文 |
| `StereoVision/StereoEngine.swift` | 已读全文 |
| `project.yml` | 只读过上游仓库的版本（本地大小 2218 字节，与上游 1.89 KB 不同，需核对） |
| `Analyzer.swift`、`AppModel.swift`、`Renderer.swift`、`Detector.swift`、`Capture.swift`、`OverlayView.swift`、`ContentView.swift`、`CameraController.swift` | **未读** |
| `tools/sgm_ref.py`、`tools/make_demo_export.py`、`tools/README.md` | **未读** |
| `codemagic.yaml`、`.github/workflows/*`、`Config/*.xcconfig`、`models/README.md` | **未读** |

---

## 3. 已确认的事实

### 3.1 管线总览 [已确认，来自 StereoEngine.swift 的 `stereo()`]

```
BGRA 相机帧（广角 + 超广角各一路）
 → rectify（两路，各用一个 3×3 单应 M，输出 640×360 的 RGBA8）
 → census（两路，输出 rg32Uint，即 48 位）
 → SGM：costVolume → sgmPath ×4 → wtaRight → wtaLeft      （或 BM：match）
 → toDepth（3×3 中值加 fB/z）
 → unrectifyDepth（用 MUp 把深度从滚转坐标系转回广角相机坐标系）
```

- 工作分辨率固定 `workW=640, workH=360`，`maxD=96`。
- 视差数 `D = min(96, max(32, maxDisparity/32*32))`，所以 D 只可能是 32、64、96。
- 整帧是一条 command buffer，随后 `cb.waitUntilCompleted()` **同步等待**，之后 CPU 直接读深度缓冲。
- 3 个 `Slot` 轮转复用纹理。

### 3.2 内存 [已确认，由代码算出]

- 代价体积 `costBuffer` = 640×360×96 = **22,118,400 字节（约 22 MB，`uchar`）**。
- 路径累加 `sumBuffer` = 上面的 2 倍 = **约 44 MB（`ushort`）**。
- 布局为 `[(y*W + x)*D + d]`（沿视差维连续），这个布局对 CPU 的 SIMD 同样友好，不要改。

### 3.3 kernel 细节（C++ 移植必须逐条复刻）[已确认，来自 Shaders.metal]

**`rectify`**
- 对目标像素 `gid`，取 4 个子采样点，偏移为 `{0.25, 0.75}²`（相对像素左上角，`i&1` 决定 x，`i>>1` 决定 y）。
- 每个点：`q = M * (gid + offset, 1)`，源坐标 `= (q.xy / q.z) / srcSize`（归一化坐标），用**线性采样**，`address::clamp_to_zero`（越界按零混合）。
- 4 次采样取平均后写入 `rgba8Unorm`（结果被量化回 8 位）。
- 注意：Metal 采样器的插值权重是硬件定点精度，**CPU 的浮点双线性不会逐位相同**。

**`unrectifyDepth`**
- `q = M * (gid + 0.5, 1)`；`q.z <= 1e-6` 则输出 0；否则 `s = q.xy / q.z`，取 `floor(s)` 的**最近邻**，越界输出 0。

**`census`**
- 亮度 `luma = dot(rgb, (0.299, 0.587, 0.114))`，中心阈值 `c = luma(center) - 0.004`。
- 7×7 窗口，跳过中心，共 48 个邻居；`luma(neighbor) < c` 时置 1。
- 位序：按 `dy=-3..3`、`dx=-3..3` 的顺序编号 `k`，`k<32` 写入 `bits.x` 的第 k 位，否则写入 `bits.y` 的第 `k-32` 位。
- 邻居坐标用 **clamp 到图像边缘**。

**`costVolume`**
- `out[d] = popcount(l.x ^ r.x) + popcount(l.y ^ r.y)`，其中右图像素 `xr = x - dir*d`；`xr` 越界则 `out[d] = 48`（最大代价）。

**`sgmPath`**（GPU 上一个线程组处理一条扫描线，线程 = 视差 d，**CPU 版必须重构**）
- 路径顺序和方向：`(dx,dy) = (1,0), (-1,0), (0,1), (0,-1)`，第一条路径 `first=1`（直接写 `S`），其余路径累加 `S = min(S + L, 65535)`。
- 每个位置：`s==0` 时 `L = c`；否则
  `P2' = max(P1+1, uint(float(P2) / (1.0 + abs(I - Ip) * 16.0)))`，
  `m = min(min(prev[d], min(prev[d-1], prev[d+1]) + P1), prevMin + P2')`，
  `L = c + m - prevMin`。
- `I` 是**矫正后左图**当前像素的亮度（float），`Ip` 是沿路径上一个像素的亮度。
- `prev[-1]` 和 `prev[D]` 的哨兵值是 `0x3FFFFFFF`。
- `prevMin` 是上一个位置 D 个 L 值的最小值。

**`wtaRight`**
- 对右图像素 `x`，遍历 `d`，左图候选 `xl = x + dir*d`，`xl` 越界即 break；读取 `S[(y*W + xl)*D + d]`，取最小，输出 `float(bd)`，**没有唯一性检查**。

**`wtaLeft`**
- `count`：从 d=0 起，第一个使 `xr = x - dir*d` 越界的 d 之前的个数；`count < 3` 输出 0。
- WTA 取 `bd`；次优 = 排除 `|d - bd| <= 1` 后的最小值；
  `ok = bd>0 && bd<count-1 && best*(1+uniqueness) < second`。
- 若 `lrCheck`：`dr = dispR(x - dir*bd, y)`，`ok &= |dr - bd| <= 1`。
- 亚像素：`den = c0 - 2*best + c2`，`den>0` 时 `sub = clamp(0.5*(c0-c2)/den, -0.5, 0.5)`；输出 `bd + sub`，无效为 0。

**`match`（5×5 Census 块匹配）**
- 窗口 `R=2`，`N=96`，代价为 25 个位置的汉明距离之和（`ushort`）。
- 同样有 `count<3` 无效、WTA、唯一性、亚像素；额外输出 `cost`：`count==nd ? best/maxCost : -1`，`maxCost=1200`。
- 用途：BM 匹配模式，以及 **自动标定 `calibrate()`**。

**`toDepth`**
- 取 3×3 邻域内 `d>0` 的视差，个数 `n>=5` 时取中值 `v[n/2]`，`z = fB / median`，不在 `[minZ, maxZ]` 内则置 0。

### 3.4 几何数学（StereoEngine.swift 的 `geometry()`）[已确认，移植时的陷阱]

- 矩阵是 **simd 的列主序**；`simd_float3x3(rows: [e1, e2, e3])` 是按**行**构造的，移植时极易转置错。
- `flip` 分支：`R = R.transpose; t = -(R * t)`，这里第二句用的是**已经转置后的 R**。
- `dir = t.x >= 0 ? 1 : -1`；`e1 = normalize(t) * dir`；`e2 = normalize((-e1.y, e1.x, 0))`；`e3 = cross(e1, e2)`；`Rr = rows[e1, e2, e3]`。
- `s = workW / srcW`；`K = (fx, fy, cx, cy) = KW 对应项 × s`；`Kr` 为由 K 构成的 3×3。
- `T = [[1,0,0],[0,1,0],[0,dy,1]]`（列主序，`columns: (1,0,0),(0,1,0),(0,dy,1)`）。
- `back = Rrᵀ * Kr⁻¹`；
  `MW = KW * back`；
  `MU = KU * Rᵀ * back * T`（这里的 R 是 flip 之后的局部变量）；
  `MUp = Kr * Rr * Kr⁻¹`；
  `fB = K.fx * |t| / 1000`（单位：米·像素）；`baselineDeg = atan2(t.y, t.x)` 转角度。
- 内参：优先读 `CMSampleBuffer` 的 `kCMSampleBufferAttachmentKey_CameraIntrinsicMatrix`，缺失时用 `f = size.x / (2*tan(fov*π/360))`、主点取图像中心。
- 外参：`AVCaptureDevice.extrinsicMatrix(from: ultra, to: wide)` 得到 `R`（3×3）和 `t`（毫米）；`|t| <= 1` 视为无外参，回退为单位矩阵和 `t=(12,0,0)`。
- 这部分（内外参的获取）是 AVFoundation，**只能留在 Swift**；`geometry()` 的数学可以移到 C++。

### 3.5 其他 [已确认]

- 点选测距函数 `StereoFrame.sample(_ r: CGRect, q: Float)`：对矩形内按步长抽样的深度取分位数，纯数学，应移到 C++。
- `StereoFrame.sample(_ m: Mask)` 依赖分割掩码，随分割一起删除。
- `DepthTarget` 的纹理是从共享 `MTLBuffer` 生成的，所以 **C++ 可以直接写 `buffer.contents()`，零拷贝**。
- 相机帧是 `32BGRA` 的 `CVPixelBuffer`，锁定后可直接给 C++ 读取。
- `depthData()` 是 Apple 深度和 LiDAR 数据源的处理，与立体匹配无关，保留在 Swift。
- `displayFragment` 里 `case 6` 使用 `palette()` 和 `labels` 纹理，是 3D segmentation 的渲染；视图模式 0–6 分别是：标准、深度、叠加、视差、配准对、红青立体、3D 分割。点云另有 `pointVertex/pointFragment`。
- 项目是 XcodeGen（`project.yml`，`sources: StereoVision`），`SWIFT_VERSION: "5.0"`，`MTL_FAST_MATH: YES`，部署目标 iOS 18。放进 `StereoVision/` 的 `.cpp` 和 `.mm` 文件会被自动编译，还需要配置桥接头、头文件搜索路径和 C++ 标准版本。

---

## 4. 关于"Shaders.metal 本来就是 C++，所以不用重写"的结论

用户转述了一段说法：MSL 是 C++14 的静态子集，`costVolume / sgmPath / wtaLeft` 已经是 C++，所以把 SGM 写成 C++ 不需要重写。**结论：对一半，不能当结论用。**

对的部分：
- MSL 基于 C++14，由 clang 编译，算法逻辑已经完整、清晰地写在 kernel 里，**不需要重新推导算法**。
- 约 260 行的计算部分，C++ 版量级是几百行。

不对的部分：
1. **脱离 Metal 工具链编译不过**：`float3 / uint2 / float3x3 / texture2d<> / device / constant / threadgroup / [[thread_position_in_grid]] / popcount / simd_min / threadgroup_barrier / saturate / mix / fract` 都来自 `<metal_stdlib>`。
2. **并行结构是 GPU 专属的**：kernel 是逐线程的写法，真正的"程序"在 Swift 的 dispatch 配置里。`sgmPath`（线程 = 视差，线程组 = 一条扫描线，三个 barrier 加 `simd_min`）在 CPU 上必须整体重构为"沿路径串行、对视差向量化、对扫描线多线程"。
3. **端到端不可能和 Metal 逐位一致**：GPU 采样器的插值精度、`MTL_FAST_MATH` 都会引入差异。

可以真正复用的部分：**参数结构体和常数**。目前 `RectParams / MatchParams / SGMParams / DepthParams` 在 Swift 和 Metal 里各写一份，靠手工对齐，很脆弱。建议放进一个 C 头文件，被 Metal、C++、Swift（桥接头）共同引用。

不建议为了"直接编译 .metal"去写一个模拟 Metal 类型的 shim：`sgmPath` 用不上，而且 `tools/sgm_ref.py`（numpy 孪生实现）已经充当了 CPU 参考。

---

## 5. 九个计算 kernel 的移植地图

| kernel | GPU 结构 | CPU 做法 | 难度 |
|---|---|---|---|
| `rectify` | 逐像素，4 子采样，双线性，越界取零 | 双重循环，可 SIMD，复刻边界语义 | 低到中 |
| `unrectifyDepth` | 逐像素，最近邻 | 双重循环 | 低 |
| `census` | 逐像素，7×7 | 滑窗；注意 float 亮度和 -0.004 偏置 | 低 |
| `match` | 逐像素，扫描全部视差，25 窗口 | 见 6.1，需优化 | 中 |
| `costVolume` | 逐像素，扫描 d | 沿 d 向量化 | 低 |
| `sgmPath` | 线程组加 barrier | **重构**，见下 | **高** |
| `wtaRight` | 逐像素，斜向读取 | 直接循环 | 低 |
| `wtaLeft` | 逐像素 | 直接循环 | 低 |
| `toDepth` | 逐像素，3×3 中值 | 直接循环 | 低 |

`sgmPath` 的 CPU 策略：沿路径串行推进，内层对 D 个视差做 SIMD（D=64 时 `uchar` 正好 4 个 128 位寄存器）；**水平路径按行分给多线程，竖直路径按列分**；路径累加用 `ushort` 饱和加；注意内存带宽，按条带处理使数据留在缓存里。

---

## 6. 对先前判断的修正（agent 不要沿用旧说法）

### 6.1 自动标定是 CPU 上最重的部分，不是 SGM [估算]

- `calibrate()` 用的是 **5×5 块匹配 `match`**，枚举 `flip`(2) × `dy ∈ [-12, 12]`(25) 再加 4 个细化点，共 **54 次**完整的"矫正 + Census + 匹配"。
- 每次约 226K 像素 × D(64–96) × 25 窗口 × 2 个 popcount ≈ 7 亿到 11 亿次；总计约 400–600 亿次 popcount。标量实现估计要十几秒到一分钟量级。
- 优化思路：先算逐像素逐视差的代价，再用**分离的盒式求和**代替 25 次 popcount；只抽样 1/16 的像素；先粗搜再细搜。它只是选参数的判据，**不要求和 GPU 逐位一致，只要求对录制的多帧选出同样的 `(flip, dy)`**。

### 6.2 "整数流水线逐位一致"需要收窄

流水线中有浮点参与了判决：Census 的 `luma < c` 比较（float）、SGM 自适应 P2 的 `float` 除法后转 `uint`、`wtaLeft` 的唯一性和左右检查、亚像素。所以结论是：
- **只要 C++ 用严格 float32、不开快速数学、不合并 FMA（gcc/clang 用 `-ffp-contract=off`，MSVC 用 `/fp:precise`），不同平台的 C++ 构建之间可以逐位一致。**
- **C++ 和 Metal 之间只能做"统计上几乎一致"的对比**（例如 99.9% 以上像素相同，其余差异落在亚像素容差内）。
- 建议把 **C++ 当作权威定义**，让 Metal 去对齐它。

### 6.3 内存数字

以第 3.2 节为准（约 22 MB + 44 MB），README 里的 "14.7 MB" 是 D=64 的情形。

### 6.4 数据拷贝不是问题

工作分辨率只有 640×360，一张 float 图不到 1 MB，拷贝可忽略；而且深度可以零拷贝写入 `DepthTarget.buffer.contents()`。

---

## 7. 架构决定（建议）

1. **独立的跨平台库 `stereo-core`**，先放在现有仓库的 `core/` 子目录，成熟后再独立成仓库。
2. **分三层**：匹配层（输入矫正后的左右图，输出视差，完全通用）、矫正层（单应重采样、滚转角、自动标定）、几何层（视差转深度、点云、深度回转、分位数）。
3. **对外接口用纯 C**：Swift 可直接 import；MSVC、gcc、clang 之间 ABI 稳定；便于以后接 Python（ctypes）等。
4. **核心库不依赖 OpenCV**：只吃裸指针、宽高、行跨度；OpenCV 只在 CLI 和测试里用于读写图片。
5. **SIMD**：先写标量参考版并通过测试，再向量化（可考虑 Highway 或 xsimd 之类跨 ISA 封装，**需自行核对对 MSVC 和 iOS 的支持与许可证**）；线程用标准库线程池。
6. **GPU 作为可选后端**：C API 里预留 `backend` 参数，默认 CPU；现在不实现。
7. **构建**：CMake（Windows MSVC、Linux gcc，CPack 的 DEB 生成器打包 `.so`、头文件、CMake 配置，aarch64 交叉编译）；iOS 因核心零依赖，最简单是直接把 `.cpp` 加入 XcodeGen 的 `sources`，跨仓库复用时再用 Swift Package 的 C++ target 或 XCFramework。
8. 建议的目录：

```
stereo-core/
  include/stereo/stereo.h        纯 C 接口
  include/stereo/params.h        共享参数结构体（同时被 Metal / C++ / Swift 引用）
  src/                           census / sgm / rectify / geometry / simd
  tests/                         对拍 sgm_ref.py 的金标准测试
  apps/cli/                      命令行：读左右图和标定，写深度 PNG / PLY
  CMakeLists.txt  Package.swift
```

---

## 8. iOS 接入与测试设计

**两级接入：**
- **第一级（用于验证）**：矫正仍用 GPU，C++ 只替换匹配部分，输入是 GPU 矫正后的左右图（左图在 `leftPool` 的 `CVPixelBuffer`，右图在共享的 `slot.right`）。此时两边输入完全相同，可以直接比较视差。需要把单个 command buffer 拆成"矫正 → 等待 → C++ 匹配 → 回转"。
- **第二级（真正跨平台）**：矫正、匹配、深度回转全部走 C++，输入是原始 BGRA 相机帧。

**做成第四个深度来源**：在现有"自研 / Apple Dual-Wide / LiDAR"的切换里加一项"自研（C++ CPU）"，便于和 Metal 版并排比较帧率和有效像素比例。

**测试分层：**
- 匹配阶段：相同矫正输入下，**整数中间量**（Census 位、代价体积、路径累加和）逐位对拍；视差输出统计对比。
- 矫正阶段：容差对比（单个灰度级以内）。
- 几何矩阵：把 `KW, KU, R, t, flip, dy` 记进 `meta.json`，在 PC 上重算 `MW, MU, MUp`，与 Swift 一致。
- **现有导出没有矫正后的左右图**（`view.png / rgb.png / depth_mm.png / cloud.ply / meta.json`），需要让 App 额外导出，或先用 `tools/make_demo_export.py` 生成合成数据。**待确认这个脚本到底输出什么。**
- 自动标定：对多帧录制数据，C++ 优化版选出的 `(flip, dy)` 应与 GPU 版一致。

---

## 9. 分支清理清单

> **本节的 [推断] 已被 §15.2 核对并修正：`Analyzer.swift` 与两个视图都不依赖分割模型，
> 删它会把「3D segmentation」一起带走。动手前先看 §15.1 与 §15.4。**

**用户已决定删除：** Bird's-eye 俯视图剖面、3D segmentation、分割模型整条链路。

| 位置 | 动作 |
|---|---|
| `Shaders.metal` | 删 `displayFragment` 的 `case 6`、`palette()`、`labels` 纹理（`texture(3)`）、`DisplayParams.hasLabels`（要与 `Renderer.swift` 的绑定、结构体布局同时修改） |
| `StereoEngine.swift` | 删 `sample(_ m: Mask)`；保留 `sample(_ r: CGRect, q:)` |
| `Analyzer.swift`（[推断] 可能是 3D 分割和俯视图数据） | **先读，确认没有被保留功能依赖，再决定删除** |
| `Detector.swift`（[推断] 分割推理、Vision 回退、跟踪） | 删除；注意尺寸测量、物体跟踪随之失效，点选测距保留 |
| `OverlayView.swift`（[推断] 掩码、框、标签叠层，也可能包含俯视图绘制） | 先读，再精简 |
| `AppModel.swift`、`ContentView.swift`、`Renderer.swift` | 去掉两个视图模式的枚举项和对应渲染分支 |
| `models/`、`StereoVision/Models/`、`scripts/export_lens_seg.*`、`.github/workflows/export-model.yml`、`project.yml` 里编译 YOLO 模型的预构建脚本 | 删除；README 说无模型时 App 退回 Apple 内置检测器（需核对 `Detector.swift` 是否还要保留这个回退） |
| `README.md` | 同步删除两个视图模式、分割相关章节 |
| `codemagic.yaml`、`scripts/fetch_ipa.sh` | 检查是否引用了模型导出，按需调整 |

**注意：用户没有 Mac，编译错误只能靠 CI 暴露，每个删除步骤请做成小提交，让 CI 跑一次。**

---

## 10. C API 草案（仅签名，供审阅，尚未实现）

```c
// stereo.h —— 纯 C 接口
typedef struct sv_context sv_context;

typedef struct {
    int   max_disp;        // 32 / 64 / 96
    int   matcher;         // 0 = SGM, 1 = 块匹配
    int   p1, p2;
    float uniqueness;
    int   lr_check;
    float min_z, max_z;
    int   threads;         // 0 = 自动
} sv_config;

typedef struct { const uint8_t* data; int w, h, stride; int fmt; } sv_image;  // fmt: BGRA8 / RGBA8 / GRAY8

// 纯数学：由内外参算出矫正单应（对应 StereoEngine.geometry）
int sv_make_geometry(const float KW[9], const float KU[9], const float R[9], const float t[3],
                     int src_w, int work_w, int flip, float dy, sv_geometry* out);

int sv_rectify(sv_context*, const sv_image* src, const float M[9], sv_image* dst);
int sv_match(sv_context*, const sv_image* left, const sv_image* right, int dir,
             float* disp_out, int disp_stride);
int sv_to_depth(sv_context*, const float* disp, int stride, float fB, float* depth_out);
int sv_unrectify(sv_context*, const float* depth, const float M[9], float* out, int stride);
int sv_calibrate(sv_context*, const sv_image* wide, const sv_image* ultra,
                 /* 内外参 */ sv_calib_result* out);
float sv_depth_quantile(const float* depth, int stride, int w, int h,
                        float x0, float y0, float x1, float y1, float q, int* ok);
```

匹配层（`sv_match`）不含任何 iPhone 专属逻辑，应能直接用于用户的 RGB 双目项目（基线约 65 mm，图像 1280×1080）。

---

## 11. 阶段计划与验收标准

| 阶段 | 内容 | 验收 |
|---|---|---|
| P0 | 拉分支，按第 9 节清理；让 App 额外导出矫正后的左右图和完整内外参 | CI 能编译通过；删除的功能不再出现；导出文件齐全 |
| P1 | 测试向量：用 `sgm_ref.py`、`make_demo_export.py` 生成各阶段输入输出，放进 `core/tests/` | 能在 Windows 上一键生成 |
| P2 | C++ 匹配层：先标量，对拍通过后做 SIMD 和多线程 | 整数中间量逐位一致；视差统计一致；在 Windows、iPhone、（如有）RK3568 上有基准数据 |
| P3 | 矫正层、几何层（移植 `geometry()`、`rectify`、`unrectifyDepth`、`calibrate` 的优化版、分位数函数） | 单应矩阵与 Swift 一致；多帧录制数据上 `(flip, dy)` 与 GPU 版一致 |
| P4 | C API、Swift 集成，作为第四个深度来源接入 | 与 Metal 版并排运行，记录帧率和有效像素比例 |
| P5 | Windows CLI、Linux deb（CPack）、CI 矩阵（windows-latest、ubuntu、macos） | 三个平台跑同一套测试 |
| P6（可选） | GPU 后端 | 与 CPU 参考实现一致 |

---

## 12. 待检查文件清单（agent 的下一步）

请**按顺序**读以下文件，并回答每个文件后面的问题，把答案补写回本文档。

1. `StereoVision/Analyzer.swift`
   - 里面有哪些函数和类型？分别属于 3D 分割、俯视图剖面、物体测量、跟踪中的哪一类？
   - 哪些被保留功能（点选测距、点云、导出）依赖它？删除它会让谁编译失败？
2. `tools/sgm_ref.py` 和 `tools/README.md`
   - 所谓"六个 kernel"具体是九个计算 kernel 里的哪六个？
   - 是否包含 `rectify`、`unrectifyDepth`、`match`、`toDepth`？
   - `--selftest` 的对拍粒度和阈值是什么？输入是怎么生成的（合成数据？真实导出？）
   - 能否直接导出各阶段（Census、代价体积、路径累加、视差）的中间结果作为 C++ 测试向量？格式是什么？
3. `tools/make_demo_export.py`
   - 输出哪些文件？是否包含矫正后的左右图和内外参？
4. `StereoVision/Renderer.swift`、`AppModel.swift`、`ContentView.swift`
   - `PipelineSettings` 的字段、默认值（尤其 `maxDisparity, p1, p2, uniqueness, lrCheck, minZ, maxZ`）。
   - 视图模式枚举在哪里定义？两个要删的模式在哪些地方被引用？
   - `DisplayParams` 的字段在 Swift 和 Metal 两边是否一致？
5. `StereoVision/Capture.swift`
   - 导出 `meta.json`、`cloud.ply`、`depth_mm.png` 的代码在哪？点云是 CPU 生成还是 GPU？是否值得移到 C++？
6. `StereoVision/Detector.swift`、`OverlayView.swift`
   - Vision 回退检测器是否值得保留？尺寸测量、跟踪的依赖范围？
7. `project.yml`（本地版）、`codemagic.yaml`、`.github/workflows/*`、`Config/*.xcconfig`、`models/README.md`
   - 本地 `project.yml` 与上游版本的差异（本地 2218 字节）。
   - CI 现在怎么构建、签名、出 IPA？删除模型后哪些步骤要改？
   - 在 `sources` 里加入 `core/` 目录，XcodeGen 需要补哪些设置（桥接头、头文件搜索路径、C++ 标准）？
8. **向用户询问**：用户在另一个项目里实现的 Python SGM 在哪？是否与 `sgm_ref.py` 同一算法（Census、4 路径、自适应 P2、左右检查）？差异点（路径数 4 或 8、代价函数、窗口、P1/P2 策略）有哪些？据此决定 C++ 里哪些做成可配置参数。

---

## 13. 风险与未知

- **CPU 版 SGM 在 iPhone 上会比 Metal 慢**：4 路径约 5900 万到 8800 万次单元更新 [估算]；标量实现是百毫秒级，NEON 加多线程优化后估计落在几十毫秒级 [估算]，能否稳定 20 fps 取决于实现和机型，必须实测。对"跨平台复用"这个目标，iOS 只需要能用且结果一致，不必追平 GPU。
- **自动标定的 CPU 开销**（第 6.1 节）。
- **对角双摄（iPhone 13–15）的滚转加回转**：深度会丢掉画面四角；`t_z ≈ 0` 这个前提不成立时是近似滚转，测试里要覆盖。
- **Metal 的 fast-math**：浮点结果不是严格 IEEE，只能统计对比。
- **RK3568 等 ARM 小核**会明显比手机和桌面慢，需要单独基准。
- **没有 Mac**：Swift 和 Metal 的编译错误、真机行为都只能靠 CI 和用户手测反馈，迭代慢，请把改动拆小。
- **未读文件**（第 12 节）里可能有与本文推断相矛盾的事实，以实际代码为准。

---

## 14. 交付约定（用户的偏好）

- 代码交付时，**由用户指定具体要哪个文件，只给单个文件**，不要打包成压缩包。
- 修改代码时给**完整的文件版本**，并加必要的注释。
- 给 Python 脚本时，**所有命令行参数都要有默认值，集中放在文件里的 `DEFAULTS` 字典**，直接运行不用输参数（原来的 `--xxx` 仍可覆盖）。
- 回复使用中文。


---

# 15. §12 核对结果（agent 已执行）

> 执行环境：Windows，`E:\lfm\stereo-camera-iphone`，分支 `test`（从 `main` 拉出）。
> Python 用 `C:\Users\EDY\.conda\envs\segzoo\python.exe`（3.10.22 + numpy 1.26.4）；
> C++ 用 MSVC 14.51（VS 18 Community）+ CMake。本节的 [已确认] 都是读过源码或跑过代码得到的，可复现。

## 15.1 一句话结论

**移植路线可行，而且比 §13 预估的乐观**：C++ 标量版与 numpy 孪生实现**逐位一致**（三个尺寸，含 640×360/D=64），
跨平台逐位一致性这条最容易被质疑的前提已经被实测证实。但要改三条判断：

1. **不要删 `Analyzer.swift`**（§9 的推断错了）：`3D segmentation` 和 `Bird's-eye` 都不依赖分割模型，
   数据全部来自 `Analyzer.grid()` 的深度连通域。删掉它 = 删掉一个**纯传统、已写好、零模型依赖、
   且最适合搬 C++** 的模块。建议只删 `Bird's-eye`，保留 `3D segmentation`。
2. **CI 不跑 xcodegen**：三个 workflow 都直接编译仓库里提交的 `.xcodeproj`。只改 `project.yml`
   而不重新生成工程，CI 编译不到 `core/`，**而且不会报错**——只会用旧工程文件静默成功。
3. **P2 的匹配层对拍不需要等 App 改导出**：`synthetic_scene()` 的左右图本身就是"已矫正"的输入。
   真机导出只在验证 rectify/unrectify/几何时才需要。

## 15.2 §12 逐条答案

### Q1 `Analyzer.swift`（167 行）[已确认]

| 内容 | 归类 |
|---|---|
| `TrackedObject`（id/rect/label/confidence/lastSeen/mask/distance/x/y/width/height） | 物体跟踪 + 尺寸测量 |
| `Blob`（id/rect/distance/x/width/cells） | 深度连通域块（3D 分割与俯视图共用） |
| `Analysis`（objects/blobs/scan/labels/gridW/gridH/center） | 每帧的分析结果包 |
| `Analyzer.analyze` | 壳：`associate` → `measure` → `grid`，另外算 `a.center` |
| `Analyzer.associate` | IoU 跟踪（阈值 0.25，位置/尺寸按 0.6 插值），0.7 s 未出现即丢弃 |
| `Analyzer.measure` | 掩码内深度中值（无掩码时用框内 30% 分位数）→ 距离 + 针孔模型尺寸 |
| `Analyzer.grid` | **纯数组运算**：`step = max(1, W/160)` 降采样成 160×90；`scan` 取中 40% 行的最小深度；4 邻域深度连通域（容差 `0.02+0.04z`），保留 ≥40 格的前 24 个 |

依赖关系（grep 全量核对）：

- **点选测距不依赖 Analyzer**：`AppModel.sampleTap()` 直接调 `StereoFrame.sample(rect, q:)`。
- **十字准星读数依赖**：`a.center = f.sample(...)` 就写在 `analyze()` 里（删了要挪走这一句，不能直接丢）。
- `Renderer.uploadLabels(model.analysis)` 拿 `labels/gridW/gridH` 喂 `texture(3)`（3D 分割着色）。
- `OverlayView` 的 `object/blob/crosshair/radar` 分别吃 objects/blobs/center/scan。
- `Capture.export` 把 `objects` 写进 `meta.json`。

**结论**：`grid()` 是 `3D segmentation` 与 `Bird's-eye` 的唯一数据源，**两者都不使用分割模型**
（模型只影响 `objects`）。删 `Analyzer` 会连带 `AppModel.analysis`、`FrameReport.analysis`、
`Renderer.uploadLabels`、`Capture.export` 的 objects、以及 `OverlayView` 的全部绘图函数。

### Q2 `tools/sgm_ref.py`（803 行）/ `tools/README.md` [已确认，修正 README 的说法]

`tools/README.md` 说"六个 kernel 的孪生实现"，实际是 **7 个**（docstring 的对照表列了 8 项，
其中 `rectify` 明写"本文件未翻译"）：

| 已翻译 | 未翻译 |
|---|---|
| `census` / `cost_volume` / `sgm_path` / `wta_right` / `wta_left` / `to_depth` / `match_block` | **`rectify`**、**`unrectifyDepth`**（9 个计算 kernel 里完全没提）、**`calibrate()`** |

`--selftest` 的粒度（实测全过）：`cost_volume` / `sgm_path`（4 个方向）/ `wta_right` 用 `np.array_equal` 逐元素；
`wta_left` 用 `np.allclose(atol=1e-6)`；另加一个语义用例（相同图 → 全无效）。
尺寸 **9×21、D=8**，随机 uint48 Census + 随机 luma。
**不覆盖** `census()`、`to_depth()`、`match_block()`，也不覆盖真实尺寸。

输入来源：`synthetic_scene(seed)`（**合成数据，不是真机导出**），逐字对应 `scripts/verify_stereo.swift`：
斜面 `d = 8 + y/H*6`、矩形 `d = 30`（x∈[260,420)、y∈[110,250)）、y∈[200,330) 一条对比度 4% 的低纹理带。

**关键限制**：`--dump DIR` 输出的是 **12 张 8 位 PNG 可视化图**（00_left / 01_census_popcount /
02_cost_d32 / 03_path_{x+,x-,y+,y-} / 03_pathsum_d32 / 04_disp_right / 05_disp{,_turbo} / 06_depth_turbo），
**只有 8 位且经过归一化**，不能当逐位对拍的向量。
→ 所以新增了 `tools/gen_golden.py` 输出原始 `.npy`（见 §15.3）。

### Q3 `tools/make_demo_export.py`（144 行）[已确认]

输出 6 个文件：`rgb.png`(1280×720) / `depth_mm.png`(640×360, 16 bit) / `view.png`(1280×720) /
`pair.png`(1280×720 左右并排) / `meta.json` / `viewer.html`。

**不含**：矫正后的左右图、`R`/`t` 外参矩阵、单应 `MW/MU/MUp`。`meta.json` 只有 `K(fx,fy,cx,cy)`、
`baseline_mm`、`baseline_angle_deg`。

→ 但 `synthetic_scene()` 返回的 `left`/`right` 本身**就是已矫正的一对**（右图 = 左图按真值视差平移），
所以 **P2 的匹配层对拍完全不依赖 App 导出的矫正图**。

### Q4 `Renderer.swift` / `AppModel.swift` / `ContentView.swift` [已确认]

`PipelineSettings`（AppModel.swift:141-156）字段与默认值：

| 字段 | 默认 | 字段 | 默认 |
|---|---|---|---|
| `matcher` | `.sgm` | `minZ` / `maxZ` | `0.12` / `3` |
| `lrCheck` | `true` | `uniqueness` | `0.1` |
| `p1` / `p2` | `8` / `96` | `maxDisparity` | `64` |
| `yOffset` | `0` | `flip` | `false` |
| `detection` | `true` | `confidence` | `0.5` |
| `orient` | `.landscapeRight` | **`calibrate`** | **`true`** |

⚠️ `calibrate` 默认 `true` → **第一帧就触发自动标定**（`flip`×`dy` 2×25 + 4 次细化 = 54 次
"矫正+Census+匹配"），期间 `Pipeline.stereo` 同步阻塞。这是 §6.1 说的 CPU 最重环节。

视图模式枚举在 `AppModel.swift:9-42`，9 个 case：`standard, depth, overlay, disparity, stereoPair,
anaglyph, segmentation(6), radar(7), pointCloud(8)`。

⚠️ **`shaderIndex = Int32(min(rawValue, 7))` 是个坑**：`radar` 的 rawValue 是 7，映射到
`displayFragment` 的 `default:`（`c * 0.25` 压暗的相机图 + Canvas 叠层画俯视图）。
删掉 `case 6` 时如果让枚举整体前移，`pointCloud` 会变成 7，而 `min(rawValue,7)` 仍返回 7
→ 点云模式被错当成俯视图。改枚举就要同步改 `shaderIndex`。

`screenDims(...)` 只被 `OverlayView.object` 使用；`visionOrientation` 只被 `Detector` 使用。

参数结构体：`DisplayParams`（8×32 位 = 32 B）与 `PCParams`（mvp 64 B + 7×4 B + 3×4 B = 104 B）
**Swift 与 Metal 两端字段一致**，但没有任何编译期断言，全靠手工。
`RectParams / MatchParams / DepthParams / SGMParams` 在 `StereoEngine.swift:411-419` 与
`Shaders.metal:5-10,125` **各写一份**——正是 §4 建议合并到共享头的对象。

### Q5 `Capture.swift`（241 行）[已确认]

`Capture.export(_:_:)`（Capture.swift:166-231）写 `view.png` / `rgb.png` / `depth_mm.png` /
`cloud.ply` / `meta.json`。

点云是 **CPU 生成**（`ply()`，Capture.swift:214-231）：逐像素 `String(format: "%.4f %.4f %.4f %d %d %d\n", ...)`
拼 ASCII。640×360 有效像素约 60% → 约 14 万行、每行一次格式化调用，**这是拍照导出卡顿的主因**，
适合下沉 C++（相机模型 + 格式化都是纯数学，BGRA 取样是 CoreVideo 的 C API）。
建议 C++ 只产字节缓冲，Swift 负责写文件。

### Q6 `Detector.swift` / `OverlayView.swift` [已确认]

- Vision 回退 `runBuiltin()`（Detector.swift:276-299）用 `VNDetectHumanRectangles` /
  `VNRecognizeAnimals` / `VNGenerateObjectnessBasedSaliency` + `VNClassifyImageRequest`，
  **不需要任何模型文件**，但严格说它们也是 OS 内置的神经网络。
- **保留回退 = 必须保留 `Analyzer` 的 tracked-object 那一半**（`Detection` → `associate` → `measure`
  → `TrackedObject`，约 80 行）。**要么全删（推荐），要么全留，不能只删推理留回退。**
- ⚠️ 两个**跨文件符号**藏在 `Detector.swift` 里，删它必须一并处理：
  - `labelHue(_:)`（Detector.swift:24）被 `OverlayView.swift:134` 的 `color(_:)` 使用；
  - `iou(_:_:)`（Detector.swift:302）被 `Analyzer.swift:62` 的 `associate` 使用。
- 尺寸测量/跟踪的依赖范围：`measure` 只需 `TrackedObject.rect` + `.mask`；`Mask` 类型定义在 Detector.swift，
  同时被 `StereoEngine.sample(_ m: Mask)` 使用 → 删 Detector 会连带删掉 `StereoFrame.sample(_ m: Mask)`。
  `StereoFrame.sample(_ r: CGRect, q:)` 是点选测距/准星/measure 共用的，**必须保留**。

### Q7 `project.yml` / CI [已确认]

本地 `project.yml`（2218 B）已读全文：tab 缩进、`sources: - path: StereoVision, excludes: ["Models/**"]`、
`preBuildScripts` 用 `coremlcompiler` 编译模型（找不到时只打 warning）、`SWIFT_VERSION: "5.0"`、
`MTL_FAST_MATH: YES`、`TARGETED_DEVICE_FAMILY: "1"`、iOS 18。与上游 1.89 KB 的精确 diff 需要
`git fetch` 上游仓库，本次未做（离线）。

**CI 现状（重要）**：

| 文件 | 触发 | 关键事实 |
|---|---|---|
| `codemagic.yaml` | 手动 | `xcodebuild -project StereoVision.xcodeproj -target StereoVision`，**不跑 xcodegen** |
| `.github/workflows/ipa.yml` | push main（`StereoVision/**`、`Config/**`、`project.yml`）+ 手动 | **不跑 xcodegen**；可选 `with_yolo` 先导出 YOLO 再打包 |
| `.github/workflows/export-model.yml` | push `models/*.pth` / `scripts/export_lens_seg.py` + 手动 | ubuntu 导 mlpackage → macos-15-intel 打包 IPA |

→ **`.xcodeproj` 是提交进仓库的**，三个流水线都直接用它。所以 **改 `project.yml` 后必须在本地
`xcodegen generate` 并把生成的 `.xcodeproj` 一起提交**，否则 CI 用旧工程文件，
新增的 `core/src/*.cpp` 根本不会被编译，而且不报错。

删模型后要改的：`project.yml` 的 `preBuildScripts` 与 `excludes: Models/**`；
`ipa.yml` 的 `model` job、`with_yolo` 输入、Fetch/Unpack 步骤、`find -name "*.mlmodelc"`；
`export-model.yml` 整个删除；`codemagic.yaml` 无需改。`scripts/fetch_ipa.sh` 若按 workflow 名过滤也要跟着改。

在 `sources` 里加 `core/` 需要的 XcodeGen 设置：

```yaml
targets:
  StereoVision:
    sources:
      - path: StereoVision
      - path: core/src
      - path: core/include
        headerVisibility: public      # 不加会把 .h 当资源/源码处理
    settings:
      base:
        SWIFT_OBJC_BRIDGING_HEADER: StereoVision/StereoVision-Bridging-Header.h
        HEADER_SEARCH_PATHS: $(SRCROOT)/core/include
        CLANG_CXX_LANGUAGE_STANDARD: c++20
        CLANG_CXX_LIBRARY: libc++
        GCC_ENABLE_CPP_EXCEPTIONS: NO
```

⚠️ MSVC 必须加 `/utf-8`：源码是 UTF-8 无 BOM（clang/gcc 默认），而 MSVC 在中文 locale 下默认按 GBK 读，
会把中文注释按多字节解析并吞掉下一行（本次已踩到，见 §15.3(c)）。

### Q8 待用户回答

1. 你在另一个项目里的 Python SGM 在哪个仓库/路径？是否与 `sgm_ref.py` 同一算法
   （Census 7×7、4 路径、自适应 P2、左右检查）？差异点（路径数 4 还是 8、代价函数、窗口大小、
   P1/P2 策略、是否做唯一性检查）？
2. 该项目的输入规格（基线约 65 mm、1280×1080？），需要 C++ 库暴露哪些可配置项？
3. `3D segmentation` 是否确认要删？（见 §15.1 第 1 条：它不依赖分割模型，删它是净损失）

## 15.3 本轮实测结果

### (a) numpy 孪生实现可复现 [已确认]

```
python tools/sgm_ref.py --selftest   → 8 项全 OK（cost_volume / sgm_path×4 / wta_right / wta_left / 语义用例）
python tools/sgm_ref.py              → 3.4 s
```

| 算法 | 本机实测 | README 记录 |
|---|---|---|
| Block 5×5 | 62.3 / 6.4 / 31.4 | 62.2 / 6.5 / 31.2 |
| SGM-4 | 75.8 / 13.6 / 10.6 | 75.2 / 13.9 / 10.8 |
| SGM-4 + LR | 72.9 / 9.1 / 18.0 | 72.4 / 9.3 / 18.3 |

（`synthetic_scene` 加了可选 `w`/`h` 参数，默认值下逐位不变——上表与改动前完全一致。）

### (b) 新增 `tools/gen_golden.py` → 原始测试向量 [已确认]

`sgm_ref.py --dump` 只有 8 位 PNG，不能对拍。新脚本输出 `.npy`（未压缩、C 顺序、小端）：

| 文件 | 形状 | 用途 |
|---|---|---|
| `left_u8` / `right_u8` | (H,W,3) uint8 | 已矫正的一对输入 |
| `luma_f32` | (H,W) float32 | 亮度 |
| `censusL_u64` / `censusR_u64` | (H,W) uint64 | Census（bit k = 第 k 个邻居） |
| `cost_u8` | (H,W,D) uint8 | 代价体积 |
| `sum_u16` | (H,W,D) uint16 | 4 条路径聚合结果 |
| `dispR_f32` / `disp_f32` / `depth_f32` | (H,W) float32 | WTA 右/左、深度 |
| `truth_f32` | (H,W) float32 | 视差真值（供后续评测） |

三档尺寸：`tiny` 64×48 D=16（**0.28 MB，已提交**，覆盖边界/首末路径/WTA 分支）、
`small` 320×180 D=32、`full` 640×360 D=64（后两者在 `.gitignore` 里，本地生成）。

### (c) C++ 标量实现 vs 金标准：**逐位一致** [已确认]

`core/` 已建立（`include/stereo/sgm.hpp`、`src/sgm.cpp`、`tests/{npy.hpp,golden_main.cpp}`、`CMakeLists.txt`），
实现 `rgb_to_luma` / `census` / `cost_volume` / `sgm_path` / `sgm_all_paths` / `wta_right` / `wta_left` / `to_depth`。

```
cmake -S core -B build/core -G "Visual Studio 18 2026" -A x64
cmake --build build/core --config Release
./build/core/Release/golden_test.exe core/tests/golden        # 或 golden-small / golden-full
```

12 项检查（逐 kernel 6 项 + 从 uint8 输入整链 6 项）在 **tiny / small / full 三个尺寸上全部逐位一致**，
含真机工作分辨率 640×360、D=64。

**踩到的坑**：`rgb_to_luma` 一开始写成 `u * (1/255f) * w`，与 numpy 的 `u / 255f` 差 **3 ULP**
（769/3072 个像素不同）。改成真除法后 **0 差异**。浮点常量的写法会直接破坏逐位对拍，
这类地方要逐一按 numpy 的表达式抄。

### (d) 标量基准（x64 桌面，单线程，不含 IO）[已确认 → 替换 §13 的估算]

| 阶段 | ms |
|---|---|
| `census` ×2 | 51.4 |
| `cost_volume` | 9.1 |
| `sgmPath` ×4 | 87.8 |
| `wta` R+L | 51.6 |
| `toDepth` | 11.6 |
| **合计** | **211.5 → 4.7 fps 上限** |

§13 说的"标量实现是百毫秒级"得到证实。优化优先级（收益/风险）：
1. `census` + `wta`（103 ms）几乎全是钳位索引与逐像素分支 → 预计算 7 个钳位偏移表，
   语义不变、零风险；
2. `sgmPath`（88 ms）→ 沿扫描线串行、对视差维 SIMD（D=64 正好 4×128 bit 或 2×256 bit），
   水平路径按行分线程、竖直路径按列分线程；
3. 之后再做多线程与条带化，避免跨核带宽瓶颈。

## 15.4 修正后的 P0 清理清单

| # | 动作 | 风险 |
|---|---|---|
| 1 | 删 `Detector.swift`；连带删 `Mask`、`StereoFrame.sample(_ m: Mask)`、`OverlayView.object/color`、`SettingsView` 的 Detection 段、`AppModel` 的 detection/confidence/showMasks/detectorName | 中：跨文件符号见 Q6 |
| 2 | 删 `StereoVision/Models/`、`models/`、`scripts/export_lens_seg.*`、`.github/workflows/export-model.yml`、`project.yml` 的 preBuildScripts | 低 |
| 3 | 删 `ViewMode.radar` + `OverlayView.radar` + `Analysis.scan` + `blob.cells` 的使用点 | 低 |
| 4 | **保留** `ViewMode.segmentation`（建议，见 §15.1） | — |
| 5 | `ipa.yml`：去掉 `model` job / `with_yolo` / Fetch+Unpack / `find *.mlmodelc` | 低 |
| 6 | `project.yml` 加 `core/`（Q7 的片段），本地 `xcodegen generate`，**把 .xcodeproj 一起提交** | 中：不提交则 CI 编译不到 core |
| 7 | App 增加导出：矫正后的左右图 + 完整 `K/R/t/MW/MU/MUp`（P3 才需要，先进 `meta.json` 省返工） | 低 |
| 8 | `README.md` 同步（功能表、视图模式表、分割章节） | 低 |

每步一个 commit，推 `test` 分支看 CI（`test` 不在 `ipa.yml` 的触发分支里，需要手动
`workflow_dispatch` 或临时把分支加进 `paths`/`branches`）。

## 15.5 下一步建议

1. 先做 §15.4 的 1/2/3/5（不碰 Xcode 工程结构），把"纯传统深度"的形态定下来；
2. 再做 6（把 `core/` 接进工程，实现 `sv_match` C 接口 + 第四个深度来源「自研（C++ CPU）」），
   在真机上与 Metal 版并排比帧率和有效像素比例——这是 §11 P4 的验收点；
3. `census`/`wta` 的偏移表优化 + `sgmPath` 的 SIMD，随时用 `golden_test` 守住逐位一致。

## 15.6 交付物清单（本轮新增/修改）

| 路径 | 状态 | 说明 |
|---|---|---|
| `test.md` | 修改 | 本节（§15） |
| `tools/gen_golden.py` | 新增 | 金标准向量生成器，`DEFAULTS` 约定，无参数可跑 |
| `tools/sgm_ref.py` | 修改 | `synthetic_scene(seed, w, h)` 支持小尺寸；默认值下逐位不变 |
| `core/CMakeLists.txt` | 新增 | 静态库 + `golden_test` + ctest |
| `core/include/stereo/sgm.hpp` | 新增 | 8 个函数声明 + `SgmParams` |
| `core/src/sgm.cpp` | 新增 | 标量参考实现（逐 kernel 对齐 MSL） |
| `core/tests/npy.hpp` | 新增 | 极简 .npy 读取 |
| `core/tests/golden_main.cpp` | 新增 | 12 项逐位对拍 + 标量基准 |
| `core/tests/golden/*.npy` | 新增 | tiny 向量（0.28 MB，提交） |
| `.gitignore` | 修改 | 忽略 `core/tests/golden-small/`、`golden-full/` |

---

# 16. P0 执行记录（agent）

按 §15.4 执行，`test` 分支上 4 个提交。**没有 Mac，Swift 部分未编译过**——
只能做静态核对（符号引用、括号配平、跨文件依赖），编译错误要由 CI 暴露。

## 16.1 已完成的删除

| 动作 | 提交 | 说明 |
|---|---|---|
| 删 `Bird's-eye` | `0e0ffd0` | `ViewMode.radar`、`OverlayView.radar()`、`Analysis.scan`、`Analyzer.grid()` 的剖面计算 |
| `shaderIndex` 改成显式映射 | `0e0ffd0` | 原来 `min(rawValue, 7)` 靠 radar 恰好占 7 才凑对 |
| 删语义分割推理链路 | `0e0ffd0` | `Detector.swift`、`Mask`、`Detection`、`TrackedObject`、`StereoFrame.sample(_ m: Mask)`、`Analyzer.associate/measure`、`labelHue`、`iou`、`screenDims`、`visionOrientation` |
| 删 UI 相关项 | `0e0ffd0` | 侧栏 Detection 按钮、设置页「Object detection」分区、物体框/掩码叠层、`showMasks`/`confidence`/`detection`/`detectorName` |
| 删模型文件与流水线 | `d8e6ca3` | `StereoVision/Models/`、`models/`、`scripts/export_lens_seg.*`、`export-model.yml`、`project.yml` 的 preBuildScripts |

**保留**：`3D segmentation`（`.segmentation`）、`Analyzer.grid()` 的连通域与标签图、
`StereoFrame.sample(_ r: CGRect, q:)`（点选测距/准星）、三种深度来源、点云、拍照录像与导出。

## 16.2 顺带修掉的问题

- `ViewMode.shaderIndex` 的隐式 `rawValue` 依赖（见 §15.2 Q4 的坑）。
- `ipa.yml`：`paths` 加 `core/**`，`branches` 加 `test`；`fetch_ipa.sh` 默认 workflow 改成 `Build unsigned IPA`。
- `meta.json`：`objects` 置空以兼容旧格式，新增 `blobs`（深度连通域面片块：id / 距离 / x / 宽度 / 格数 / 归一化框）。

## 16.3 仍未做（下一步）

| # | 事项 | 备注 |
|---|---|---|
| 1 | **`xcodegen generate` 并提交 `.xcodeproj`** | 本次没做：本机没有 xcodegen，也没有 Mac。这是接 `core/` 的前提，见 §15.2 Q7 |
| 2 | `project.yml` 加 `core/src` + `core/include` + 桥接头 / C++ 标准 | 片段已写在 §15.2 Q7，等第 1 步一起做 |
| 3 | C API（`sv_match` 等）+ 第四个深度来源「自研（C++ CPU）」 | §11 P4 |
| 4 | App 导出矫正后左右图 + 完整 `K/R/t/MW/MU/MUp` | §11 P0 的剩余项，P3 才用得上 |
| 5 | `census`/`wta` 钳位偏移表 + `sgmPath` SIMD | 见 `core/README.md` 的基准与顺序 |

## 16.4 静态核对结果（无 Mac，替代编译）

代理能做的检查都做了：

| 检查 | 结果 |
|---|---|
| 删除符号的残留引用（`grep` `Mask`/`Detection`/`detector`/`showMasks`/`radar`/`scan`/`labelHue`/`iou`/`screenDims`/`visionOrientation`） | 干净 |
| 6 个改动过的 Swift 文件的 `{} / () / []` 配平（剥掉字符串与注释后计数） | 全部配平 |
| 跨文件符号（`Blob`/`Analysis`/`Analysis.blobs`/`Snapshot.analysis`/`Unchecked`） | 定义与使用一致 |
| `core/` 的 C++ 逐位对拍（3 个尺寸） | 全部通过 |
| 模型/CI 残留引用（`grep` `mlpackage`/`mlmodel`/`export-model`/`with_yolo`/`coremlcompiler`） | 干净 |

**第一次可能失败的地方**（按概率排序，供 CI 报错时对照）：

1. `project.yml` 与提交的 `.xcodeproj` 不同步 —— 如果有人在 `main` 上重新生成过工程，
   `test` 分支合并时会冲突；反之如果没重新生成，CI 用旧工程照样能编（但编不到 `core/`）。
2. `ContentView` 的设置页删掉了一个 `Section`，`Form` 里的顺序变了 —— 语法上没问题，
   但注意 `Section("Stereo matching")` 后面的 `.disabled(model.source != .stereo)` 修饰符仍然挂在它身上（正确）。
3. `Capture.export` 里 `meta["objects"] = [] as [[String: Any]]` 的写法；
   如果 `JSONSerialization` 对空数组的桥接有问题，改成 `[Any]()` 即可。

---

# 17. 三层框架已跑通（CI 验证通过）

## 17.1 现在的结构

```
第一层  C++（由 Apple clang 编译）
  core/include/stereo/stereo.h     纯 C 接口：POD 结构体 + 不透明句柄，唯一对外契约
  core/include/stereo/sgm.hpp      C++ 内部声明（含 SgmParams）
  core/src/sgm.cpp                 census / costVolume / sgmPath×4 / wtaRight / wtaLeft / toDepth
  core/src/stereo_api.cpp          C 门面：参数校验 + 缓冲区按尺寸复用 + 翻译成算法调用

第二层  C / Objective-C++（中间层）
  StereoVision/StereoVision-Bridging-Header.h   Swift 只看这里（不许出现任何 C++ 类型）
  StereoVision/Bridge/StereoCoreBridge.h        纯 ObjC 接口（只 import Foundation）
  StereoVision/Bridge/StereoCoreBridge.mm       持有 stereo_context、翻译 stereo_image、错误转 NSString

第三层  Swift（前端）
  StereoVision/AppModel.swift      runCoreSelfTest()：取像素 → 调中间层 → 报耗时/有效率
  StereoVision/StereoEngine.swift  StereoFrame 暴露 pairLeftPB（已矫正左图）与 dir（匹配方向）
  StereoVision/ContentView.swift   设置页「C++ core」一节，结果写进 HUD
```

调用链是一次真实的穿透，不是占位：

```
Swift（锁 CVPixelBuffer / MTLTexture.getBytes）
  → SVStereoCore.matchLeft(...)          ObjC++ 层，负责生命周期与像素格式
    → stereo_match(...)                  纯 C 接口
      → census → cost_volume → sgm_all_paths → wta_right → wta_left   C++ 算法
```

## 17.2 CI 结果（2026-11-25，commit `03dd88c`）

| 流水线 | 结果 | 内容 |
|---|---|---|
| **Build C++ core** | ✅ 三平台全绿 | `ubuntu-latest` / `windows-latest` / `macos-15-intel` 各编译一遍，12 项逐位对拍 + C API 冒烟全过 |
| **Build unsigned IPA** | ✅ 成功 | Apple clang 编译 `sgm.cpp` + `stereo_api.cpp`，ObjC++ 桥接，Swift 前端，产出未签名 IPA |

三个平台的标量基准（tiny 向量 64×48/D=16，仅供参考机器快慢）：

| runner | 合计 |
|---|---|
| ubuntu-latest | 1.58 ms |
| windows-latest | 2.13 ms |
| macos-15-intel | 1.36 ms |

**这一步验证了最关键的三个前提**：

1. C++ 核心在 **Apple clang + iOS SDK** 下能编（CI 的 IPA job 真的编了 `sgm.cpp` / `stereo_api.cpp`）；
2. 纯 C 头能同时被 **C 编译器**（`c_api_smoke.c`）、**ObjC++**（桥接的 `.mm`）、**Swift 桥接头**使用；
3. 同一份 C++ 代码在 Windows / Linux / macOS 三平台输出**逐位相同**。

## 17.3 这一轮踩到并修掉的坑

| 问题 | 现象 | 修法 |
|---|---|---|
| `.xcodeproj` 陈旧 | `Build input file cannot be found: Detector.swift` | CI 不跑 xcodegen，删源码必须同步改 pbxproj（删 4 处引用 + CoreML 的 shell script phase） |
| `gnu++14` | `inline constexpr` 编译失败 | 工程级 `CLANG_CXX_LANGUAGE_STANDARD = gnu++20` |
| `stereo_match` 漏了 `disp_stride <= 0` 的默认值 | 只有最后一行被写入，C API 对拍全错 | 补 `if (disp_stride <= 0) disp_stride = W;` |
| `max_disp` 校验过严（只允许 32/64/96） | tiny 向量（D=16）跑不了 | 库层放宽成 4..96；三档是 App 的 UI 约定，不是算法约束 |
| MSVC 只给 C++ 目标加了 `/utf-8` | `c_api_smoke.c`（纯 C）中文注释把字符串字面量"吃掉" | 改成全局 `add_compile_options(/utf-8)` |
| `'/*' within block comment` | `stereo.h` 头部注释里写了 `core/src/*.cpp` | 改成「core/src 下的 .cpp」 |
| Swift 6 并发警告 | `Task.detached { [weak self] ... }` 捕获 `self` | 改成 `Task { @MainActor in ... await Task.detached { Static } .value }`，detached 里不捕 self |

## 17.4 一个容易错的细节：两幅配准图的字节序**不同**

同一个 `rectify` kernel 写给两种纹理，但格式不一样：

| | 来源 | 纹理格式 | 内存字节序 |
|---|---|---|---|
| 左图 | `leftPool` 的 `CVPixelBuffer`（32BGRA） | `bgra8Unorm` | **B,G,R,A** |
| 右图 | `slot.right` | `rgba8Unorm` | **R,G,B,A** |

所以 `stereo_image.format` 必须逐图声明（左 `BGRA8`、右 `RGBA8`）。灰度测试图上无所谓，
真实彩色帧上通道搞反会让亮度算错、Census 位翻转。这也是为什么 C 接口的格式字段是**每幅图一个**，
而不是整个 context 一个。

## 17.5 还没做的（下一步）

| # | 事项 | 说明 |
|---|---|---|
| 1 | **独立静态库 target `StereoCore`** | 现在 C++ 是直接编进 App target（Apple clang 编的，但没有单独的 `.a`）。CMake 在桌面端已经产出 `libstereo_core.a`。要做成 Xcode 里的 static-library target，需要新增 PBXNativeTarget + 依赖 + Frameworks phase（本项目 CI 不跑 xcodegen，只能手写 pbxproj 或由你在有 xcodegen 的机器上生成） |
| 2 | 接成**第四个深度来源**「自研（C++ CPU）」 | 现在自检按钮跑完就丢；正式接入要把 `SVStereoCore` 挂到 `Pipeline` 上复用 context，并把 `disp` 上传成纹理走现有的 `toDepth` + `unrectifyDepth` |
| 3 | 真机自检数据 | 上面三个基准都是 CI runner；iPhone 上的标量耗时还没测（README 的 GPU 版是 20–22 fps） |
| 4 | `match`（5×5 块匹配）与 `calibrate` 的 C++ 版 | 见 §15.2 Q2：`sgm_ref.py` 也没翻译 `rectify` / `unrectifyDepth` / `calibrate` |
| 5 | `census` / `wta` 偏移表 + `sgmPath` SIMD | 见 `core/README.md` 的基准与顺序 |
