# StereoEngine 后端解耦改造执行方案

> 目标：将双目处理流程与具体算法后端解耦。流程编排保持统一，矫正、匹配、深度计算等阶段可以独立选择 Metal、C++ 或未来 ONNX 实现。
>
> 本方案基于当前 `StereoEngine.swift`（482 行）制定。由于尚未检查 `Pipeline.swift`、`Contracts.swift`、`Shaders.metal`、ObjC++ Bridge 实现及 `project.yml`，执行前必须先核对相关接口，不允许仅凭本方案猜测或重写接口。

## 1. 改造原则

1. **固定流程，阶段可替换**：相机参数获取、几何求解、矫正、匹配、视差转深度、深度回转、分析和输出是处理阶段；Metal/C++/ONNX 是阶段实现，不是三套互相复制的完整流程。
2. **后端实现隔离**：Metal 实现不应散落在通用流程编排中；C++ 调用不应散落在 Metal 实现中；ONNX 以后作为独立匹配实现接入。
3. **不要把所有阶段强行抽象成一个万能 Backend**：按阶段定义窄接口，只有确实需要替换的阶段才抽象。
4. **保留现有行为**：第一阶段只重构组织方式，不改变算法、参数、图像格式、数值结果、计时口径或 UI 行为。
5. **`stereo_process_frame` 保持不动**：它是未来非 Apple 平台全 CPU 流程的入口，不是当前 iOS 流程的编排入口。当前 iOS 继续使用分阶段接口。
6. **iPhone 系统深度不是 SGM 后端**：`depthData` 是另一种数据来源/处理路径，不应伪装成 Metal/C++/ONNX 匹配器之一。
7. ** 如果要python环境验证，使用anaconda activate segzoo。

## 2. 先纠正当前实现的实际边界

当前 `StereoEngine.swift` 中：

- `stereo(...)`：Metal 矫正 + Metal Census/SGM 或 Block Match + Metal 深度转换 + Metal 深度回转。
- `stereoCPU(...)`：Metal 矫正 + CPU 读回 + C++ Census/SGM/WTA + C++ 视差转深度 + C++ 深度回转。
- `depthData(...)`：使用 iPhone/AVFoundation 提供的系统深度图，再通过 C++ `filterDepth` 做范围过滤。

因此，当前所谓 C++ 路径**并非全 CPU 路径**。它仍然依赖 Metal 做极线矫正；CPU 匹配前还存在像素读回。`stereo_process_frame` 才是未来完整 CPU pipeline 的独立入口。

还要注意：`StereoEngine.core` 不只是给 C++ 匹配用。即使走全 Metal 路径，当前准星采样、连通域分析、导出等功能仍可能依赖 `SVStereoCore` / `SVExporter`。不要在拆后端时误删或延迟创建这些公共能力。

## 3. 目标结构

建议先采用以下结构；可以按现有 XcodeGen 源文件组织方式调整目录，但职责边界应保持一致。

```text
StereoVision/
├── Engine/
│   ├── StereoEngine.swift             # 唯一流程编排者
│   ├── StereoTypes.swift              # 阶段输入/输出值类型
│   ├── StereoConfiguration.swift      # 后端组合配置
│   │
│   ├── Geometry/
│   │   └── StereoGeometrySolver.swift # 将相机参数交给现有 C++ 几何接口
│   │
│   ├── Rectification/
│   │   ├── RectificationBackend.swift
│   │   ├── MetalRectifier.swift
│   │   └── CppRectifier.swift         # 可先不实现，仅在确有 C++ 矫正 API 时添加
│   │
│   ├── Matching/
│   │   ├── MatchingBackend.swift
│   │   ├── MetalSGMMatcher.swift
│   │   ├── MetalBlockMatcher.swift    # 如果 Block 与 SGM 生命周期/资源差异明显，独立实现
│   │   ├── CppSGMMatcher.swift
│   │   └── ONNXSGMMatcher.swift       # 未来接入，不在本次实现
│   │
│   ├── Depth/
│   │   ├── DepthBackend.swift
│   │   ├── MetalDepthProcessor.swift
│   │   └── CppDepthProcessor.swift
│   │
│   ├── Calibration/
│   │   └── StereoCalibrator.swift     # 保留现有 C++ 搜索 + GPU score 的组合
│   │
│   └── Metal/
│       ├── MetalContext.swift         # device、command queue、texture cache 等公共资源
│       ├── MetalPipelines.swift       # PSO 创建/持有
│       ├── MetalFrameResources.swift  # Slot、DepthTarget、缓冲和纹理池
│       └── MetalDispatch.swift        # dispatch/encode 公共辅助（仅在确实复用时抽取）
│
├── Runtime/
│   ├── Camera/
│   ├── Pipeline.swift
│   └── ...
│
├── Bridge/
│   └── ...                            # 现有 ObjC++ Bridge，保持 C API 边界
│
└── Shaders.metal
```

不要一次性创建所有空文件。按实施阶段迁移，某个接口或实现确实存在时再建立对应文件。

## 4. 流程编排与阶段接口

### 4.1 统一流程

`StereoEngine` 负责按顺序调用阶段，不直接写具体 Metal kernel，也不直接展开 C++ 匹配细节：

```text
CMSampleBuffer(left/right)
        |
        v
FrameInputProvider
  - 取 CVPixelBuffer
  - 获取每帧 intrinsics
  - 取得已配置的 CameraRig / extrinsics
        |
        v
StereoGeometrySolver
  - 调用现有 SVRectification.solve
  - 生成 Geometry（MW/MU/MUp/K/dir/fB/baseline）
        |
        v
RectificationBackend
  - rectify(left, MW)
  - rectify(right, MU)
        |
        v
MatchingBackend
  - 输入左右矫正图
  - 输出 disparity
        |
        v
DepthBackend
  - disparity -> rectified depth
  - unrectify depth -> wide-camera depth
        |
        v
现有 C++ Analyzer / FrameStore / FrameReport
        |
        v
StereoFrame
```

几何求解和相机参数获取不要混为一谈：相机参数仍由 iOS/AVFoundation 提供；几何矩阵仍可继续通过现有 `SVRectification.solve` 进入 C++ `geometry.cpp` 求解。Metal 矫正只消费几何结果，不负责求解几何。

### 4.2 接口建议

先定义最小接口，不要把 `CMSampleBuffer`、`MTLCommandBuffer`、`SVStereoCore` 全部塞进一个通用协议。

概念示例（具体 Swift 类型要根据项目现有定义调整）：

```swift
protocol RectificationBackend {
    func rectify(
        left: ImageFrame,
        right: ImageFrame,
        geometry: StereoGeometry
    ) throws -> RectifiedPair
}

protocol MatchingBackend {
    func match(
        pair: RectifiedPair,
        geometry: StereoGeometry,
        settings: PipelineSettings
    ) throws -> DisparityFrame
}

protocol DepthBackend {
    func makeDepth(
        disparity: DisparityFrame,
        geometry: StereoGeometry,
        settings: PipelineSettings
    ) throws -> DepthFrame
}
```

这只是接口形状示意，不要求照抄。重点是阶段之间传递明确的数据对象，而不是让匹配器自行访问相机、重新算几何或创建整条 pipeline。

数据对象至少应明确：
- 图像尺寸、像素格式、bytesPerRow/stride。
- CPU 指针的所有权和有效期。
- GPU texture 的所有权和 GPU 使用完成条件。
- disparity 的单位、方向、无效值。
- depth 的单位、坐标系、无效值。
- 计时字段由哪个阶段负责产生。

如果当前阶段输入输出使用 `MTLTexture`，C++ 匹配就需要一个明确的 readback/CPU image adapter；不要把读回逻辑藏在通用 Matcher 中。

## 5. 后端组合：把“模式”变成配置，而不是流程分支

建议把当前三种产品运行模式明确表示为：

### A. Metal 双目模式

```text
Rectification = Metal
Matching      = Metal SGM / Metal Block
Depth         = Metal
```

保持当前 `stereo(...)` 的计算行为和 GPU 单 command buffer 路径（如当前实现确实依赖同一 command buffer 的资源顺序，则拆分后必须保留该优化，不能无意中变成每阶段单独提交）。

### B. C++ 匹配模式（当前混合路径）

```text
Rectification = Metal
Readback      = 明确的像素读回适配步骤
Matching      = C++ SGM
Depth         = C++ disparity-to-depth + C++ unrectify
```

当前 `stereoCPU(...)` 的实际代码是这一模式。命名可考虑 `metalRectifyCppMatch` 或 `hybridCppMode`，避免让人误以为它是完整 CPU pipeline。

### C. iPhone 系统深度模式

```text
Input         = AVDepthData
Depth filter  = C++ filterDepth
```

它不经过自研 stereo rectification、SGM 或 disparity-to-depth。建议将它作为独立的 `DepthSource` / `FrameSource` 路径，不纳入 stereo backend 的组合枚举。

### D. 未来 ONNX 匹配模式

```text
Rectification = Metal
Readback      = 按 ONNX Runtime 执行提供者/输入要求决定
Matching      = ONNX SGM
Depth         = Metal 或 C++
```

ONNX 只替换 Matching 阶段。不要为 ONNX 再复制一份 `stereoONNX()` 全流程。将来若 ONNX 能接入 Metal GPU/Apple Execution Provider，也由 ONNX matcher 自己封装其 session、输入输出和同步策略。

注意：ONNX 模型的实际执行设备、算子支持和性能尚未确定，不能在本次架构重构中假设它一定运行在 GPU。

## 6. 当前 StereoEngine.swift 的迁移映射

| 当前内容 | 目标归属 | 处理建议 |
|---|---|---|
| `StereoFrame` | `StereoTypes.swift` | 先保留字段和初始化行为；后续再讨论是否拆分 GPU/CPU 输出资源 |
| `DepthTarget` | `MetalFrameResources.swift` | Metal 资源，不属于算法流程 |
| `device`、`queue` | `MetalContext.swift` | Metal 公共上下文 |
| PSO 属性及初始化 | `MetalPipelines.swift` | 统一创建与持有 |
| `costBuffer`、`sumBuffer`、`dispRight` | Metal matcher 资源 | 不放在通用 StereoEngine |
| `cache`、`leftPool`、`slots`、`appleTargets` | Metal/PixelBuffer 资源管理 | 明确线程安全和 slot 使用规则 |
| `rig`、`configure`、`inputs`、`intrinsics` | FrameInputProvider / Camera 参数适配 | 相机参数仍由 Swift 取得 |
| `Geometry`、`geometry(...)` | `StereoGeometrySolver.swift` | 保留 `SVRectification.solve` |
| `dispatch`、`encodeRectify` | `MetalRectifier.swift` 或 Metal 公共工具 | 不让 C++ matcher 看到 Metal PSO |
| `encodeMatch`、`encodeSGM` | 对应 Metal Matcher | 从编排类迁出 |
| `stereo(...)` | `StereoEngine.processStereoFrame` | 只保留流程编排 |
| `stereoCPU(...)` | 由配置选择 C++ Matcher/DepthBackend | 删除重复的整条流程函数 |
| `calibrate(...)`、`score(...)` | `StereoCalibrator.swift` + Metal scoring | 先保留现有 C++ 搜索 + GPU 打分机制 |
| `depthData(...)` | 独立 SystemDepthProcessor / source path | 不纳入 stereo matcher backend |
| `RectParams` 等 shader 参数结构 | Metal 专属文件 | 与对应 shader 保持 ABI 一致 |

特别注意：当前 `stereoCPU` 的 `depth(fromDisparity:)` 和 `unrectifyDepth(...)` 都是 C++ 调用；不要按注释或旧设计描述误迁移成 Metal。

## 7. 必须先处理的资源与并发问题

重构前先检查调用线程和资源复用。当前类标记为 `@unchecked Sendable`，并有可变属性：
- `rig`
- `slotIndex`
- `costScratch`
- `appleTargets`
- CVMetalTextureCache / PixelBufferPool

需要查明 `stereo`、`stereoCPU`、`calibrate/score`、`depthData` 是否可能并发执行。不能仅因它们位于同一实例就假设串行。

另外：
1. `score` 使用 `slots[0]`，需要保证它不会与正常帧处理同时覆盖该 slot。
2. `stereoCPU` 的 `defer` 会在函数返回时解锁输入 pixel buffer；必须确认 C++ 调用是同步完成，不会保留该指针。
3. `StereoFrame` 的 `retained` 数组保留了 CVMetalTexture / SampleBuffer 等对象；迁移时必须保留相同生命周期。
4. `DepthTarget` 的共享 MTLBuffer 被 CPU 写入后，Metal Renderer 读取前需要有正确的同步关系。
5. GPU command buffer 完成前不能复用其仍在使用的 slot/texture/buffer。
6. `StereoFrame.depthPointer` 指向共享 buffer；必须说明谁可读写、何时失效，避免跨帧悬空或数据竞争。

本次不要顺手大改线程模型。先画出当前真实调用线程和资源生命周期，再保证拆分后行为一致。

## 8. 分阶段实施步骤

### Phase 0：补齐依赖信息与基线

Agent 先读取：
- `StereoEngine.swift`
- `Pipeline.swift`
- `Contracts.swift`
- `Shaders.metal`
- `Bridge` 下 `SVStereoCore`、`SVRectification`、`SVExporter` 的头文件和实现
- `project.yml`
- 当前 C++ API 头文件及 `stereo_c_api.cpp`

输出：
1. 当前函数调用关系图。
2. 每帧运行在哪个队列/线程。
3. 每个 MTLTexture/MTLBuffer/CVPixelBuffer 的创建者、写入者、读取者和释放时机。
4. 当前三种模式的实际执行阶段。
5. 现有计时字段与诊断页字段映射。

先构建并运行现有测试，记录基线。不得先改代码。

### Phase 1：只迁移资源和类型，不改流程

1. 将 `StereoFrame`、`DepthTarget`、`Geometry` 和 shader 参数类型迁移到职责清晰的文件。
2. 将 Metal device/queue、PSO、Slot/纹理缓冲资源分别迁出。
3. `StereoEngine` 暂时通过属性持有这些对象。
4. 不改变函数调用顺序、参数、kernel、command buffer 数量、等待位置和计时范围。
5. 编译并真机验证 Metal 模式、C++ 混合模式、iPhone 系统深度模式。

### Phase 2：抽出几何求解与相机输入

1. 将 `inputs(...)`、intrinsics 获取和 `rig` 参数管理迁至输入适配层。
2. 将 `geometry(...)` 迁至 `StereoGeometrySolver`。
3. 继续调用现有 `SVRectification.solve`，不要复制几何公式到 Swift。
4. 验证 MW/MU/MUp/K/dir/fB/baseline 与重构前一致。

### Phase 3：抽出 Metal 矫正和 Metal 匹配

1. 将 `encodeRectify` 迁至 `MetalRectifier`。
2. 将 Census、Cost Volume、SGM Path、WTA、Block Match 迁至 Metal matcher。
3. 先保留同一 command buffer 的编码方式，避免性能和同步行为变化。
4. 保留当前 PSO、Buffer、Texture 的复用策略。
5. 逐项比较重构前后 disparity/depth 输出。

### Phase 4：抽出 C++ 匹配与深度

1. 将右图纹理读回和 CVPixelBuffer 锁定封装成明确的 ImageReadback/CPU Image adapter。
2. 将 `core.matchLeft(...)` 封装为 `CppSGMMatcher`。
3. 将 `core.depth(...)` 和 `SVStereoCore.unrectifyDepth(...)` 封装为 `CppDepthProcessor`。
4. 保持 C++ 调用同步完成后才释放临时指针和 pixel buffer lock。
5. 保留现有 `readbackMs`、`matchMs`、`depthMs` 计时口径。
6. 不把 C++ 混合模式误命名为完整 CPU backend。

### Phase 5：统一编排

`StereoEngine` 最终只负责：
1. 接收同步后的左右相机帧和设置。
2. 取得图像与相机参数。
3. 求解 Geometry。
4. 调用配置的 RectificationBackend。
5. 调用配置的 MatchingBackend。
6. 调用配置的 DepthBackend。
7. 组装现有 `StereoFrame`。
8. 将结果交给既有 Pipeline/Analyzer。

提供明确的配置，例如：

```swift
enum StereoMode {
    case metal
    case cppHybrid
}
```

系统深度继续由独立 source/mode 处理。未来 ONNX 接入时增加匹配实现或配置，不增加另一条复制的全流程函数。

### Phase 6：标定独立化

1. 保留 C++ `stereo_calibrate_search` 的候选搜索。
2. 保留 GPU `score` 的计算策略。
3. 将 score 所需的 Metal kernel/资源依赖显式注入标定器。
4. 确保标定和正常帧处理不会并发写入同一 Slot。
5. 本次不删除 CPU `stereo_calibrate`，也不改 `stereo_process_frame`。

### Phase 7：回归与清理

至少验证：
- Metal 模式：rectify、SGM/Block、depth、unrectify。
- C++ 混合模式：Metal rectify、readback、C++ match、C++ depth/unrectify。
- iPhone 系统深度模式。
- 自动标定及 flip/yOffset 更新。
- 设置 Apply 后下一帧生效。
- Analyzer、准星采样、点云导出、Renderer。
- 连续运行、切换模式、相机中断恢复。
- 计时数据与旧版本一致（允许正常测量波动）。

C++ 现有测试继续运行；Metal 侧至少用固定输入做重构前后输出对比。最后检查 XcodeGen 源文件清单脚本。

## 9. 验收标准

- `StereoEngine.swift` 不再直接持有和配置所有 Metal PSO/Buffer/Texture 资源。
- `StereoEngine` 不再包含完整展开的 Metal SGM kernel 调度细节。
- `StereoEngine` 不再包含 C++ 匹配和深度处理的指针级细节。
- Metal Matcher、C++ Matcher、未来 ONNX Matcher 可以独立替换。
- iPhone 系统深度路径与自研双目匹配路径分离。
- Geometry 只计算一次，结果在阶段间显式传递。
- 不复制 `stereo_process_frame` 的逻辑，也不修改它。
- 现有功能、结果、计时语义和资源生命周期不发生非预期变化。
- 所有新增 Swift 源文件被 XcodeGen 纳入工程。
- 完成编译、C++ 测试和真机回归。

## 10. 给 Agent 的执行纪律

1. 先完成 Phase 0 的代码阅读和依赖图，再提出具体迁移计划。
2. 每个 Phase 单独提交/形成可审查 diff；每阶段完成后编译和测试。
3. 不要一次性重写整个 `StereoEngine.swift`。
4. 不要改 C ABI，不要改 C++ 算法，不要改 shader 数值逻辑。
5. 不要为了抽象而创建大量只有一个调用点的协议和 Manager。
6. 不要把 `CMSampleBuffer`、`MTLTexture`、裸指针的生命周期藏在不透明的通用数据对象里。
7. 遇到未读到的 Bridge/API 定义时，先回到源码核对，不得猜函数签名。
8. 如果发现现有线程安全或生命周期问题，先单独报告证据和风险，不要在本轮架构迁移中顺手引入大范围并发改造。

**建议实施顺序：Phase 0 → 1 → 2 → 3 → 4 → 5 → 6 → 7。**
