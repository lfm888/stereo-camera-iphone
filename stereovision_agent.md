# AGENT EXECUTION PLAN: `StereoVision/` 目录结构与命名规范化重构

> **给执行 Agent 的说明**：
> 你正在重构 `StereoVision/` 目录下的 Swift 代码结构。当前 `StereoVision/Engine/`（22 个文件，8 个子目录）与 `StereoVision/Bridge/` 已经完成模块化拆分，但 `StereoVision/` 根目录下仍平铺着 14 个 Swift 文件。
> 你的任务是：**在不修改任何算法逻辑、Shader 参数、统计口径和 C++ 核心代码的前提下**，将根目录下的 Swift 文件按架构职责归入子目录，并执行语义化重命名与契约收敛。

---

## 0. 绝对红线（执行全程必须遵守）

1. **禁止触碰的目录**：严禁修改 `core/`、`Shaders.metal` 以及 `StereoVision/Bridge/`（除非执行阶段 4 的可选桥接合并，否则保持只读）。
2. **禁止修改 `.xcodeproj`**：工程文件由 CI 的 `xcodegen generate --spec project.yml` 生成且未入库。不要创建或提交 `StereoVision.xcodeproj`。确保新增子目录能被 `project.yml` 和 `scripts/check_xcodeproj_sources.sh` 正确扫描。
3. **单向依赖红线**：
   - `StereoVision/UI/` 下**绝对禁止**出现 `import AVFoundation`、`StereoEngine` 或 `SVStereoCore`。
   - `StereoVision/Contracts/` 下**绝对禁止**出现 `import SwiftUI` 或 `import AVFoundation`。
   - `StereoVision/Engine/` 不得反向依赖 `UI/`、`App/`、`Camera/`、`Rendering/` 或 `Analysis/`。
4. **保持逻辑等价**：所有重命名与拆分仅涉及符号名、文件路径及访问控制（`internal`/`public`），不得更改运行时行为或性能表现。

---

## 1. 全量文件移动与重命名映射表

请严格按照下表执行文件创建、内容迁移、重命名和旧文件删除：

| 操作类型 | 原路径 | 目标路径 | 核心符号重命名 / 迁移说明 |
| :--- | :--- | :--- | :--- |
| **拆分** | `StereoVision/ContentView.swift` | `StereoVision/StereoVisionApp.swift` | 仅保留 `@main struct StereoVisionApp: App` 入口 |
| **拆分** | `StereoVision/ContentView.swift` | `StereoVision/UI/RootView.swift` | 将原 `ContentView` 视图重命名为 `RootView`，管理主界面与 `.sheet` |
| **拆分** | `StereoVision/Contracts.swift` | `StereoVision/Contracts/UIContracts.swift` | 迁移 UI 快照与意图：`CaptureUIState`, `DiagnosticsSnapshot`, `GpuReport`, `SettingsDraft`, `DeviceInfo`, `CaptureIntents`, `SettingsIntents`, `DisplayOrientation` |
| **合并拆分** | `StereoVision/Contracts.swift` + `Pipeline.swift` | `StereoVision/Contracts/PipelineContracts.swift` | 迁移管线数据结构：`ViewMode`, `Matcher`, `PipelineSettings`, `PipelineKind`, `StageTimings`, `FrameReport`, `FrameStore` |
| **移动** | `StereoVision/CaptureScreen.swift` | `StereoVision/UI/CaptureScreen.swift` | 保持类型名 `CaptureScreen`（及内部 `HUD`/`SidePanel`）不变 |
| **移动** | `StereoVision/OverlayView.swift` | `StereoVision/UI/OverlayView.swift` | 保持类型名 `OverlayView` 不变 |
| **移动** | `StereoVision/SettingsScreen.swift` | `StereoVision/UI/SettingsScreen.swift` | 保持类型名 `SettingsScreen` 不变 |
| **移动** | `StereoVision/DiagnosticsScreen.swift` | `StereoVision/UI/DiagnosticsScreen.swift` | 保持类型名 `DiagnosticsScreen` 不变 |
| **移动+重命名** | `StereoVision/CameraController.swift` | `StereoVision/Camera/CameraSessionController.swift` | 类名 `CameraController` → `CameraSessionController` |
| **移动+重命名** | `StereoVision/Calibration.swift` | `StereoVision/Camera/CameraCalibrationReader.swift` | 类型名 `CalibrationSource` → `CameraCalibrationReader` |
| **移动+重命名** | `StereoVision/Analyzer.swift` | `StereoVision/Analysis/DepthSceneAnalyzer.swift` | 类名 `Analyzer` → `DepthSceneAnalyzer` |
| **移动+重命名** | `StereoVision/Renderer.swift` | `StereoVision/Rendering/MetalSceneRenderer.swift` | 类名 `Renderer` → `MetalSceneRenderer` |
| **移动+重命名** | `StereoVision/Capture.swift` | `StereoVision/Rendering/MediaExportManager.swift` | 类名 `Capture` → `MediaExportManager` |
| **移动+重命名** | `StereoVision/Pipeline.swift` | `StereoVision/App/FramePipeline.swift` | 类名 `Pipeline` → `FramePipeline`（剥离数据结构后仅保留调度方法） |
| **移动** | `StereoVision/AppModel.swift` | `StereoVision/App/AppModel.swift` | 保持类名 `AppModel` 不变，更新内部引用的新类型名 |

> **保留不变的文件/目录**：
> - `StereoVision/Shaders.metal`
> - `StereoVision/Engine/**`（所有 22 个文件及其内部类型保持原样）
> - `StereoVision/Bridge/**`（所有 ObjC++ 桥接文件保持原样）

---

## 2. 分阶段执行指令

请按顺序执行以下 5 个阶段，**每完成一个阶段，在继续前先检查引用完整性**。

### Phase 1：建立目录与契约层 (`Contracts/`)
1. 创建目录：`StereoVision/Contracts`、`StereoVision/App`、`StereoVision/UI`、`StereoVision/Camera`、`StereoVision/Analysis`、`StereoVision/Rendering`。
2. 读取 `StereoVision/Contracts.swift` 和 `StereoVision/Pipeline.swift`。
3. 创建 `StereoVision/Contracts/UIContracts.swift`，写入所有与 SwiftUI 交互的值类型和协议（`CaptureUIState`、`DiagnosticsSnapshot`、`GpuReport`、`SettingsDraft`、`DeviceInfo`、`CaptureIntents`、`SettingsIntents`、`DisplayOrientation`）。
4. 创建 `StereoVision/Contracts/PipelineContracts.swift`，写入所有供引擎、分析、渲染共享的数据模型（`ViewMode`、`Matcher`、`PipelineSettings`、`PipelineKind`、`StageTimings`、`FrameReport`、`FrameStore`）。
5. 删除原 `StereoVision/Contracts.swift`。

### Phase 2：拆分入口与迁移界面层 (`UI/`)
1. 读取 `StereoVision/ContentView.swift`。
2. 创建 `StereoVision/StereoVisionApp.swift`，仅保留 `@main struct StereoVisionApp: App`，根视图指向 `RootView(model: model)`。
3. 创建 `StereoVision/UI/RootView.swift`，将原 `ContentView` 重命名为 `RootView`。
4. 删除原 `StereoVision/ContentView.swift`。
5. 将 `CaptureScreen.swift`、`OverlayView.swift`、`SettingsScreen.swift`、`DiagnosticsScreen.swift` 移动至 `StereoVision/UI/`。
6. 检查 `StereoVision/UI/` 下所有文件，确保所有引用 `Renderer` 的地方更新为 `MetalSceneRenderer`（如 `CaptureScreen` 的入参）。

### Phase 3：迁移并重命名硬件、分析与渲染层 (`Camera/`, `Analysis/`, `Rendering/`)
1. **`Camera/`**：
   - 将 `StereoVision/CameraController.swift` 迁移为 `StereoVision/Camera/CameraSessionController.swift`，全局替换类型名 `CameraController` → `CameraSessionController`。
   - 将 `StereoVision/Calibration.swift` 迁移为 `StereoVision/Camera/CameraCalibrationReader.swift`，全局替换类型名 `CalibrationSource` → `CameraCalibrationReader`（注意同步更新 `Engine/Input/FrameInputProvider.swift` 中的调用）。
2. **`Analysis/`**：
   - 将 `StereoVision/Analyzer.swift` 迁移为 `StereoVision/Analysis/DepthSceneAnalyzer.swift`，全局替换类型名 `Analyzer` → `DepthSceneAnalyzer`。
3. **`Rendering/`**：
   - 将 `StereoVision/Renderer.swift` 迁移为 `StereoVision/Rendering/MetalSceneRenderer.swift`，全局替换类型名 `Renderer` → `MetalSceneRenderer`。
   - 将 `StereoVision/Capture.swift` 迁移为 `StereoVision/Rendering/MediaExportManager.swift`，全局替换类型名 `Capture` → `MediaExportManager`。
4. 删除已迁移的旧文件。

### Phase 4：迁移应用编排层 (`App/`) 与构建脚本检查
1. 将原 `StereoVision/Pipeline.swift`（仅保留调度类逻辑）迁移为 `StereoVision/App/FramePipeline.swift`，全局替换类型名 `Pipeline` → `FramePipeline`（注意避免误替换 `MetalPipelines`、`PipelineSettings`、`PipelineKind`）。
2. 将 `StereoVision/AppModel.swift` 移动至 `StereoVision/App/AppModel.swift`，更新其内部对 `CameraSessionController`、`MetalSceneRenderer`、`MediaExportManager`、`FramePipeline` 的引用。
3. 删除原 `StereoVision/Pipeline.swift` 和 `StereoVision/AppModel.swift`。
4. 检查根目录 `project.yml` 和 `scripts/check_xcodeproj_sources.sh`：
   - 确认 `project.yml` 对 `StereoVision` 目录的配置是递归包含所有子目录 `.swift` 文件的；如果是硬编码文件列表，请同步更新路径。
   - 运行 `bash scripts/check_xcodeproj_sources.sh`（若脚本不依赖 macOS `xcodebuild` 则直接验证）。

### Phase 5：同步更新架构文档
更新仓库中的以下文档，使其文件路径和类名与最新目录树 100% 一致：
1. `ARCHITECTURE.md`：更新 §0 架构图、§3 Swift 前端文件表、§4 三条调用链中的路径与类名。
2. `docs/engine-backend-refactor.md`：在文档末尾追加本次根目录文件归档与重命名的映射记录。

---

## 3. 自动化验收脚本（Agent 完成操作后必须运行）

请在终端运行以下命令，**所有命令的检查结果均需符合预期**，否则必须修复后再结束任务：

```bash
# 1. 验证 StereoVision 根目录下是否只剩下 StereoVisionApp.swift 和 Shaders.metal（不应有其他 .swift 文件）
find StereoVision -maxdepth 1 -name "*.swift"
# 预期输出仅有一行: StereoVision/StereoVisionApp.swift

# 2. 验证 UI/ 目录下没有泄露底层硬件或引擎依赖（期望输出为空）
grep -rnE "import (AVFoundation|MetalKit)|StereoEngine|SVStereoCore" StereoVision/UI/

# 3. 验证 Contracts/ 目录下没有泄露 UI 或硬件依赖（期望输出为空）
grep -rnE "import (SwiftUI|AVFoundation)" StereoVision/Contracts/

# 4. 检查是否还有残留的旧类名引用（期望输出为空，注意排除注释或文档中的说明）
grep -rnE "\b(CameraController|CalibrationSource|Analyzer|Renderer)\b" StereoVision/

# 5. 运行 C++ 核心测试套件，确保零回归
cmake -S core -B build/core -DCMAKE_BUILD_TYPE=Release && cmake --build build/core --config Release
cd build/core && ctest -C Release
```

---

## 4. 完成汇报格式要求

任务完成后，请向用户输出简明总结，包含：
1. 最终 `StereoVision/` 目录树（`find StereoVision -type f | sort`）。
2. 上述 5 条自动化验收命令的执行结果摘要。
3. 提醒用户在 macOS CI Runner 上验证 `xcodegen generate` 与 `xcodebuild` 编译。