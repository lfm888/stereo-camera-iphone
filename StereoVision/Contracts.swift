import CoreGraphics
import Foundation

// 本文件只放"类型定义"，不含任何逻辑：
//   1. 共享词汇        ViewMode / Matcher / DisplayOrientation
//   2. 界面 ↔ 状态契约  SettingsDraft / DeviceInfo / CaptureUIState / CaptureIntents / SettingsIntents
//   3. 诊断快照        GpuReport / DiagnosticsSnapshot
//
// 原来这些都挤在 AppModel.swift 里。拆出来之后：
//   - AppModel.swift  只管"状态聚合 + 接线 + 意图实现"
//   - Pipeline.swift  只管"每帧在后台怎么跑"
//   - 本文件          是两边与界面之间的共同语言
//
// 三个界面只认这几样东西：
//   CaptureUIState / DiagnosticsSnapshot / DeviceInfo  —— 只读入参（值类型，界面改不了状态）
//   CaptureIntents / SettingsIntents                    —— 唯一可写入口
// 于是"诊断页不会偷改状态"是编译器保证的，不靠约定。

// MARK: - 共享词汇

enum ViewMode: Int, CaseIterable, Identifiable {
    case standard, depth, overlay, disparity, stereoPair, anaglyph, segmentation, pointCloud
    var id: Self { self }

    var title: String {
        switch self {
        case .standard: "Standard"
        case .depth: "Depth map"
        case .overlay: "Overlay"
        case .disparity: "Disparity"
        case .stereoPair: "Rectified pair"
        case .anaglyph: "Anaglyph"
        case .segmentation: "3D segmentation"
        case .pointCloud: "Point cloud"
        }
    }

    var icon: String {
        switch self {
        case .standard: "camera"
        case .depth: "flame"
        case .overlay: "square.2.layers.3d"
        case .disparity: "circle.lefthalf.filled"
        case .stereoPair: "rectangle.split.2x1"
        case .anaglyph: "eyeglasses"
        case .segmentation: "square.grid.3x3.fill"
        case .pointCloud: "cube.transparent"
        }
    }

    var needsStereoPair: Bool { self == .stereoPair || self == .anaglyph }

    /// 显式映射，不用 rawValue：删过枚举项之后 rawValue 会前移，而着色器里的
    /// mode 是固定编号（Shaders.metal 的 displayFragment）。之前用
    /// `min(rawValue, 7)` 靠"Bird's-eye 恰好是 7"凑数，删掉它就错位了。
    var shaderIndex: Int32 {
        switch self {
        case .standard: 0
        case .depth: 1
        case .overlay: 2
        case .disparity: 3
        case .stereoPair: 4
        case .anaglyph: 5
        case .segmentation: 6
        case .pointCloud: 7   // 不走 displayFragment，Renderer 里单独分支
        }
    }
}

enum Matcher: String, CaseIterable, Identifiable, Sendable {
    case sgm, block
    var id: Self { self }
    var title: String { self == .sgm ? "SGM 4 paths" : "Block matching 5×5" }
}

/// 显示方向：手机怎么拿，屏幕上就把画面转多少。
///
/// 采集和匹配永远跑在传感器的横屏坐标系里（buffer 640×360），所以转屏不会重开 session、
/// 不丢帧、不用重新标定，深度/掩码/外参也都不变；只有“怎么显示”在变：
///   0 landscapeRight     buffer 原样贴屏（原来的唯一姿势）
///   1 portrait           内容顺时针转 90°
///   2 landscapeLeft      180°
///   3 portraitUpsideDown 270°
enum DisplayOrientation: Int, Sendable, CaseIterable {
    case landscapeRight = 0, portrait = 1, landscapeLeft = 2, portraitUpsideDown = 3

    var isPortrait: Bool { self == .portrait || self == .portraitUpsideDown }

    var key: String {
        switch self {
        case .landscapeRight: "landscape_right"
        case .portrait: "portrait"
        case .landscapeLeft: "landscape_left"
        case .portraitUpsideDown: "portrait_upside_down"
        }
    }

    var label: String {
        switch self {
        case .landscapeRight: "landscape →"
        case .portrait: "portrait"
        case .landscapeLeft: "← landscape"
        case .portraitUpsideDown: "portrait ⇅"
        }
    }

    /// 着色器里要把屏幕 uv 换成 buffer uv，这跟这里的 screen/buffer 是同一对互逆变换。
    func screen(_ p: CGPoint) -> CGPoint {
        switch self {
        case .landscapeRight: CGPoint(x: p.x, y: p.y)
        case .portrait: CGPoint(x: 1 - p.y, y: p.x)
        case .landscapeLeft: CGPoint(x: 1 - p.x, y: 1 - p.y)
        case .portraitUpsideDown: CGPoint(x: p.y, y: 1 - p.x)
        }
    }

    func buffer(_ p: CGPoint) -> CGPoint {
        switch self {
        case .landscapeRight: CGPoint(x: p.x, y: p.y)
        case .portrait: CGPoint(x: p.y, y: 1 - p.x)
        case .landscapeLeft: CGPoint(x: 1 - p.x, y: 1 - p.y)
        case .portraitUpsideDown: CGPoint(x: 1 - p.y, y: p.x)
        }
    }

    func screen(_ r: CGRect) -> CGRect {
        let a = screen(CGPoint(x: r.minX, y: r.minY)), b = screen(CGPoint(x: r.maxX, y: r.maxY))
        return CGRect(x: min(a.x, b.x), y: min(a.y, b.y), width: abs(a.x - b.x), height: abs(a.y - b.y))
    }

    func buffer(_ r: CGRect) -> CGRect {
        let a = buffer(CGPoint(x: r.minX, y: r.minY)), b = buffer(CGPoint(x: r.maxX, y: r.maxY))
        return CGRect(x: min(a.x, b.x), y: min(a.y, b.y), width: abs(a.x - b.x), height: abs(a.y - b.y))
    }
}

// MARK: - 界面与状态之间的契约

/// 设置页编辑的**引擎参数草稿**。
///
/// 改动在点 Apply 之前不进管线：换 maxDisparity 会让 C++ context 重新分配缓冲区，
/// 所以不能拖一下滑杆就生效。
struct SettingsDraft {
    var minZ: Float = 0.12
    var maxZ: Float = 3
    var uniqueness: Float = 0.1
    var maxDisparity = 64
    var matcher = Matcher.sgm
    var lrCheck = true
    var p1: Float = 8
    var p2: Float = 96
    var yOffset: Float = 0
    var flip = false
}

/// 只读的设备/标定信息。
struct DeviceInfo {
    var source: DepthSource
    var isMultiCam: Bool
    var baselineMM: Float
    var baselineDeg: Float
    var intrinsics: Intrinsics
    var depthSize: CGSize
}

/// 主界面的只读状态。
struct CaptureUIState {
    var mode: ViewMode
    var source: DepthSource
    var aspect: CGFloat
    var orientation: DisplayOrientation
    var fps: Double
    var device: DeviceInfo
    var hasExtrinsics: Bool
    var calibrating: Bool
    var status: String?
    var draft: SettingsDraft
    var overlayAlpha: Float
    var yaw: Float
    var pitch: Float
    var orbit: Float
    var gpuSummary: String
    var analysis: Analysis
    var tap: CGPoint?
    var tapDistance: Float?
    var isRecording: Bool
    var recordingStart: Date?
    var captureMessage: String?
}

/// 主拍摄界面能做的事。
@MainActor
protocol CaptureIntents {
    func startCapture() async
    func selectMode(_ mode: ViewMode)
    func selectNextSource()
    func autoCalibrate()
    func takePhoto()
    func toggleRecording()
    func measureTap(at normalized: CGPoint)
    func setOrbit(yaw: Float, pitch: Float)
    func setOrbitDistance(_ orbit: Float)
    func resetOrbit()
}

/// 设置界面能做的事。
@MainActor
protocol SettingsIntents {
    var draft: SettingsDraft { get }
    var device: DeviceInfo { get }
    var overlayAlpha: Float { get }
    func apply(_ draft: SettingsDraft)
    func setOverlayAlpha(_ value: Float)
    func setSource(_ source: DepthSource)
    func autoCalibrate()
}

// MARK: - 诊断

/// 最近一帧的诊断数据：走的哪条管线、各阶段耗时、显存占用。
///
/// 只存**数字和枚举**，不存文案。"某条管线每个阶段叫什么、跑在哪、备注是什么"
/// 是显示问题，由 DiagnosticsScreen.swift 里的 `PipelineKind.rows(_:)` 负责。
struct GpuReport: Sendable {
    var kind: PipelineKind? = nil
    var timings = StageTimings()
    var allocatedMB: Double = 0
    var workingSetMB: Double = 0

    // 保留旧名字，读 gpuReport.gpuMs / cpuMs 的地方不用改。
    var gpuMs: Double { timings.gpuMs }
    var cpuMs: Double { timings.cpuMs }
}

/// 诊断页的**只读快照**。
///
/// 诊断页只需要这些数字。给它一个值类型，而不是 AppModel —— 这样它在编译期就写不了状态，
/// "诊断界面只显示信息"这件事不靠自觉。
struct DiagnosticsSnapshot {
    var sourceTitle: String = "—"
    var kind: PipelineKind? = nil
    var fps: Double = 0
    var timings = StageTimings()
    var allocatedMB: Double = 0
    var workingSetMB: Double = 0
    var depthSize: CGSize = CGSize(width: 1, height: 1)
}
