# StereoEngine 改造前基线（Phase 0）

> 这份文件是后端拆分改造的 **Phase 0 产出**：改代码**之前**先把当前的调用关系、线程、
> 先把当前的调用关系、线程、资源生命周期、三条路径的实际阶段和计时口径记下来。
> 后面每一次迁移都以这份记录为对照，行为不一致就是回归。
>
> 记录对象：改造前的 `StereoVision/StereoEngine.swift`（481 行）+ `Pipeline.swift`。

## 1. 当前函数调用关系

```
AppModel.init (main)
└─ StereoEngine(device:)
   ├─ SVStereoCore(maxDisparity:64, p1:8, p2:96, uniqueness:0.1, lrCheck:true)
   ├─ makeCommandQueue / makeDefaultLibrary / 9 × makeComputePipelineState
   ├─ costBuffer / sumBuffer / dispRight（SGM 专用）
   ├─ CVMetalTextureCacheCreate / CVPixelBufferPoolCreate（左图输出）
   └─ 3 × Slot（right / censusL / censusR / disp / cost / depthRolled / DepthTarget）

CameraController.dataOutputSynchronizer (camera.data，串行)
├─ onDevices  → StereoEngine.configure(wide:ultra:) → CalibrationSource.rig
├─ onStereo   → Pipeline.stereo
│   ├─ [calibrate 标志] StereoEngine.calibrate → SVStereoCore.calibrate(score:)
│   │                                             └─ score: geometry + rectify×2 + census×2 + match
│   ├─ cpuMatcher ? StereoEngine.stereoCPU : StereoEngine.stereo
│   └─ Pipeline.finish → Analyzer.analyze → publish（dispatch 回主线程）
└─ onDepth    → Pipeline.depthData → StereoEngine.depthData → SVExporter.filterDepth

Renderer.draw (main，MTKView)
├─ store.latest（StereoFrame）
└─ AppModel.captureTarget → Capture.beginPhoto（C++ copyDepth / PLY / 打包）
AppModel.measure / apply (main) → queryCore.sampleDepth（独立的 SVStereoCore）
```

## 2. 每帧跑在哪个线程

| 调用 | 线程 / 队列 | 备注 |
|---|---|---|
| `StereoEngine.stereo` / `.stereoCPU` / `.depthData` / `.calibrate` | `CameraController.dataQueue`（串行） | 三条路径共用一条队列，所以**同一实例上它们互不并发** |
| `StereoEngine.configure` | `CameraController.sessionQueue` | 写 `rig`；与 `dataQueue` 上的读**没有同步**（改造前就存在，本轮不动） |
| `Analyzer.analyze` | `CameraController.dataQueue` | 复用同一个 `SVAnalysis` 容器 |
| `Renderer.draw` / `Capture.export` / `AppModel.measure` | 主线程 | 读 `StereoFrame` 的纹理与 `depthPointer` |

结论：帧处理路径（stereo / stereoCPU / calibrate / depthData）是**串行**的。`slotIndex`
的轮换因此不需要加锁，但主线程读 `depthPointer` / `depth` 纹理与下一帧 GPU 写入之间
没有显式同步 —— 这是已知的既有风险（本轮只记录，不做并发改造）。

## 3. 资源的创建者 / 写入者 / 读取者 / 释放时机

| 资源 | 创建 | 写入 | 读取 | 释放 |
|---|---|---|---|---|
| `CVMetalTextureCache`（引擎） | `StereoEngine.init` | 每帧 `texture(pb)` | GPU（矫正输入） | 引擎实例 |
| `CVPixelBufferPool`（左图输出） | `StereoEngine.init` | 每帧 GPU `rectify` 写入 | C++ 读回（stereoCPU）、Rectified pair / Anaglyph 视图 | 引擎实例 |
| `Slot.right` ×3 | `init` | GPU `rectify`（超广角那一路） | GPU census、显示（right） | 引擎实例 |
| `Slot.censusL/R` | `init` | GPU `census` | GPU 匹配 | 引擎实例 |
| `Slot.disp` | `init` | GPU WTA / block match | GPU `toDepth`、显示（disparity 视图不读它） | 引擎实例 |
| `Slot.cost` | `init`（shared） | GPU `match` | 标定 `score`（`getBytes`） | 引擎实例 |
| `Slot.depthRolled` | `init` | GPU `toDepth` | GPU `unrectifyDepth` | 引擎实例 |
| `Slot.target`（`DepthTarget`） | `init` | GPU `unrectifyDepth` **或** C++ `unrectifyDepth` | GPU 显示、C++ Analyzer、点选测距、导出 | 引擎实例 |
| `costBuffer` / `sumBuffer` | `init`（private） | GPU costVolume / sgmPath | GPU sgmPath / WTA | 引擎实例 |
| `dispRight` | `init`（private） | GPU `wtaRight` | GPU `wtaLeft` | 引擎实例 |
| `appleTargets` ×3 | 首帧 `depthData` | C++ `filterDepth`（直接写共享 buffer） | 显示 / Analyzer / 导出 | 引擎实例（尺寸变化时重建） |
| `costScratch`（`[Float]`） | 首次 `score` | `getBytes` | `SVExporter.meanNonNegative` | 引擎实例 |
| `StereoFrame.retained` | 每帧 | — | — | `StereoFrame` 释放时（下一帧替换 `FrameStore.latest`） |

`retained` 里放的正是 **CVMetalTexture（wcv / ucv / lcv）+ 宽相机 CMSampleBuffer**：
纹理是借来的 view，底层 CVPixelBuffer 被回收后立即失效，所以三者必须一起持有。
`StereoFrame` 另外持有 `pairLeftPB`（左图 CVPixelBuffer），给 C++ 读回/导出用。

## 4. 三条路径的实际阶段

| 路径 | 入口 | 矫正 | 匹配 | 视差→深度 | 深度回转 |
|---|---|---|---|---|---|
| Metal 双目 | `stereo` | Metal `rectify` | Metal SGM 或 Metal Block | Metal `toDepth` | Metal `unrectifyDepth` |
| C++ 混合 | `stereoCPU` | Metal `rectify` | **CPU**（`core/src/sgm.cpp`） | **CPU** `depth(fromDisparity:)` | **CPU** `unrectifyDepth` |
| 系统深度 | `depthData` | 无 | 无 | 无 | 无（只做 `filterDepth`） |

注意：`stereoCPU` **不是**全 CPU 流水线 —— 矫正仍在 GPU，匹配前还有一次读回。
真正的全 CPU 编排是 C++ 的 `stereo_process_frame`，iOS 不走它，本轮也不动它。

## 5. 计时字段与诊断页映射

| `StereoFrame` 字段 | 计量范围 | `StageTimings` | 诊断页行 |
|---|---|---|---|
| `gpuMs` | Metal 路径整条 command buffer 的 `gpuEndTime - gpuStartTime` | `gpuMs` | 「rectify → … → unrectify」 |
| `gpuRectifyMs` | 混合路径第一个 command buffer（两路 rectify） | `gpuRectifyMs` | 「rectify（两路单应重采样）」 |
| `readbackMs` | 锁 CVPixelBuffer + 两次临时分配 + `getBytes` | `readbackMs` | 「像素读回」 |
| `matchMs` | `core.matchLeft` | `matchMs` | 「census → … → wta」 |
| `depthMs` | `depth(fromDisparity:)` + `unrectifyDepth`（含临时缓冲分配） | `depthMs` | 「toDepth → unrectify」 |
| `cpuMs` | `readbackMs + matchMs + depthMs`（算出来的） | `cpuMs` | 概览「CPU 合计」 |

## 6. 改造前必须保留的行为

1. Metal 路径一帧只有**一条** command buffer，`waitUntilCompleted` 后立即组帧。
2. slot 轮换（`(slotIndex + 1) % 3`）发生在取几何之前；几何失败也会前进。
3. 标定打分复用 `slots[0]`，且**两次 rectify 写同一个目标纹理**，中间夹一次 census：
   `rectify(W)→right, census(right)→censusL, rectify(U)→right, census(right)→censusR, match`。
4. 混合路径的 `defer` 解锁输入 pixel buffer；C++ 调用是同步的，不保留指针。
5. `StereoFrame.depthPointer` 指向共享 `MTLBuffer`，行跨度是**像素**。
6. 系统深度路径完全绕开几何 / 匹配 / 视差转深度。

## 7. 基线验证

改造前（`test` 分支，2026-11）：

```
cmake --build build/core --config Release
cd build/core && ctest -C Release
→ 7/7 passed（golden / c_api / geometry / analyze / cloud / calibrate / frame）
```

Swift / Metal 侧本机（Windows）没有工具链，真机回归清单见
`docs/engine-backend-refactor.md` §6（真机回归清单）；本轮只做代码结构的等价迁移，
kernel、C ABI、C++ 算法、pixel format、参数与计时口径均未改动。
