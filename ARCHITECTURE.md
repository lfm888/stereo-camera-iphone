# 架构地图

> 用途：改代码 / 删文件之前先看这一页。行数是 `wc -l` 实测值（2026-11）。
> 代码量 ≈ 8.6k 行：C++ 实现 3.4k、C++ 测试 2.4k、Swift 2.9k、ObjC++ 桥 1.2k、Metal 0.4k。
> 结论先放这里：**iOS 上没有任何一段 C++ 算法是被绕过的**，反向也不成立——C++ 从不持有平台指针。

## 0. 五层，依赖只向下

```
StereoVision/*.swift          SwiftUI 界面 + 运行时（相机/引擎/渲染/导出）
        │  只读快照 + intent 协议
StereoVision/Bridge/*.mm      ObjC++ 门面：指针、生命周期、simd ↔ float[9] 换乘
        │  纯 C 头（POD + 不透明句柄）
core/include + core/src       C++ 算法（零 Apple 依赖、零第三方依赖）
core/tests                    C++ 侧全部测试（Windows/Linux/macOS 都跑）
Shaders.metal                 GPU kernel（iOS 的快路径）
```

四条硬规矩：

1. `core/` 不许 include 任何 Apple 头；`Swift` 不许 include 任何 C++ 头。
2. `core/include/stereo/*.hpp` 只给 C++ 与桥接层看；`stereo_c_api.h` 是唯一的 C 契约。
3. 界面层只拿**值类型**（`CaptureUIState` / `DiagnosticsSnapshot` / `DeviceInfo`）和**协议**（`CaptureIntents` / `SettingsIntents`）——"诊断页不能偷改状态"是编译器保证的。
4. **工程文件不提交**：`StereoVision.xcodeproj` 由 CI 用 `xcodegen generate --spec project.yml` 生成，`scripts/check_xcodeproj_sources.sh` 保证不漏文件。增删源文件只动文件本身。

## 1. `core/` — C++ 算法

### 1.1 对外契约：`include/stereo/stereo_c_api.h`（478 行，版本 150）

**24 个导出符号**（`stereo_*`）+ POD 结构体。分六组：

| 组 | 函数 | 干什么 |
|---|---|---|
| 生命周期 | `stereo_create` / `stereo_destroy` / `stereo_config_default` / `stereo_configure` / `stereo_last_error` | context + 参数；内部缓冲按尺寸复用 |
| 第 2 步 极线矫正 | `stereo_rectify_geometry` | 内参+外参 → MW/MU/MUp/K/Pl/Pr/Q |
| | `stereo_rectify_image` / `stereo_unrectify_depth` | 单应重采样 / 深度回转（Metal kernel 的 CPU 孪生） |
| 第 3 步 匹配 | `stereo_match` | Census 7×7 + costVolume + SGM 4 路径 + 唯一性 + LR 检查 + 亚像素 |
| 第 4 步 深度 | `stereo_disparity_to_depth` | 视差 → 深度（3×3 中值 + 范围截断） |
| 第 5 步 后处理 | `stereo_analyze` / `stereo_analyze_grid` | 深度连通域 → 面块 + 标签图 + 准星读数 |
| | `stereo_sample_depth` | 归一化矩形内的第 q 分位数 |
| 导出 | `stereo_point_cloud` / `stereo_point_cloud_capacity` / `stereo_depth_filter` / `stereo_depth_to_u16_mm` / `stereo_copy_depth` | PLY 点云、深度裁剪、16 位毫米打包、跨度拷贝 |
| 自动标定 | `stereo_calibrate`（CPU 全流水线）/ `stereo_calibrate_search`（平台提供打分回调） | 在 flip×dy 上搜索行对齐最好的参数 |
| 全流水线 | `stereo_process_frame` | **第 2..5 步一条龙**，给非 Apple 前端；iOS 不走它（见 §6.1） |
| 参数表 | `stereo_param_count` / `stereo_param_table` / `stereo_param_find` | 名称/分组/范围/步长/默认值/单位/枚举（**唯一真源**，见 §1.3） |

### 1.2 实现

| 文件 | 行 | 职责 | 主要入口 |
|---|---|---|---|
| `src/sgm.cpp` | 268 | 第 3/4 步本体（逐 kernel 对齐 MSL） | `rgb_to_luma` `census` `cost_volume` `sgm_all_paths` `wta_right` `wta_left` `to_depth` |
| `src/geometry.cpp` | 213 | 3×3 小工具 + 极线矫正几何 | `rectify_geometry` |
| `src/rectify.cpp` | 142 | 重采样（4 次超采样 + 双线性）/ 深度回转（最近邻） | `rectify_image` `unrectify_depth` |
| `src/analyze.cpp` | 207 | 降采样成网格 → 4 邻域连通域 → 面块统计 | `analyze_depth` |
| `src/cloud.cpp` | 187 | PLY（`std::to_chars`）、深度裁剪/打包/拷贝 | `point_cloud_ply` 等 |
| `src/calibrate.cpp` | 222 | 自动标定搜索；CPU 打分版也在里面 | `calibrate_search` `calibrate_rectification` |
| `src/sample.cpp` | 58 | 自适应抽样 + 第 q 分位数 | `sample_quantile` |
| `src/params.cpp` | 74 | **参数描述表**（11 条），`stereo_config_default` 的默认值也来自它 | `param_table` `param_default` |
| `src/stereo_c_api.cpp` | 903 | 参数校验 + 缓冲管理 + POD 翻译 + `process_frame` 的组合调用（**不含算法**） | 24 个导出符号 |

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

## 3. `StereoVision/` — Swift 前端（2864 行）

### 3.1 界面（5 个文件，只读快照 + intent）

| 文件 | 行 | 职责 | 入参 |
|---|---|---|---|
| `ContentView.swift` | 57 | `@main` + 连线根：持有 `AppModel`、管设置 sheet | — |
| `CaptureScreen.swift` | 194 | 主拍摄界面：预览 + 手势 + `HUD` + `SidePanel` | `CaptureUIState` + `CaptureIntents` + `Renderer` |
| `SettingsScreen.swift` | 172 | 设置：按参数表生成控件，改 `@State draft`，Apply/Cancel | `SettingsDraft` + `DeviceInfo` + `SettingsIntents` |
| `DiagnosticsScreen.swift` | 142 | 诊断：整屏 sheet，纯只读；含 `StageDiag` 与 `PipelineKind.rows(_:)` 文案 | `DiagnosticsSnapshot` + `onDismiss` |
| `OverlayView.swift` | 72 | 主界面 Canvas 叠层（准星读数、面块框、点选气泡） | `CaptureUIState` |

### 3.2 运行时（不含 UI）

| 文件 | 行 | 职责 | 关键成员 |
|---|---|---|---|
| `Contracts.swift` | 308 | 共享词汇 + 界面契约 + 诊断值类型 | `ViewMode`/`Matcher`/`DisplayOrientation`、`SettingsDraft`（含 `subscript(key:)`）、`DeviceInfo`/`CaptureUIState`、`CaptureIntents`/`SettingsIntents`、`GpuReport`/`DiagnosticsSnapshot` |
| `Pipeline.swift` | 154 | 每帧后台管线与报告类型 | `PipelineSettings`、`PipelineKind`、`StageTimings`、`FrameReport`、`FrameStore`、`Pipeline`（`stereo`/`depthData`/`timings(of:)`/`finish`） |
| `AppModel.swift` | 369 | 状态聚合 + 接线 + 意图实现 | `start`/`setSource`/`calibrate`/`measure`/`captureTarget`/`metadata`/`outputSize` + 只读视图 `ui`/`diagnostics`/`draft`/`device` |
| `StereoEngine.swift` | 481 | Metal 管线 + C++ 调用 + `StereoFrame`/`DepthTarget` | `stereo`（全 GPU）、`stereoCPU`（GPU 矫正 + C++ 匹配/深度）、`calibrate`+`score`、`depthData`、`geometry`、`encodeRectify`/`encodeSGM`/`encodeMatch` |
| `CameraController.swift` | 213 | AVFoundation 会话：双摄/系统深度、format 选择、同步输出 | `start`/`stop`、`makeStereo`/`makeDepth`、`dataOutputSynchronizer` |
| `Calibration.swift` | 157 | **第 1 步**：只向系统要参数，不做几何 | `CalibrationSource.intrinsics(of:fov:size:)`、`.calibration(of:)`、`.extrinsics(from:to:)`、`.rig(wide:ultra:)`、`.depthIntrinsics` |
| `Analyzer.swift` | 90 | C++ 分析结果的薄适配层（C 结构 → `CGRect`/数组） | `Blob`/`Analysis`/`Analyzer.analyze(_:using:)` |
| `Renderer.swift` | 199 | MetalKit 渲染：显示 / 点云 / 离屏烧录 | `draw`、`encode`、`uploadLabels`、`drawPoints`、`offscreenPass` + 矩阵工具 |
| `Capture.swift` | 256 | 拍照/录像/导出（不持有 `AppModel`） | `takePhoto`、`toggleRecording`、`beginPhoto`、`beginRecordingFrame`、`export`、`ply` |

### 3.3 GPU

| 文件 | 行 | 内容 |
|---|---|---|
| `Shaders.metal` | 399 | 计算：`rectify` `unrectifyDepth` `census` `match` `costVolume` `sgmPath` `wtaRight` `wtaLeft` `toDepth`；显示/点云：`fullscreenVertex` `displayFragment` `pointVertex` `pointFragment` |

## 4. 三条操作的调用链

### 4.1 打开软件 → 相机启动 → 标准模式

**启动一次（同步）**

```
StereoVisionApp                   ContentView.swift
└─ ContentView                    ContentView.swift
   └─ AppModel()                  AppModel.swift
      ├─ StereoEngine(device:)    StereoEngine.swift   → 9 条 PSO、3 个 Slot 纹理、SVStereoCore
      ├─ Pipeline(engine:)        Pipeline.swift       → FrameStore、Analyzer、PipelineSettings
      ├─ Renderer(device:store:)  Renderer.swift       → 显示/点云 PSO、labelTex、纹理缓存
      ├─ Capture()                Capture.swift
      ├─ CameraController()       CameraController.swift
      ├─ 接线 onStereo / onDepth / onDevices / onError / publish
      └─ watchOrientation()       AppModel.swift       → push{orient}
```

**开流**

```
ContentView.body                  .task { await model.startCapture() }
└─ AppModel.startCapture()        AppModel.swift      （CaptureIntents）
   └─ AppModel.start()            → AVCaptureDevice 授权 → refreshOrientation → setSource(.stereo)
      └─ CameraController.start()  CameraController.swift
         └─ makeStereo()          → MultiCam session + 两个 VideoDataOutput + Synchronizer
            └─ onDevices → StereoEngine.configure()   StereoEngine.swift
               └─ CalibrationSource.rig()             Calibration.swift   ← 第 1 步：读 K / R / t
```

**每帧（相机数据队列）**

```
CameraController.dataOutputSynchronizer(...)     CameraController.swift
└─ onStereo → Pipeline.stereo                    Pipeline.swift
   ├─ [可选] StereoEngine.calibrate              StereoEngine.swift   （calibrate 标志为真时；
   │         C++ search + GPU 打分 → SVStereoCore.calibrate:flip0:dy0:）
   ├─ StereoEngine.stereo                        StereoEngine.swift   ← 默认全 GPU 路径
   │  ├─ inputs()            → CalibrationSource.intrinsics(of:fov:size:) + CVMetalTextureCache
   │  ├─ geometry()          → SVRectification.solve → stereo_rectify_geometry → geometry.cpp
   │  └─ 一条 command buffer：encodeRectify ×2 → census ×2 → encodeSGM
   │                          → toDepth → unrectifyDepth      （kernel 全在 Shaders.metal）
   └─ Pipeline.finish                            Pipeline.swift
      ├─ store.latest = frame
      ├─ Analyzer.analyze(_:using:)              Analyzer.swift
      │  └─ SVStereoCore.analyzeDepth:into:      → stereo_analyze → analyze.cpp + sample.cpp
      ├─ Pipeline.timings(of:) → FrameReport(kind / timings)
      └─ publish → AppModel.apply(_ r:)          AppModel.swift
```

**显示（MTKView 每帧）**

```
Renderer.draw(in:)                Renderer.swift
├─ uploadLabels(model.analysis)   → labelTex.replace
├─ encode(...)                    → displayFragment（标准模式 = mode 0，只贴相机图）
└─ AppModel.captureTarget()       AppModel.swift   （没拍照没录像时立刻 nil）

OverlayView(state:)               OverlayView.swift（准星读数/面块框/点选气泡）
HUD(state:)                       CaptureScreen.swift
```

标准模式只是 `ViewMode.standard`（默认值）→ `shaderIndex == 0`，无额外分支。

### 4.2 打开诊断界面

```
SidePanel 按钮                    CaptureScreen.swift   showDiagnostics = true
└─ ContentView 的 .sheet          ContentView.swift
   ├─ 取数：AppModel.diagnostics   AppModel.swift        （只读快照，值类型）
   │  ← gpuReport(kind/timings) ← AppModel.apply(_ r: FrameReport) ← Pipeline.timings(of:)
   └─ DiagnosticsScreen(snapshot:onDismiss:)   DiagnosticsScreen.swift
      ├─ 概览 / GPU 内存：直接读 snapshot
      ├─ 各阶段表：ForEach(PipelineKind.rows(timings))   文案在诊断页里
      └─ Done → onDismiss → showDiagnostics = false
```

诊断页**不调用任何引擎函数**，只消费每帧算好的报告。

### 4.3 打开设置界面 → 改任意参数

```
SidePanel 按钮                    CaptureScreen.swift   showSettings = true
└─ ContentView 的 .sheet          ContentView.swift
   ├─ 进参：AppModel.draft / .device / .overlayAlpha
   └─ SettingsScreen(draft:device:overlayAlpha:intents:onDismiss:)   SettingsScreen.swift

界面构建
└─ SVParameterTable.allParameters()   Bridge → stereo_param_count / stereo_param_table → params.cpp
   ├─ group → Section（depth / matching / display）
   ├─ kind  → Slider / Toggle / segmented Picker
   └─ min/max/step/unit → 滑杆范围与格式化（Swift 里没有硬编码区间）

拖动滑杆 / 切开关
└─ 只改 SettingsScreen 自己的 @State draft（走 SettingsDraft.subscript(key:)）← **不进管线**
   例外：overlay_alpha → SettingsIntents.setOverlayAlpha → AppModel.overlayAlpha（即时生效）

点 Apply
└─ SettingsIntents.apply(draft) → AppModel.apply(_ d: SettingsDraft)   AppModel.swift
   ├─ 写 10 个 private(set) 属性
   └─ **一次** push{...} 写进 PipelineSettings（不是 11 个 didSet 各推一次）

下一帧生效位置
├─ maxDisparity / p1 / p2 / uniqueness / lrCheck → StereoEngine.encodeSGM
│  （CPU 后端还会 SVStereoCore.configure 重新分配 C++ 缓冲）
├─ matcher   → StereoEngine.stereo 的分支（.sgm → encodeSGM，.block → encodeMatch）
├─ minZ/maxZ → DepthParams → toDepth；以及 displayFragment 的着色映射
└─ flip/yOffset → StereoEngine.geometry → SVRectification.solve
```

点 **Cancel** → 丢弃 `draft`（`overlay_alpha` 已生效，这是唯一的不一致）。

## 5. 一帧的数据流（一句话）

```
BGRA 双帧 ─GPU rectify→ 640×360 左右图 ─Metal SGM→ 视差 ─toDepth→ 深度(rolled) ─unrectify→ 宽相机帧深度
                                                              │
                        C++ analyze（网格+连通域+分位数）──────┴→ Analysis/Blobs/labels/中心读数
                                                              │
                                             Renderer（着色/点云）+ OverlayView + HUD
```

## 6. 评审时值得怀疑的地方（供删减参考）

| # | 对象 | 事实 | 值得问的问题 |
|---|---|---|---|
| 1 | `stereo_process_frame` + `SVFrameRequest`/`SVFrameTimings` + `-processFrame:` | **Swift 侧零调用**，只有 `frame_test` 与非 Apple 前端会用 | iOS 上要不要留？若将来做"全 CPU 后端"或 Qt 前端，它是入口；只服务 iOS 就是纯死代码（约 350 行 C++/桥接） |
| 2 | `stereo_calibrate`（CPU 全流水线版） | 只有测试用；iOS 走 `stereo_calibrate_search` + GPU 打分（CPU 版 640×360 实测 3.4 s） | 只保留 `_search` 可删 ~120 行，但会失去"无 GPU 平台也能标定" |
| 3 | 块匹配后端 `Matcher.block` | `Shaders.metal` 的 `match` kernel + `StereoEngine.encodeMatch` + `stereo()` 的分支；**同时被标定的 GPU 打分复用** | 后端还用得上吗？砍它要先把标定打分换成别的代价函数 |
| 4 | 两条系统深度分支 | `DepthSource.apple`/`.lidar` + `StereoEngine.depthData`（~60 行）+ `CameraController.makeDepth`（~45 行） | 如果只用自研双目，整条可删 |
| 5 | `CalibrationSource.calibration(of:)` / `.extrinsics(from:to:)` | 只在 `rig()` 里各用一次；每帧真正用的是 `intrinsics(of:fov:size:)` | 三者能否合一 |
| 6 | 双份"视差后"实现 | `stereo_process_frame` 内部调 `to_depth`/`unrectify`，iOS 的 `stereoCPU` 调 `depthFromDisparity`+`unrectifyDepth`（同一对函数、两次调用） | 可合并成 `stereo_process_disparity`（省一次 malloc + 一次跨桥）；FAQ Q18 有分析 |
| 7 | `core/tests/` 2351 行 | 不是运行时代码，但它是"改 C++ 不出错"的唯一保障（**iOS 侧没有任何测试**） | 删测试 = 放弃回归保护 |
| 8 | `Renderer` 的矩阵工具 | `perspective`/`translate`/`rotateX/Y/Z`（~25 行）只服务点云视图 | 值得留 |
| 9 | `tools/` 下的脚本 | `sgm_ref.py`+`gen_golden.py` 是金标准的来源（有用）；`viewer.html` / `make_demo_export.py` 用途不明 | 后两个确认没人用可删 |
| 10 | 文档 | `test.md` 61 KB（过程记录）、`FAQ.md` 18 KB、本文件 | 历史过程文档，按需留 |

**已核实"无引用"的东西**：无。之前清掉的 `DisplayOrientation.inverse`、`CameraCalibration.role/referenceSize`、`CameraRole`、`runCoreSelfTest`、`StereoCore` target 都已不在。

**检查用的命令**（评审时可以自己跑）：

```bash
# Swift 里是否还有逐像素循环（期望 0）
grep -rn "for .* in 0\.\.<" StereoVision/*.swift

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
