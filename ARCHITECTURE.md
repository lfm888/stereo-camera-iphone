# 架构地图

> 用途：改代码 / 删文件之前先看这一页。行数是 `wc -l` 实测值（2026-11）。
> 全部代码量 ≈ 8.7k 行：C++ 算法 2.8k、C++ 测试 1.9k、Swift 2.7k、ObjC++ 桥 0.8k、Metal 0.4k。

## 0. 五层，依赖只向下

```
StereoVision/*.swift          SwiftUI 界面 + 运行时（相机/引擎/渲染/导出）
        │  只读快照 + intent 协议
StereoVision/Bridge/*.mm      ObjC++ 门面：指针、生命周期、simd ↔ float[9] 换乘
        │  纯 C 头（POD + 不透明句柄）
core/include + core/src       C++ 算法（零 Apple 依赖、零第三方依赖）
core/tests                    C++ 侧全部测试（Windows/Linux/macOS 都跑）
Shaders.metal                 GPU kernel（iOS 上的快路径）
```

三条硬规矩：

1. `core/` 不许 include 任何 Apple 头；`Swift` 不许 include 任何 C++ 头。
2. `core/include/stereo/*.hpp` 只给 C++ 与桥接层看；`stereo_c_api.h` 是唯一的 C 契约。
3. 界面层只拿**值类型**（`CaptureUIState` / `DiagnosticsSnapshot` / `DeviceInfo`）和**协议**（`CaptureIntents` / `SettingsIntents`）——所以"诊断页不会偷改状态"是编译器保证的。

## 1. `core/` — C++ 算法

### 1.1 对外契约

| 文件 | 行 | 是什么 |
|---|---|---|
| `include/stereo/stereo_c_api.h` | 354 | **唯一的 C 导出表**，版本 140。18 个函数 + POD 结构体，Windows/Linux/iOS 共用 |

| 函数 | 干什么 |
|---|---|
| `stereo_create` / `destroy` / `config_default` / `configure` / `last_error` | context 生命周期与参数；内部缓冲按尺寸复用 |
| `stereo_match` | 第 3 步：Census 7×7 + SGM 4 路径 + 唯一性 + LR 检查 + 亚像素 |
| `stereo_disparity_to_depth` | 第 4 步：视差 → 深度（3×3 中值 + 范围截断） |
| `stereo_rectify_geometry` | 第 2 步的几何：内参+外参 → MW/MU/MUp/K/Pl/Pr/Q |
| `stereo_rectify_image` / `stereo_unrectify_depth` | 第 2 步重采样 / 深度回转（Metal kernel 的 CPU 孪生） |
| `stereo_sample_depth` / `stereo_analyze` / `stereo_analyze_grid` | 深度查询与连通域分析（准星读数、点选测距、3D segmentation 面块） |
| `stereo_point_cloud` / `stereo_point_cloud_capacity` / `stereo_depth_filter` / `stereo_depth_to_u16_mm` / `stereo_copy_depth` | 导出：PLY 点云、深度裁剪、16 位毫米打包、跨度拷贝 |
| `stereo_calibrate` / `stereo_calibrate_search` | 自动标定：前者 CPU 全流水线，后者由平台提供打分回调（iOS 用 GPU） |

### 1.2 实现

| 文件 | 行 | 职责 | 主要入口 |
|---|---|---|---|
| `src/sgm.cpp` | 268 | 第 3/4 步本体（逐 kernel 对齐 MSL） | `rgb_to_luma` `census` `cost_volume` `sgm_all_paths` `wta_right` `wta_left` `to_depth` |
| `src/geometry.cpp` | 213 | 3×3 小工具 + 极线矫正几何 | `rectify_geometry` |
| `src/rectify.cpp` | 142 | 单应重采样（4 次超采样 + 双线性）、深度回转（最近邻） | `rectify_image` `unrectify_depth` |
| `src/sample.cpp` | 58 | 归一化矩形内自适应抽样 + 第 q 分位数 | `sample_quantile` |
| `src/analyze.cpp` | 207 | 降采样成网格 → 4 邻域深度连通域 → 面块统计 | `analyze_depth` |
| `src/cloud.cpp` | 187 | PLY（`std::to_chars`）、深度裁剪/打包/拷贝 | `point_cloud_ply` `depth_filter` `depth_to_u16_mm` `copy_depth` |
| `src/calibrate.cpp` | 222 | 自动标定搜索（2×25 + 4 细化）；CPU 打分版也在里面 | `calibrate_search` `calibrate_rectification` |
| `src/stereo_c_api.cpp` | 611 | 参数校验 + 缓冲管理 + POD 翻译（**不含算法**） | 上面那 18 个符号 |

### 1.3 测试（不进 App，CI 的 6 个套件）

| 文件 | 行 | 覆盖 |
|---|---|---|
| `tests/golden_main.cpp` | 271 | 与 numpy 孪生实现**逐位对拍**（SGM 全链路） |
| `tests/geometry_test.cpp` | 487 | 矫正几何的数学不变量（极线同行、近视差 fx·b/Z、Q 还原 3D、往返） |
| `tests/analyze_test.cpp` | 444 | 连通域/分位数，对拍一套独立写的朴素参考 |
| `tests/cloud_test.cpp` | 257 | PLY 头部与顶点行逐字符、裁剪/打包/拷贝边界 |
| `tests/calibrate_test.cpp` | 313 | 注入已知 dy 再让它找回（含 C 回调路径、耗时） |
| `tests/c_api_smoke.c` | 162 | 纯 C 编译器能编、能用、NULL/非法参数会拒 |

## 2. `StereoVision/Bridge/` — ObjC++ 门面（827 行）

| 类 | 行 | 管什么 |
|---|---|---|
| `SVStereoCore` | h 43-160 / mm 62-300 | 持有一个 `stereo_context`。方法：`configure`、`matchLeft`、`depth(fromDisparity:)`、`+unrectifyDepth`、`sampleDepth`、`analyzeDepth:into:`、`+calibrate:flip0:dy0:`（收 block 当打分回调） |
| `SVRectifyInput` / `SVRectification` | h 263-307 / mm 380+ | 第 2 步的输入输出（`+solve:`），把 `float[9]` 换成 `simd_float3x3` |
| `SVAnalysis` / `SVBlobRecord` | h 172-200 / mm 300+ | 面块与标签图的**借用指针**容器（避免每帧分配） |
| `SVCalibration` | h 162-170 | 自动标定结果（flipped / yOffset / score） |
| `SVExporter` | h 207-255 / mm 440+ | 点云 PLY（返回 `NSData`）、毫米打包、深度裁剪、跨度拷贝、非负均值 |

这一层只做三件事：指针与生命周期、simd ↔ `float[9]`、POD ↔ ObjC 对象。**不放算法**。

## 3. `StereoVision/` — Swift 前端

### 3.1 界面（4 个文件，只读快照 + intent）

| 文件 | 行 | 职责 | 入参 |
|---|---|---|---|
| `ContentView.swift` | 57 | `@main` + 连线根：持有 `AppModel`、管两个 sheet | — |
| `CaptureScreen.swift` | 194 | 主拍摄界面：预览 + 手势 + `HUD` + `SidePanel`（右侧功能条） | `CaptureUIState` + `CaptureIntents` + `Renderer` |
| `SettingsScreen.swift` | 121 | 参数编辑：改 `@State draft`，Apply/Cancel | `SettingsDraft` + `DeviceInfo` + `SettingsIntents` |
| `DiagnosticsScreen.swift` | 116 | 诊断：整屏 sheet，纯只读（含 `StageDiag`/`GpuReport`/`DiagnosticsSnapshot` 定义） | `DiagnosticsSnapshot` + `onDismiss` |
| `OverlayView.swift` | 72 | 主界面上的 Canvas 叠层（准星读数、面块框、点选气泡） | `CaptureUIState` |

### 3.2 运行时（不含 UI）

| 文件 | 行 | 职责 | 关键成员 |
|---|---|---|---|
| `AppModel.swift` | 748 | `@Observable` 状态聚合 + 每帧管线调度 + 意图实现 | `Pipeline`（`stereo`/`depthData`/`finish`/`diagnostics`）、`AppModel`（`start`/`setSource`/`calibrate`/`measure`/`captureTarget`/`updateDraft→apply`）、契约类型 `CaptureUIState`/`SettingsDraft`/`DeviceInfo`/`DiagnosticsSnapshot`*、协议 `CaptureIntents`/`SettingsIntents` |
| `StereoEngine.swift` | 479 | Metal 管线 + C++ 调用 + `StereoFrame`/`DepthTarget` | `stereo`（全 GPU）、`stereoCPU`（C++ 匹配+深度）、`calibrate`+`score`、`depthData`、`geometry`、`encodeRectify`/`encodeSGM`/`encodeMatch` |
| `CameraController.swift` | 213 | AVFoundation 会话：双摄/系统深度、format 选择、同步输出 | `start`/`stop`、`makeStereo`/`makeDepth`、`dataOutputSynchronizer` |
| `Calibration.swift` | 157 | **第 1 步**：只向系统要参数，不做几何 | `CalibrationSource.intrinsics(of:fov:size:)`、`.calibration(of:)`、`.extrinsics(from:to:)`、`.rig(wide:ultra:)`、`.depthIntrinsics` |
| `Analyzer.swift` | 90 | C++ 分析结果的**薄适配层**（C 结构 → `CGRect`/数组） | `Blob` / `Analysis` / `Analyzer.analyze(_:using:)` |
| `Renderer.swift` | 199 | MetalKit 渲染：显示 / 点云 / 离屏烧录 | `draw`、`encode`、`uploadLabels`、`drawPoints`、`offscreenPass`、矩阵工具 |
| `Capture.swift` | 256 | 拍照/录像/导出（不持有 `AppModel`） | `takePhoto`、`toggleRecording`、`beginPhoto`、`beginRecordingFrame`、`export`、`ply` |

### 3.3 GPU

| 文件 | 行 | 内容 |
|---|---|---|
| `Shaders.metal` | 399 | 计算：`rectify` `unrectifyDepth` `census` `match` `costVolume` `sgmPath` `wtaRight` `wtaLeft` `toDepth`；显示/点云：`fullscreenVertex` `displayFragment` `pointVertex` `pointFragment` |

## 4. 三条操作的调用链

### 4.1 打开软件 → 相机启动 → 标准模式

**启动一次（同步）**

```
StereoVisionApp                        ContentView.swift
└─ ContentView                         ContentView.swift:17
   └─ AppModel()                       AppModel.swift:452
      ├─ StereoEngine(device:)         StereoEngine.swift:148  → 9 条 PSO、3 个 Slot 纹理、SVStereoCore
      ├─ Pipeline(engine:)             AppModel.swift:263    → FrameStore、Analyzer、PipelineSettings
      ├─ Renderer(device:store:)       Renderer.swift:41     → 显示/点云 PSO、labelTex、纹理缓存
      ├─ Capture()                     Capture.swift:11
      ├─ CameraController()            CameraController.swift:45
      ├─ 接线（onStereo/onDepth/onDevices/onError/publish）
      └─ watchOrientation()            AppModel.swift:470   → push{orient}
```

**`.task` 启动相机**

```
ContentView.body                       ContentView.swift:31   .task { await model.startCapture() }
└─ AppModel.startCapture()             AppModel.swift:690     (CaptureIntents)
   └─ AppModel.start()                 AppModel.swift:499
      ├─ AVCaptureDevice.requestAccess(for: .video)
      ├─ refreshOrientation()          AppModel.swift:483
      └─ setSource(.stereo)            AppModel.swift:509
         ├─ push{ cpuMatcher = false }（写 PipelineSettings）
         └─ CameraController.start()   CameraController.swift:63
            └─ makeStereo()            CameraController.swift:98
               ├─ AVCaptureMultiCamSession + 1280×720 16:9 30fps
               ├─ 两个 AVCaptureVideoDataOutput（wide / ultra）+ intrinsic matrix delivery
               ├─ AVCaptureDataOutputSynchronizer(delegate:)
               └─ onDevices?(wide, ultra)
                  └─ StereoEngine.configure()      StereoEngine.swift:183
                     └─ CalibrationSource.rig()    Calibration.swift:129   ← 第 1 步：读 K / R / t
```

**每帧（相机数据队列）**

```
CameraController.dataOutputSynchronizer(...)        CameraController.swift:201
└─ onStereo?(wide, ultra)
   └─ Pipeline.stereo(...)                          AppModel.swift:273
      ├─ PipelineSettings 取值（锁）
      ├─ [可选] StereoEngine.calibrate(...)          StereoEngine.swift:407（仅 calibrate 标志为真时）
      ├─ StereoEngine.stereo(...)                    StereoEngine.swift:283   ← 默认走这条（全 GPU）
      │  ├─ inputs(...)                              StereoEngine.swift:273
      │  │   ├─ CalibrationSource.intrinsics(of:fov:size:)   Calibration.swift:83
      │  │   └─ CVMetalTextureCacheCreateTextureFromImage
      │  ├─ geometry(...)                            StereoEngine.swift:205
      │  │   └─ SVRectification.solve()              Bridge → stereo_rectify_geometry → core/src/geometry.cpp
      │  └─ 一条 command buffer：
      │     encodeRectify ×2 → census ×2 → encodeSGM(costVolume, sgmPath×4, wtaRight, wtaLeft)
      │     → toDepth → unrectifyDepth               （kernel 全在 Shaders.metal）
      └─ Pipeline.finish(...)                        AppModel.swift:327
         ├─ store.latest = frame
         ├─ Analyzer.analyze(_:using:)               Analyzer.swift:47
         │  └─ SVStereoCore.analyzeDepth:into:       Bridge → stereo_analyze → core/src/analyze.cpp
         ├─ Pipeline.diagnostics(kind, frame)        AppModel.swift:297（生成诊断表的文案）
         └─ publish(FrameReport) → dispatch 到主线程
            └─ AppModel.apply(_:)                     AppModel.swift:661
```

**显示（主线程，MTKView 每帧）**

```
Renderer.draw(in:)                        Renderer.swift:87
├─ Renderer.uploadLabels(model.analysis)  Renderer.swift:105
├─ Renderer.encode(...)                   Renderer.swift:114
│  └─ displayFragment（Shaders.metal）← 标准模式 = mode 0，只显示相机图
└─ AppModel.captureTarget()               AppModel.swift:633（既没拍照也没录像时立刻返回 nil）

OverlayView(state:)                       OverlayView.swift:6   （准星读数 / 面块框 / 点选气泡）
HUD(state:)                               CaptureScreen.swift:72
```

标准模式本身只是 `ViewMode.standard`（默认值）→ `shaderIndex == 0`，没有额外分支。

### 4.2 打开诊断界面

```
SidePanel 按钮                             CaptureScreen.swift:162   showDiagnostics = true
└─ ContentView 的 .sheet                   ContentView.swift:40
   ├─ 取数：AppModel.diagnostics            AppModel.swift:435        （只读快照，值类型）
   │  ← gpuReport ← AppModel.apply(_ r: FrameReport) ← FrameReport.stages ← Pipeline.diagnostics
   └─ DiagnosticsScreen(snapshot:onDismiss:)  DiagnosticsScreen.swift:59
      ├─ 概览 / GPU 内存：直接读 snapshot
      ├─ 各阶段表：ForEach(snapshot.stages)  ← Pipeline.diagnostics(AppModel.swift:297) 每帧生成
      └─ Done → onDismiss() → showDiagnostics = false
```

诊断页**不调用任何引擎函数**，只消费每帧已经算好的报告。

### 4.3 打开设置界面 → 改一个参数

```
SidePanel 按钮                             CaptureScreen.swift:165   showSettings = true
└─ ContentView 的 .sheet                   ContentView.swift:32
   ├─ 进参数（都是在主线程算好的值）：
   │   AppModel.draft                     AppModel.swift:401   当前值 → SettingsDraft
   │   AppModel.device                    AppModel.swift:417   DeviceInfo
   │   AppModel.overlayAlpha
   └─ SettingsScreen(draft:device:overlayAlpha:intents:onDismiss:)   SettingsScreen.swift:10

拖动任何一条滑杆 / 切换任何一个开关
└─ 只改 SettingsScreen 自己的 @State var draft     SettingsScreen.swift:15   ← 不进管线

例外：Overlay opacity
└─ SettingsIntents.setOverlayAlpha → AppModel.overlayAlpha  AppModel.swift:745
   （纯显示参数，即时生效；Renderer 下一帧读取）

点 Apply
└─ SettingsIntents.apply(draft) → AppModel.apply(_ d: SettingsDraft)   AppModel.swift:720
   ├─ 写 10 个 private(set) 属性（minZ/maxZ/uniqueness/maxDisparity/matcher/lrCheck/p1/p2/yOffset/flip）
   └─ 一次 push{...} 写进 PipelineSettings（不是 11 个 didSet 各推一次）

下一帧生效位置
├─ maxDisparity / p1 / p2 / uniqueness / lrCheck → StereoEngine.encodeSGM   StereoEngine.swift:244
│  （CPU 后端还会 SVStereoCore.configure 重新分配 C++ 缓冲）
├─ matcher   → StereoEngine.stereo 里的分支（.sgm → encodeSGM，.block → encodeMatch）
├─ minZ/maxZ → DepthParams → toDepth；以及 Shaders.metal 的着色映射
└─ flip/yOffset → StereoEngine.geometry → SVRectification.solve
```

点 **Cancel** → 丢弃 `draft`。注意：`overlayAlpha` 已经即时生效了（已知的小不一致）。

## 5. 一帧的数据流（一句话版）

```
BGRA 双帧 ──GPU rectify──> 640×360 左右图 ──Metal SGM──> 视差 ──toDepth──> 深度(_rolled) ──unrectify──> 宽相机帧深度
                                                                          │
                                     C++ analyze（网格+连通域+分位数）────┴──> Analysis / Blobs / labels / 中心读数
                                                                          │
                                                          Renderer（着色/点云）+ OverlayView + HUD
```

## 6. 评审时值得怀疑的地方（供删减参考）

| # | 对象 | 事实 | 值得问的问题 |
|---|---|---|---|
| 1 | 块匹配后端 `Matcher.block` | `Shaders.metal` 的 `match` kernel（60 行）+ `StereoEngine.encodeMatch` + `stereo()` 里的分支；它同时被 `calibrate` 的 GPU 打分复用 | 这个后端还用得上吗？砍掉它要先把标定打分换成别的代价函数 |
| 2 | 参数结构体两份 | `RectParams`/`MatchParams`/`DepthParams`/`SGMParams` 在 `StereoEngine.swift` 末尾与 `Shaders.metal` 各写一份，没有编译期校验 | 是否值得合并进一个共享头（C 头可以被 MSL/Swift 同时引用） |
| 3 | 两份标定打分 | `StereoEngine.score`（GPU，App 在用）与 `core/src/calibrate.cpp` 的 CPU 打分（只有非 Apple 平台 + 测试在用） | 后者是否只保留给"跨平台"？如果永远只有 iOS，可以删掉 CPU 打分只留搜索 |
| 4 | `AppModel.runCoreSelfTest` | ~45 行 + 设置页一个按钮；功能上已被第 4 个深度来源（`cpuCore`）替代 | 还能删吗 |
| 5 | 两条系统深度分支 | `DepthSource.apple` / `.lidar` + `StereoEngine.depthData`（~60 行）+ `CameraController.makeDepth`（~45 行） | 如果只用自研双目，这两条可以整条删掉 |
| 6 | `CalibrationSource.calibration(of:)` | 设备级读法，只在 `rig()` 里用一次；每帧真正用的是 `intrinsics(of:fov:size:)` | 两者能否合一 |
| 7 | `Renderer` 的矩阵工具 | `perspective`/`translate`/`rotateX/Y/Z`（~25 行）只服务点云视图 | 值得留 |
| 8 | `core/tests/` 1.9k 行 | 不是运行时代码，但它是"改 C++ 不出错"的唯一保障（iOS 侧没有任何测试） | 删测试 = 放弃回归保护 |

**没有被任何地方引用的东西**（可以直接删，已核实）：

- 无。上一轮已清掉 `DisplayOrientation.inverse`、`CameraCalibration.role`、`CameraCalibration.referenceSize`、`CameraRole`。

**检查用的命令**（评审时可以自己跑）：

```bash
# Swift 里是否还有逐像素循环（期望 0）
grep -rn "for .* in 0\.\.<" StereoVision/*.swift

# 某个 Swift 符号还有没有引用
grep -rn "符号名" StereoVision/

# C++ 侧全部测试
cmake -S core -B build/core -DCMAKE_BUILD_TYPE=Release && cmake --build build/core --config Release
cd build/core && ctest -C Release
```

> 注意：iOS 侧**没有单元测试**，删改 Swift 之后只能靠 CI 编译 + 真机验证。
> `project.pbxproj` 是提交进仓库的、CI 不跑 xcodegen——删文件必须同时改它，否则 CI 会静默用旧工程。
