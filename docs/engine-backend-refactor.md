# StereoEngine 后端解耦：改造记录与验收清单

> 本次改造按 Phase 0–7 执行（Phase 0 基线见 `docs/engine-backend-baseline.md`）。
> 落点：`StereoVision/StereoEngine.swift`（481 行）→ `StereoVision/Engine/`（22 个文件）。
> 改造**前**的调用关系 / 线程 / 资源生命周期 / 计时口径记录在
> `docs/engine-backend-baseline.md`，本文只写"改成什么样、凭什么等价、怎么验"。

## 1. 目录与职责

```
StereoVision/Engine/
├── StereoEngine.swift            唯一编排者（237 行）
├── StereoTypes.swift             StereoWork / StereoGeometry / StereoFrame
├── StereoConfiguration.swift     StereoMode（.metal / .cppHybrid）+ settings.stereoMode
├── Input/FrameInputProvider.swift        CMSampleBuffer → 纹理 + 内参 + CameraRig
├── Geometry/StereoGeometrySolver.swift   SVRectification.solve（core/src/geometry.cpp）
├── Rectification/
│   ├── RectificationBackend.swift        协议（编码进调用方的 encoder）
│   └── MetalRectifier.swift              rectify kernel
├── Matching/
│   ├── MatchingBackend.swift             协议 + MetalMatchInput
│   ├── MetalSGMMatcher.swift             costVolume / sgmPath×4 / wta（含自己的缓冲）
│   ├── MetalBlockMatcher.swift           5×5 块匹配
│   └── CppSGMMatcher.swift               core.matchLeft
├── Readback/CPUImageReadback.swift       像素读回适配（锁 buffer + getBytes + 视差缓冲）
├── Depth/
│   ├── DepthBackend.swift                MetalDepthBackend 协议
│   ├── MetalDepthProcessor.swift         toDepth → unrectifyDepth
│   ├── CppDepthProcessor.swift           core.depth + unrectifyDepth
│   └── SystemDepthProcessor.swift        AVDepthData 路径（不是 SGM 后端）
├── Calibration/StereoCalibrator.swift    C++ 候选搜索 + GPU 打分
└── Metal/
    ├── MetalContext.swift                device / queue / texture cache
    ├── MetalPipelines.swift              9 条 compute PSO
    ├── MetalFrameResources.swift         Slot×3 / DepthTarget / left PixelBufferPool
    ├── MetalDispatch.swift               dispatch 样板
    └── MetalShaderTypes.swift            kernel 参数结构（ABI）
```

没有创建 `CppRectifier`：Bridge 没暴露 `stereo_rectify_image`，没有可用的 C++ 矫正入口。
没有创建 ONNX matcher：本次不接。系统深度**不在** `StereoMode` 里，它是独立数据源。

## 2. 迁移映射（原文件内容 → 新归属）

| 原 `StereoEngine.swift` | 现在 | 等价性说明 |
|---|---|---|
| `StereoFrame` | `StereoTypes.swift` | 字段、初始化参数、默认值、`retained` 语义逐字照搬 |
| `DepthTarget` | `Metal/MetalFrameResources.swift` | 逐字照搬（对齐、shared buffer、linear texture） |
| `device` / `queue` / `cache` / `texture(_:writable:)` | `Metal/MetalContext.swift` | PSO 创建与控制流不变；纹理缓存仍是同一个实例 |
| 9 条 PSO | `Metal/MetalPipelines.swift` | kernel 名、创建顺序、`try!` 行为不变 |
| `costBuffer` / `sumBuffer` / `dispRight` | `Matching/MetalSGMMatcher.swift` | 长度、storageMode、用途不变（SGM 独占） |
| `Slot` ×3 / `appleTargets` / `leftPool` / `slotIndex` | `Metal/MetalFrameResources.swift` | 轮换顺序、3 份数量、`(i+1)%3`、共享 slotIndex 全部保留 |
| `rig` / `intrinsics(...)` / `inputs(...)` | `Input/FrameInputProvider.swift` | 内参来源与回退（fov 70 / 108）不变；`CameraRig` 仍由 `CalibrationSource` 提供 |
| `Geometry` / `geometry(...)` | `StereoTypes.swift` + `Geometry/StereoGeometrySolver.swift` | 仍只调 `SVRectification.solve`，Swift 里没有几何公式 |
| `dispatch(...)` | `Metal/MetalDispatch.swift` | 线程数口径、`setBytes` 的 index、网格尺寸完全一致 |
| `encodeRectify` | `Rectification/MetalRectifier.swift` | 参数 `RectParams(M, srcSize=源纹理尺寸)` 不变 |
| `encodeMatch` / `encodeSGM` | `Matching/MetalBlockMatcher.swift` / `MetalSGMMatcher.swift` | 编码顺序、`D` 的取法、路径顺序 (1,0)(-1,0)(0,1)(0,-1)、`first` 标志不变 |
| `stereo(...)` | `StereoEngine.processMetal` | 仍是**一条** command buffer；kernel 顺序 rectify×2 → census×2 → match → toDepth → unrectify |
| `stereoCPU(...)` | `StereoEngine.processCppHybrid` + `CppSGMMatcher` + `CppDepthProcessor` + `CPUImageReadback` | 同步点数量不变（rectify 一个、之后全同步）；`core.configure` 仍在分配缓冲之前 |
| `score(...)` / `calibrate(...)` | `Calibration/StereoCalibrator.swift` | 打分仍用 `slots[0]`，编码顺序仍是 `rectify(W)→census→rectify(U)→census→match` |
| `depthData(...)` | `Depth/SystemDepthProcessor.swift` | 只做 `filterDepth`；K 仍来自 `CalibrationSource.depthIntrinsics` |
| `RectParams` / `MatchParams` / `DepthParams` / `SGMParams` | `Metal/MetalShaderTypes.swift` | 字段与 `Shaders.metal` 一致（未改 shader） |
| `stereo` / `stereoCPU` 两个入口 | `processStereoFrame(_:_:_:mode:)` | `Pipeline` 用 `settings.stereoMode` 选择；流程只有一份 |

## 3. 计时口径（未变）

| 字段 | 现在由谁产生 | 范围 |
|---|---|---|
| `gpuMs` | `processMetal` | 整条 command buffer 的 `gpuEndTime - gpuStartTime` |
| `gpuRectifyMs` | `processCppHybrid` | 第一个 command buffer（两路 rectify） |
| `readbackMs` | `processCppHybrid` 包住 `CPUImageReadback.make` | 锁 buffer + 两次临时分配 + `getBytes` |
| `matchMs` | `processCppHybrid` 包住 `CppSGMMatcher.match` | `core.matchLeft` |
| `depthMs` | `processCppHybrid` 包住 `CppDepthProcessor.makeDepth` | 临时缓冲分配 + `depth` + `unrectify` |
| `cpuMs` | `StereoFrame.cpuMs` | 上面三项之和 |

## 4. 未改动的东西（硬约束）

- C ABI（`stereo_c_api.h`）、C++ 算法（`core/src/**`）、`stereo_process_frame`；
- `Shaders.metal`（kernel 源码一字未动）；
- 像素格式 / 跨度约定：左图 `32BGRA` + `CVPixelBuffer` 的跨度，右图 `.rgba8Unorm` + `w*4`；
- 工作分辨率 640×360、最大视差 96、cost buffer 尺寸；
- `StereoFrame` 的字段与 `retained` 生命周期；
- 诊断页文案与 `PipelineKind`；
- 线程模型：帧处理仍只在 `CameraController.dataQueue` 上串行；主线程读 `depthPointer`
  与下一帧写入之间没有显式同步（**既有风险，本轮不新增也不修复**）。

## 5. 已知的既有问题（本轮只记录）

1. `configure(wide:ultra:)` 在 `sessionQueue` 写 `CameraRig`，帧处理在 `dataQueue` 读 —— 无同步。
2. 主线程（`AppModel.measure`、`Capture.beginPhoto`、`Renderer`）读 `StereoFrame` 的
   `depthPointer` / 纹理，与相机队列的下一帧写入之间没有显式同步，靠 3 份 slot 轮换兜底。
3. `SVStereoCore` 不是线程安全的：`pipeline` 用一个、`AppModel.queryCore` 用一个，已分开。

## 6. 验收清单

本机（Windows）只能做 C++ 侧的验证；Swift/Metal 需要在 macOS + 真机上跑下面这张表。

### 6.1 已在本机执行

```bash
cmake --build build/core --config Release
cd build/core && ctest -C Release     # 7/7 passed（golden / c_api / geometry / analyze / cloud / calibrate / frame）
bash scripts/check_xcodeproj_sources.sh   # 只能在 macOS 上跑（需要 xcodegen + 生成的工程）
```

C++ 与 shader 未改动，所以 golden / 几何 / 分析 / 标定的逐位结果不受本次重构影响。

### 6.2 真机回归（按顺序）

1. **Metal 模式**（`DepthSource.stereo`，SGM）：Standard / Depth / Overlay / Disparity /
   Rectified pair / Anaglyph / 3D segmentation / Point cloud 八个视图各看一眼。
2. **Metal Block**（设置页 matcher 切 Block）：同一场景下视差图与 SGM 明显不同但合理；
   确认块匹配仍被标定复用。
3. **C++ 混合模式**（`DepthSource.cpuCore`）：诊断页三段计时都有数字，
   `readbackMs + matchMs + depthMs == cpuMs`；与改造前同一场景对比深度图。
4. **系统深度**（Apple Dual-Wide / LiDAR）：深度图、点云、导出正常；不出现视差相关视图。
5. **自动标定**：点 Auto calibrate → 诊断页帧率恢复正常、HUD 的 flip / dy 有更新；
   之后再跑 Metal 与 C++ 两种模式都应正常。
6. **设置 Apply**：改 maxDisparity / p1 / p2 / uniqueness / lrCheck / minZ / maxZ / flip / dy，
   下一帧生效（Metal 走 `MetalSGMMatcher.encodeMatch`，C++ 走 `configure`）。
7. **连续运行与切换**：两种模式来回切、相机中断（切后台/来电）后恢复；
   观察是否出现深度图花屏/错帧（slot 轮换回归）。
8. **导出**：拍照（png + depth_mm + meta.json）、录像、点云 PLY 各一次。

### 6.3 重构前后输出对比（建议）

在改造前后各跑一次固定场景，比较：

- `depth_mm.png` 的逐像素差异（Metal 模式应完全一致；C++ 模式应完全一致）；
- 诊断页的 `gpuMs` / `readbackMs` / `matchMs` / `depthMs`（允许测量波动）；
- `meta.json` 里的 `rectification`（flip / dy / baseline）与 `max_disparity`。

如果 Metal 模式出现差异，优先检查 `MetalDispatch` 的 `setBytes` index 与
`MetalShaderTypes` 的字段顺序；如果 C++ 模式出现差异，优先检查
`CPUImageReadback` 的左右跨度/格式声明。

## 7. 根目录归档与重命名（`stereovision_agent.md` 执行记录）

第二阶段：`StereoVision/` 顶层 14 个 Swift 文件归入子目录并语义化重命名。
**只改路径/符号名/注释，算法、timer、shader 参数、C++ 与 Bridge 均未动。**

| 原路径 | 新路径 | 符号 |
|---|---|---|
| `ContentView.swift` | `StereoVisionApp.swift`（只留 `@main`）+ `UI/RootView.swift` | `ContentView` → `RootView` |
| `ContentView.swift` 里的 `MetalView` | `Rendering/MetalView.swift` | 新增文件（`import MetalKit` 不能留在 UI/） |
| `Contracts.swift` | `Contracts/UIContracts.swift` | `DisplayOrientation` / `SettingsDraft` / `DeviceInfo` / `CaptureUIState` / `CaptureIntents` / `SettingsIntents` / `GpuReport` / `DiagnosticsSnapshot` |
| `Contracts.swift` + `Pipeline.swift` 的数据结构 | `Contracts/PipelineContracts.swift` | `ViewMode` / `Matcher` / `PipelineSettings` / `PipelineKind` / `StageTimings` / `FrameReport` / `FrameStore` |
| `Pipeline.swift` | `App/FramePipeline.swift` | `Pipeline` → `FramePipeline`（只剩调度） |
| `AppModel.swift` | `App/AppModel.swift` | 不变 |
| `CameraController.swift` | `Camera/CameraSessionController.swift` | `CameraController` → `CameraSessionController`（`DepthSource` 不变） |
| `Calibration.swift` | `Camera/CameraCalibrationReader.swift` | `CalibrationSource` → `CameraCalibrationReader` |
| `Analyzer.swift` | `Analysis/DepthSceneAnalyzer.swift` | `Analyzer` → `DepthSceneAnalyzer` |
| `Renderer.swift` | `Rendering/MetalSceneRenderer.swift` | `Renderer` → `MetalSceneRenderer` |
| `Capture.swift` | `Rendering/MediaExportManager.swift` | `Capture` → `MediaExportManager` |
| `CaptureScreen.swift` / `SettingsScreen.swift` / `DiagnosticsScreen.swift` / `OverlayView.swift` | `UI/` 同名 | 不变 |

### 7.1 结果结构

```
StereoVision/
├── StereoVisionApp.swift      @main
├── Shaders.metal
├── Bridge/  Engine/           保持原样
├── Contracts/  UIContracts.swift · PipelineContracts.swift
├── UI/         RootView · CaptureScreen · SettingsScreen · DiagnosticsScreen · OverlayView
├── App/        AppModel · FramePipeline
├── Camera/     CameraSessionController · CameraCalibrationReader
├── Analysis/   DepthSceneAnalyzer
└── Rendering/  MetalSceneRenderer · MetalView · MediaExportManager
```

### 7.2 依赖规则与验证

- `UI/`：只 `import SwiftUI`；grep `import (AVFoundation|MetalKit)|StereoEngine|SVStereoCore` 为空。
- `Contracts/`：只 `import CoreGraphics/Foundation`；grep `import (SwiftUI|AVFoundation)` 为空。
- 旧类名（`CameraController` / `CalibrationSource` / `Analyzer` / `Renderer`）在全仓 `StereoVision/` 下零残留（含注释）。
- `Engine/` 未反向依赖 UI/App/Rendering/Analysis；唯一跨层是 `Engine/Input`、`Engine/Depth` 消费
  `Camera/CameraCalibrationReader` 的**标定数据**（`Intrinsics` / `CameraRig`），这是几何求解的输入，不涉及会话/设备。
- `project.yml` 的 `sources: - path: StereoVision` 是递归的，新增子目录无需改工程；`scripts/check_xcodeproj_sources.sh` 的 `COMPILED_DIRS` 仍覆盖。
- 验证：`ctest` 7/7（C++ 未动）；Swift 侧由 CI 的 `xcodegen generate` + `xcodebuild` 编译。
