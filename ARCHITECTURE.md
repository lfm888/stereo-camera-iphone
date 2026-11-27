# 架构地图

> 用途：改代码 / 删文件之前先看这一页。行数是 `wc -l` 实测值（2026-11）。
> 代码量 ≈ 9.5k 行：C++ 实现 2.3k、C++ 测试 2.4k、Swift 3.8k（其中后端拆分后的 `Engine/` 1.4k）、ObjC++ 桥 1.2k、Metal 0.4k。
> 结论先放这里：**iOS 上没有任何一段 C++ 算法是被绕过的**，反向也不成立——C++ 从不持有平台指针。

## 0. 五层，依赖只向下

```
StereoVisionApp.swift            @main（只接 RootView）
StereoVision/UI/*.swift          SwiftUI 界面（只 import SwiftUI）
StereoVision/Contracts/*.swift   只读快照 + intent + 共享数据模型（无 UI 框架依赖）
StereoVision/App/*.swift         接线 AppModel + 每帧调度 FramePipeline
StereoVision/Camera/*.swift      AVFoundation 会话 + 标定读取
StereoVision/Analysis/*.swift    深度连通域 → Swift 值类型
StereoVision/Rendering/*.swift   MetalKit 渲染 / MTKView 包装 / 导出
StereoVision/Engine/*.swift      双目流程编排 + 可替换的阶段后端（Metal / C++）
        │  Swift 只看 ObjC 头（桥接层是唯一入口）
StereoVision/Bridge/*.mm      ObjC++ 门面：指针、生命周期、simd ↔ float[9] 换乘
        │  纯 C 头（POD + 不透明句柄）
core/include + core/src       C++ 算法（零 Apple 依赖、零第三方依赖）
core/tests                    C++ 侧全部测试（Windows/Linux/macOS 都跑）
Shaders.metal                 GPU kernel（iOS 的快路径）
```

五条硬规矩：

1. `core/` 不许 include 任何 Apple 头；`Swift` 不许 include 任何 C++ 头。
2. `core/include/stereo/*.hpp` 只给 C++ 实现与 C++ 测试看；Bridge 只 include `stereo_c_api.h`（它是唯一的 C 契约）。
3. 界面层只拿**值类型**（`CaptureUIState` / `DiagnosticsSnapshot` / `DeviceInfo`）和**协议**（`CaptureIntents` / `SettingsIntents`）——"诊断页不能偷改状态"是编译器保证的。
4. **工程文件不提交**：`StereoVision.xcodeproj` 由 CI 用 `xcodegen generate --spec project.yml` 生成，`scripts/check_xcodeproj_sources.sh` 保证不漏文件。增删源文件只动文件本身。
5. **目录红线**（`stereovision_agent.md` §0 的 grep 验收）：`UI/` 不 import AVFoundation/MetalKit、不出现 `StereoEngine`/`SVStereoCore`；`Contracts/` 不 import SwiftUI/AVFoundation；`Engine/` 不反向依赖 UI/App/Rendering/Analysis。唯一例外：`Engine/Input` 与 `Engine/Depth` 消费 `Camera/` 的**标定数据**（`Intrinsics`/`CameraRig`/`CameraCalibrationReader`），不涉及会话与设备。

## 1. `core/` — C++ 算法

### 1.1 对外契约：`include/stereo/stereo_c_api.h`（总头）+ 9 个领域子头（版本 150）

**24 个导出符号**（`stereo_*`）+ POD 结构体。按领域拆在 9 个**自包含**子头里，
总头只 `#include` 它们（旧的 include 路径不变）。每个子头有自己的 `extern "C"` 与
include guard；CMake 为每个头生成一个只 include 它的 `.c`（`stereo_header_selfcheck`），
漏一个依赖就编不过。符号面另由 `tools/check_api_surface.py` + `core/tests/api_surface.txt` 锁定。

| 子头 | 内容 |
|---|---|
| `stereo_types.h` | 版本宏、`stereo_format`、`stereo_image`、`stereo_intrinsics` |
| `stereo_context.h` | 不透明句柄、`stereo_config`、生命周期、`stereo_last_error` |
| `stereo_match.h` | `stereo_match`（Census + SGM 4 路径）、`stereo_disparity_to_depth` |
| `stereo_rectify.h` | `stereo_rectify_geometry` / `_image` / `stereo_unrectify_depth` |
| `stereo_analyze.h` | `stereo_analyze_grid` / `stereo_sample_depth` / `stereo_analyze` |
| `stereo_export.h` | `stereo_point_cloud(_capacity)` / `_depth_filter` / `_depth_to_u16_mm` / `_copy_depth` |
| `stereo_calibrate.h` | `stereo_calibrate`（CPU 打分）/ `stereo_calibrate_search`（平台回调） |
| `stereo_params.h` | `stereo_param_count` / `_table` / `_find`（参数表唯一真源，见 §1.3） |
| `stereo_pipeline.h` | `stereo_process_frame`（第 2..5 步；iOS 不走它，见 FAQ Q18） |

### 1.2 实现

| 文件 | 行 | 职责 | 主要入口 |
|---|---|---|---|
| `src/sgm.cpp` | 289 | 第 3/4 步原语（逐 kernel 对齐 MSL）+ `match_luma` 组合 | `census` `cost_volume` `sgm_all_paths` `wta_right` `wta_left` `to_depth` `match_luma` |
| `src/geometry.cpp` | 213 | 3×3 小工具 + 极线矫正几何 | `rectify_geometry` |
| `src/rectify.cpp` | 142 | 重采样（4 次超采样 + 双线性）/ 深度回转（最近邻） | `rectify_image` `unrectify_depth` |
| `src/color.cpp` | 110 | 像素格式 → 亮度 / 紧凑 BGRA8（从边界层搬来） | `to_luma` `to_bgra8` |
| `src/analyze.cpp` | 207 | 降采样成网格 → 4 邻域连通域 → 面块统计 | `analyze_depth` |
| `src/sample.cpp` | 58 | 自适应抽样 + 第 q 分位数 | `sample_quantile` |
| `src/cloud.cpp` | 187 | PLY（`std::to_chars`）、深度裁剪/打包/拷贝 | `point_cloud_ply` 等 |
| `src/calibrate.cpp` | 222 | 自动标定搜索；CPU 打分版也在里面 | `calibrate_search` `calibrate_rectification` |
| `src/params.cpp` | 84 | **参数描述表**（11 条）+ `config_default`（C/C++ 共用） | `param_table` `param_default` `config_default` |
| `src/workspace.cpp` | 174 | 跨帧缓冲（match / analyze / calibrate / frame），尺寸不变不重分配 | `MatchBuffers` `AnalyzeBuffers` `CalibrateBuffers` `FrameBuffers` |
| `src/pipeline.cpp` | 217 | 第 2..5 步组合（C 与 C++ 前端共用） | `stereo::process_frame` |
| `src/capi/*.cpp` | 496（8 个文件，最大 102） | 边界层：校验 + POD 换乘 + 错误码，**无像素循环**；`capi_internal.hpp` 是 `struct stereo_context` 的唯一定义 | 24 个导出符号 |

### 1.3 参数表（`params.cpp`）

11 条：`min_z` `max_z` / `matcher` `max_disparity` `p1` `p2` `lr_check` `uniqueness` `y_offset` `flip_baseline` / `overlay_alpha`。

- **默认值只有这一份**：`stereo_config_default`（C）、`PipelineSettings`（Swift）、`SettingsDraft`（Swift）全部从表取。
- **滑杆范围也只有这一份**：`SettingsScreen` 按表生成控件。
- 表里的 min/max 是**给界面的建议范围**，不一定等于库的硬限制（库接受 `max_disp` 4..96）。

### 1.4 测试（不进 App，CI 的 7 个套件）

| 文件 | 行 | 覆盖 |
|---|---|---|
| `tests/golden_main.cpp` | 271 | 与 numpy 孪生实现**逐位对拍**（SGM 全链路） |
| `tests/geometry_test.cpp` | 487 | 矫正几何的数学不变量（极线同行、视差 = fx·b/Z、Q 还原 3D） |
| `tests/analyze_test.cpp` | 444 | 连通域/分位数，对拍一套独立写的朴素参考 |
| `tests/frame_test.cpp` | 346 | `process_frame` 与手工 6 步**逐位一致**；参数表自洽性 |
| `tests/calibrate_test.cpp` | 313 | 注入已知 dy 再让它找回（含平台打分回调路径） |
| `tests/cloud_test.cpp` | 257 | PLY 头部与顶点行逐字符、裁剪/打包/拷贝边界 |
| `tests/c_api_smoke.c` | 233 | 纯 C 编译器能编、能用、NULL/非法参数会拒 |

## 2. `StereoVision/Bridge/` — ObjC++ 门面（1170 行）

| 类 | 管什么 |
|---|---|
| `SVStereoCore` | 持有一个 `stereo_context`。方法：`configure`、`matchLeft`、`depth(fromDisparity:)`、`+unrectifyDepth`、`sampleDepth`、`analyzeDepth:into:`、`+calibrate:flip0:dy0:`（收 block 当打分回调）、`processFrame:...`（**Swift 目前不调**） |
| `SVRectifyInput` / `SVRectification` | 第 2 步的输入输出（`+solve:`），`float[9]` ↔ `simd_float3x3` |
| `SVAnalysis` / `SVBlobRecord` | 面块与标签图的**借用指针**容器（避免每帧分配） |
| `SVCalibration` | 自动标定结果（flipped / yOffset / score） |
| `SVExporter` | 点云 PLY（返回 `NSData`）、毫米打包、深度裁剪、跨度拷贝、非负均值 |
| `SVParameter` / `SVParameterTable` | 参数表桥接；Swift 设置界面按它生成滑杆 |
| `SVFrameRequest` / `SVFrameTimings` | `process_frame` 的输入/耗时（只服务它） |

这一层只做三件事：指针与生命周期、simd ↔ `float[9]`、POD ↔ ObjC 对象。**不放算法**。

## 3. `StereoVision/` — Swift 前端（3865 行）

按"谁可以依赖谁"分目录（红线见 `stereovision_agent.md` §0）：

| 目录 | 内容 | 依赖约束 |
|---|---|---|
| 根 | `StereoVisionApp.swift`（`@main`）、`Shaders.metal`、`Bridge/`、`Assets.xcassets` | — |
| `Contracts/` | 界面快照/意图 + 共享词汇 + 管线数据结构 | 只 import CoreGraphics / Foundation（**不许** SwiftUI / AVFoundation） |
| `UI/` | 5 个 SwiftUI 视图 | 只 import SwiftUI + Contracts（**不许** AVFoundation / MetalKit，也不许出现 `StereoEngine` / `SVStereoCore`） |
| `App/` | `AppModel`（接线）+ `FramePipeline`（每帧调度） | 全部 |
| `Camera/` | AVFoundation 会话 + 标定读取 | AVFoundation / CoreVideo / simd |
| `Analysis/` | 深度连通域 → Swift 值类型 | CoreGraphics + Bridge |
| `Rendering/` | MetalKit 渲染、MTKView 包装、导出 | MetalKit / AVFoundation / SwiftUI |
| `Engine/` | 双目流程 + 阶段后端（§3.2.5） | 不得反向依赖 UI/App/Rendering/Analysis；`Input/` 只消费 Camera 的**标定数据** |

### 3.1 App 入口与界面（`UI/`）

| 文件 | 行 | 职责 | 入参 |
|---|---|---|---|
| `StereoVisionApp.swift` | 21 | `@main`，只做 `WindowGroup { RootView() }` | — |
| `UI/RootView.swift` | 47 | 连线根：持有 `AppModel`，管设置/诊断两个 sheet | — |
| `UI/CaptureScreen.swift` | 194 | 主拍摄界面：预览 + 手势 + `HUD` + `SidePanel` | `CaptureUIState` + `CaptureIntents` + `MetalSceneRenderer` |
| `UI/SettingsScreen.swift` | 172 | 设置：按参数表生成控件，改 `@State draft`，Apply/Cancel | `SettingsDraft` + `DeviceInfo` + `SettingsIntents` |
| `UI/DiagnosticsScreen.swift` | 142 | 诊断：整屏 sheet，纯只读；含 `StageDiag` 与 `PipelineKind.rows(_:)` 文案 | `DiagnosticsSnapshot` + `onDismiss` |
| `UI/OverlayView.swift` | 72 | 主界面 Canvas 叠层（准星读数、面块框、点选气泡） | `CaptureUIState` |

### 3.2 运行时

#### 3.2.1 `Contracts/` — 共同语言

| 文件 | 行 | 内容 |
|---|---|---|
| `Contracts/UIContracts.swift` | 255 | `DisplayOrientation`、`SettingsDraft`（含 `subscript(key:)`）、`DeviceInfo`、`CaptureUIState`、`CaptureIntents`/`SettingsIntents`、`GpuReport`、`DiagnosticsSnapshot` |
| `Contracts/PipelineContracts.swift` | 161 | 共享词汇 `ViewMode`/`Matcher` + `PipelineSettings`、`PipelineKind`、`StageTimings`、`FrameReport`、`FrameStore` |

#### 3.2.2 `App/`

| 文件 | 行 | 职责 | 关键成员 |
|---|---|---|---|
| `App/AppModel.swift` | 371 | 状态聚合 + 接线 + 意图实现 | `start`/`setSource`/`calibrate`/`measure`/`captureTarget`/`metadata`/`outputSize` + 只读视图 `ui`/`diagnostics`/`draft`/`device` |
| `App/FramePipeline.swift` | 81 | 每帧在相机队列上的调度（数据结构在 Contracts） | `stereo`/`depthData`/`timings(of:)`/`finish` |

#### 3.2.3 `Camera/`

| 文件 | 行 | 职责 | 关键成员 |
|---|---|---|---|
| `Camera/CameraSessionController.swift` | 226 | AVFoundation 会话：双摄/系统深度、format 选择、同步输出 | `start`/`stop`、`makeStereo`/`makeDepth`、`dataOutputSynchronizer`、`DepthSource` |
| `Camera/CameraCalibrationReader.swift` | 157 | **第 1 步**：只向系统要参数，不做几何 | `intrinsics(of:fov:size:)`、`.calibration(of:)`、`.extrinsics(from:to:)`、`.rig(wide:ultra:)`、`.depthIntrinsics`；数据形状 `Intrinsics`/`CameraRig` |

#### 3.2.4 `Analysis/` 与 `Rendering/`

| 文件 | 行 | 职责 | 关键成员 |
|---|---|---|---|
| `Analysis/DepthSceneAnalyzer.swift` | 90 | C++ 分析结果的薄适配层（C 结构 → `CGRect`/数组） | `Blob`/`Analysis`/`DepthSceneAnalyzer.analyze(_:using:)` |
| `Rendering/MetalSceneRenderer.swift` | 211 | MetalKit 渲染：显示 / 点云 / 离屏烧录 | `draw`、`encode`、`uploadLabels`、`drawPoints`、`offscreenPass` + 矩阵工具 |
| `Rendering/MetalView.swift` | 26 | `MTKView` 的 SwiftUI 包装（`UIViewRepresentable`）—— UI/ 里唯一不碰 MetalKit 的原因 | `MetalView` |
| `Rendering/MediaExportManager.swift` | 266 | 拍照/录像/导出（不持有 `AppModel`） | `takePhoto`、`toggleRecording`、`beginPhoto`、`beginRecordingFrame`、`export`、`ply` |

#### 3.2.5 引擎阶段（`Engine/`）

流程固定、阶段可替换。每个后端只做自己那一段，不复制整条流程；`StereoEngine` 是唯一的编排者。

| 文件 | 行 | 职责 | 关键类型 |
|---|---|---|---|
| `StereoEngine.swift` | 237 | 编排：输入 → 几何 → 矫正 → 匹配 → 深度 → 组帧 | `processStereoFrame(_:_:_:mode:)` |
| `StereoTypes.swift` | 111 | 阶段间值类型 + 工作分辨率 | `StereoWork`、`StereoGeometry`、`StereoFrame` |
| `StereoConfiguration.swift` | 34 | 后端组合（配置，不是流程分支） | `StereoMode`（`.metal` / `.cppHybrid`）、`PipelineSettings.stereoMode` |
| `Input/FrameInputProvider.swift` | 73 | 相机输入适配：样本 → 纹理 + 每帧内参 + `CameraRig` | `FrameInputs`、`FrameInputProvider` |
| `Geometry/StereoGeometrySolver.swift` | 52 | 第 2 步几何，交给 `SVRectification.solve`（core/src/geometry.cpp） | `solve(rig:wide:ultra:...)` |
| `Rectification/RectificationBackend.swift` | 26 | 矫正阶段窄接口（编码进调用方的 command buffer） | `RectificationBackend` |
| `Rectification/MetalRectifier.swift` | 31 | `rectify` kernel | `MetalRectifier` |
| `Matching/MatchingBackend.swift` | 56 | 匹配阶段窄接口 + 纹理视图 | `MatchingBackend`（`encodeCensus`/`encodeMatch`）、`MetalMatchInput` |
| `Matching/MetalSGMMatcher.swift` | 77 | SGM 4 路径（costVolume → sgmPath×4 → wta） | `MetalSGMMatcher`（持有 cost/sum 缓冲、dispRight） |
| `Matching/MetalBlockMatcher.swift` | 39 | 5×5 块匹配（也被标定打分复用） | `MetalBlockMatcher` |
| `Matching/CppSGMMatcher.swift` | 40 | C++ 匹配（core/src/sgm.cpp），同步调用 | `configure`/`match` |
| `Readback/CPUImageReadback.swift` | 77 | 显式像素读回适配：锁 buffer + `getBytes` + 视差缓冲 | `CPUImageReadback` |
| `Depth/DepthBackend.swift` | 27 | 深度阶段窄接口（只有 Metal 版有 encoder 形状） | `MetalDepthBackend` |
| `Depth/MetalDepthProcessor.swift` | 39 | `toDepth` → `unrectifyDepth` | `MetalDepthProcessor` |
| `Depth/CppDepthProcessor.swift` | 48 | C++ `depth(fromDisparity:)` + `unrectifyDepth`（直接写共享 buffer） | `CppDepthProcessor` |
| `Depth/SystemDepthProcessor.swift` | 55 | 系统深度（AVDepthData）→ `filterDepth`，**不是** SGM 后端 | `SystemDepthProcessor` |
| `Calibration/StereoCalibrator.swift` | 83 | 自动标定：C++ 候选搜索 + GPU 打分（slots[0]） | `StereoCalibrator` |
| `Metal/MetalContext.swift` | 40 | device / command queue / 纹理缓存 | `MetalContext` |
| `Metal/MetalPipelines.swift` | 43 | 9 条 compute PSO 的创建与持有 | `MetalPipelines` |
| `Metal/MetalFrameResources.swift` | 110 | Slot ×3、`DepthTarget`、左图 PixelBufferPool | `MetalSlot`、`MetalFrameResources` |
| `Metal/MetalDispatch.swift` | 36 | dispatch 样板（线程数口径与改造前一致） | `MetalDispatch` |
| `Metal/MetalShaderTypes.swift` | 39 | kernel 参数结构（ABI 与 Shaders.metal 对齐） | `RectParams`/`MatchParams`/`DepthParams`/`SGMParams` |

阶段与后端的对应关系、以及每帧/每资源跑在哪个线程、谁写谁读，见 `docs/engine-backend-baseline.md`。

### 3.3 GPU

| 文件 | 行 | 内容 |
|---|---|---|
| `Shaders.metal` | 399 | 计算：`rectify` `unrectifyDepth` `census` `match` `costVolume` `sgmPath` `wtaRight` `wtaLeft` `toDepth`；显示/点云：`fullscreenVertex` `displayFragment` `pointVertex` `pointFragment` |

## 4. 三条操作的调用链

### 4.1 打开软件 → 相机启动 → 标准模式

**启动一次（同步）**

```
StereoVisionApp                   StereoVisionApp.swift
└─ RootView                       UI/RootView.swift
   └─ AppModel()                  App/AppModel.swift
      ├─ StereoEngine(device:)    Engine/StereoEngine.swift → MetalContext + MetalPipelines + MetalFrameResources
      │                                                  + FrameInputProvider + 各阶段 backend + SVStereoCore
      ├─ FramePipeline(engine:)        App/FramePipeline.swift       → FrameStore、DepthSceneAnalyzer、PipelineSettings
      ├─ MetalSceneRenderer(device:store:)  Rendering/MetalSceneRenderer.swift       → 显示/点云 PSO、labelTex、纹理缓存
      ├─ MediaExportManager()                Rendering/MediaExportManager.swift
      ├─ CameraSessionController()       Camera/CameraSessionController.swift
      ├─ 接线 onStereo / onDepth / onDevices / onError / publish
      └─ watchOrientation()       App/AppModel.swift       → push{orient}
```

**开流**

```
RootView.body                     .task { await model.startCapture() }
└─ AppModel.startCapture()        App/AppModel.swift      （CaptureIntents）
   └─ AppModel.start()            → AVCaptureDevice 授权 → refreshOrientation → setSource(.stereo)
      └─ CameraSessionController.start()  Camera/CameraSessionController.swift
         └─ makeStereo()          → MultiCam session + 两个 VideoDataOutput + Synchronizer
            └─ onDevices → StereoEngine.configure()   Engine/StereoEngine.swift
               └─ FrameInputProvider.configure        Engine/Input/FrameInputProvider.swift
               └─ CameraCalibrationReader.rig()             Camera/CameraCalibrationReader.swift   ← 第 1 步：读 K / R / t
```

**每帧（相机数据队列）**

```
CameraSessionController.dataOutputSynchronizer(...)     Camera/CameraSessionController.swift
└─ onStereo → FramePipeline.stereo                    App/FramePipeline.swift
   ├─ [可选] StereoEngine.calibrate              Engine/StereoEngine.swift
   │         └─ StereoCalibrator.calibrate       Engine/Calibration/StereoCalibrator.swift
   │            （C++ 候选搜索 + GPU 打分：rectify → census → block match，用 slots[0]）
   ├─ StereoEngine.processStereoFrame            Engine/StereoEngine.swift   ← 唯一编排入口（mode = .metal）
   │  ├─ FrameInputProvider.inputs()             → 纹理 + 内参 + CameraRig
   │  ├─ StereoGeometrySolver.solve()            → SVRectification.solve → geometry.cpp（每帧一次）
   │  ├─ MetalRectifier.encode ×2                → rectify kernel
   │  ├─ Metal(SGM|Block)Matcher.encodeCensus/Match
   │  ├─ MetalDepthProcessor.encode              → toDepth → unrectifyDepth
   │  └─ 以上全部在**一条** command buffer 里
   └─ FramePipeline.finish                            App/FramePipeline.swift
      ├─ store.latest = frame
      ├─ DepthSceneAnalyzer.analyze(_:using:)              Analysis/DepthSceneAnalyzer.swift
      │  └─ SVStereoCore.analyzeDepth:into:      → stereo_analyze → analyze.cpp + sample.cpp
      ├─ FramePipeline.timings(of:) → FrameReport(kind / timings)
      └─ publish → AppModel.apply(_ r:)          App/AppModel.swift
```

**显示（MTKView 每帧）**

```
MetalSceneRenderer.draw(in:)                Rendering/MetalSceneRenderer.swift
├─ uploadLabels(model.analysis)   → labelTex.replace
├─ encode(...)                    → displayFragment（标准模式 = mode 0，只贴相机图）
└─ AppModel.captureTarget()       App/AppModel.swift   （没拍照没录像时立刻 nil）

OverlayView(state:)               UI/OverlayView.swift（准星读数/面块框/点选气泡）
HUD(state:)                       UI/CaptureScreen.swift
```

标准模式只是 `ViewMode.standard`（默认值）→ `shaderIndex == 0`，无额外分支。

### 4.2 打开诊断界面

```
SidePanel 按钮                    UI/CaptureScreen.swift   showDiagnostics = true
└─ RootView 的 .sheet             UI/RootView.swift
   ├─ 取数：AppModel.diagnostics   App/AppModel.swift        （只读快照，值类型）
   │  ← gpuReport(kind/timings) ← AppModel.apply(_ r: FrameReport) ← FramePipeline.timings(of:)
   └─ DiagnosticsScreen(snapshot:onDismiss:)   UI/DiagnosticsScreen.swift
      ├─ 概览 / GPU 内存：直接读 snapshot
      ├─ 各阶段表：ForEach(PipelineKind.rows(timings))   文案在诊断页里
      └─ Done → onDismiss → showDiagnostics = false
```

诊断页**不调用任何引擎函数**，只消费每帧算好的报告。

### 4.3 打开设置界面 → 改任意参数

```
SidePanel 按钮                    UI/CaptureScreen.swift   showSettings = true
└─ RootView 的 .sheet             UI/RootView.swift
   ├─ 进参：AppModel.draft / .device / .overlayAlpha
   └─ SettingsScreen(draft:device:overlayAlpha:intents:onDismiss:)   UI/SettingsScreen.swift

界面构建
└─ SVParameterTable.allParameters()   Bridge → stereo_param_count / stereo_param_table → params.cpp
   ├─ group → Section（depth / matching / display）
   ├─ kind  → Slider / Toggle / segmented Picker
   └─ min/max/step/unit → 滑杆范围与格式化（Swift 里没有硬编码区间）

拖动滑杆 / 切开关
└─ 只改 SettingsScreen 自己的 @State draft（走 SettingsDraft.subscript(key:)）← **不进管线**
   例外：overlay_alpha → SettingsIntents.setOverlayAlpha → AppModel.overlayAlpha（即时生效）

点 Apply
└─ SettingsIntents.apply(draft) → AppModel.apply(_ d: SettingsDraft)   App/AppModel.swift
   ├─ 写 10 个 private(set) 属性
   └─ **一次** push{...} 写进 PipelineSettings（不是 11 个 didSet 各推一次）

下一帧生效位置
├─ maxDisparity / p1 / p2 / uniqueness / lrCheck → MetalSGMMatcher.encodeMatch
│  （CPU 混合模式还会 CppSGMMatcher.configure 重新分配 C++ 缓冲）
├─ matcher   → StereoEngine.processStereoFrame 里的 matchers[matcher]（.sgm / .block 两个实现）
├─ minZ/maxZ → MetalDepthProcessor（DepthParams → toDepth）；以及 displayFragment 的着色映射
└─ flip/yOffset → StereoGeometrySolver.solve → SVRectification.solve
```

点 **Cancel** → 丢弃 `draft`（`overlay_alpha` 已生效，这是唯一的不一致）。

## 5. 一帧的数据流（一句话）

```
BGRA 双帧 ─GPU rectify→ 640×360 左右图 ─Metal SGM→ 视差 ─toDepth→ 深度(rolled) ─unrectify→ 宽相机帧深度
                                                              │
                        C++ analyze（网格+连通域+分位数）──────┴→ Analysis/Blobs/labels/中心读数
                                                              │
                                             MetalSceneRenderer（着色/点云）+ OverlayView + HUD
```

## 6. 评审时值得怀疑的地方（供删减参考）

| # | 对象 | 事实 | 值得问的问题 |
|---|---|---|---|
| 1 | `stereo_process_frame` + `SVFrameRequest`/`SVFrameTimings` + `-processFrame:` | **Swift 侧零调用**，只有 `frame_test` 与非 Apple 前端会用 | iOS 上要不要留？若将来做"全 CPU 后端"或 Qt 前端，它是入口；只服务 iOS 就是纯死代码（约 350 行 C++/桥接） |
| 2 | `stereo_calibrate`（CPU 全流水线版） | 只有测试用；iOS 走 `stereo_calibrate_search` + GPU 打分（CPU 版 640×360 实测 3.4 s） | 只保留 `_search` 可删 ~120 行，但会失去"无 GPU 平台也能标定" |
| 3 | 块匹配后端 `Matcher.block` | `Shaders.metal` 的 `match` kernel + `Engine/Matching/MetalBlockMatcher.swift`；**同时被标定的 GPU 打分复用** | 后端还用得上吗？砍它要先把标定打分换成别的代价函数 |
| 4 | 两条系统深度分支 | `DepthSource.apple`/`.lidar` + `Engine/Depth/SystemDepthProcessor.swift`（~55 行）+ `CameraSessionController.makeDepth`（~45 行） | 如果只用自研双目，整条可删 |
| 5 | `CameraCalibrationReader.calibration(of:)` / `.extrinsics(from:to:)` | 只在 `rig()` 里各用一次；每帧真正用的是 `intrinsics(of:fov:size:)` | 三者能否合一 |
| 6 | 双份"视差后"实现 | `stereo_process_frame` 内部调 `to_depth`/`unrectify`，iOS 的 `CppDepthProcessor` 调 `depthFromDisparity`+`unrectifyDepth`（同一对函数、两次调用） | 可合并成 `stereo_process_disparity`（省一次 malloc + 一次跨桥）；FAQ Q18 有分析 |
| 7 | `core/tests/` 2351 行 | 不是运行时代码，但它是"改 C++ 不出错"的唯一保障（**iOS 侧没有任何测试**） | 删测试 = 放弃回归保护 |
| 8 | `MetalSceneRenderer` 的矩阵工具 | `perspective`/`translate`/`rotateX/Y/Z`（~25 行）只服务点云视图 | 值得留 |
| 9 | `tools/` 下的脚本 | `sgm_ref.py`+`gen_golden.py` 是金标准的来源（有用）；`viewer.html` / `make_demo_export.py` 用途不明 | 后两个确认没人用可删 |
| 10 | 文档 | `test.md` 981 行（过程记录）、`FAQ.md`、`ARCHITECTURE.md`、`docs/engine-backend-{baseline,refactor}.md`、`stereovision_agent.md`（目录整理方案）；已删除无引用的 24 MB 演示素材（`demo.gif` / `demo.mp4` / `get_gif.py` / `self-logo.png`） | 过程文档按需留；两份 engine 文档 + 目录方案是这次重构的唯一说明 |

**已核实"无引用"并已删除的东西**：`docs/demo.gif`、`docs/demo.mp4`、`docs/get_gif.py`、`docs/self-logo.png`（旧 README 的演示素材，删前确认 md / 代码里零引用，共 24 MB）；本地构建残留（`build/gifwork`、`build/artifacts`、`build/core-ci` 等，已忽略不入库）。代码侧之前清掉的 `DisplayOrientation.inverse`、`CameraCalibration.role/referenceSize`、`CameraRole`、`runCoreSelfTest`、`StereoCore` target 都已不在。

**检查用的命令**（评审时可以自己跑）：

```bash
# Swift 里是否还有逐像素循环（期望 0）
grep -rn "for .* in 0\.\.<" --include='*.swift' StereoVision/

# 目录红线（三条都期望空）
grep -rnE "import (AVFoundation|MetalKit)|StereoEngine|SVStereoCore" StereoVision/UI/
grep -rnE "import (SwiftUI|AVFoundation)" StereoVision/Contracts/
grep -rnE "\b(CameraController|CalibrationSource|Analyzer|Renderer)\b" StereoVision/

# 某个 Swift 符号还有没有引用
grep -rn "符号名" StereoVision/

# C++ 侧全部测试（7 个套件）
cmake -S core -B build/core -DCMAKE_BUILD_TYPE=Release && cmake --build build/core --config Release
cd build/core && ctest -C Release
```

> 注意：iOS 侧**没有单元测试**，删改 Swift 之后只能靠 CI 编译 + 真机验证。
> 工程文件由 `project.yml` 在 CI 里生成（`xcodegen generate`），`.gitignore` 已忽略
> `StereoVision.xcodeproj/`；`scripts/check_xcodeproj_sources.sh` 会在构建前确认
> "仓库里的源文件"与"工程里参与编译的文件"一致，漏一个就当场失败。

## 7. 构建、产物与文档

| 目标 | 在哪跑 | 怎么跑 |
|---|---|---|
| C++ 对拍 + 几何/分析/标定测试（7 个套件） | 本机即可（Windows/Linux/macOS，零第三方依赖） | `cmake -S core -B build/core -DCMAKE_BUILD_TYPE=Release && cmake --build build/core --config Release && cd build/core && ctest -C Release` |
| 未签名 IPA | GitHub Actions 的 macOS runner（`.github/workflows/ipa.yml`；push `main`/`test` 或手动 dispatch） | CI：`xcodegen generate` → `scripts/check_xcodeproj_sources.sh` → `xcodebuild`（`CODE_SIGNING_ALLOWED=NO`）→ 打成 `Payload/` zip |
| 取回 IPA | 本地（需要 GitHub token，脚本会先问 `git credential`） | `./scripts/fetch_ipa.sh` → `build/StereoVision-unsigned.ipa` |
| 工程文件 | CI / macOS 本地 | `xcodegen generate --spec project.yml`（`.xcodeproj` 不入库） |

Windows 上没有 Swift/Xcode 工具链：本地只能跑 C++ 测试和语法级检查；Swift 编译与真机回归
必须依赖 macOS runner / 真机（回归清单见 `docs/engine-backend-refactor.md` §6）。

| 文档 | 用途 |
|---|---|
| `ARCHITECTURE.md` | 本页：分层、文件职责、调用链、评审候选 |
| `FAQ.md` | 为什么这样分层、算法分工、常见疑问 |
| `docs/engine-backend-baseline.md` | 后端拆分**前**的调用关系、线程、资源生命周期、计时口径（对照基线） |
| `docs/engine-backend-refactor.md` | 拆分**后**的迁移映射、等价性说明、真机回归清单 |
| `stereovision_agent.md` | `StereoVision/` 目录与命名规范（本次目录整理的验收依据） |
| `test.md` | 历史过程记录（按需留） |
| `tools/README.md` | Python 孪生实现、金标准生成、viewer |

App 运行时文件见 §3（`Engine/` 细分见 §3.2.5），C++ 见 §1，桥接见 §2。
