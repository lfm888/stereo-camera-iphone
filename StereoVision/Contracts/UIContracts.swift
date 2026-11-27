//
//  Contracts/UIContracts.swift
//  StereoVision
//
//  界面与状态之间**唯一**的共同语言（只读快照 + 意图协议）。
//
//    CaptureUIState / DiagnosticsSnapshot / DeviceInfo  —— 只读入参（值类型，界面改不了状态）
//    CaptureIntents / SettingsIntents                    —— 唯一可写入口
//  于是"诊断页不会偷改状态"是编译器保证的，不靠约定。
//
//  红线（见 stereovision_agent.md §0）：本目录不允许出现 SwiftUI / AVFoundation 的 import。
//  界面用到的枚举（ViewMode / Matcher / PipelineKind）在 PipelineContracts.swift。
//
//  三个界面只依赖 SwiftUI，拿到的都是这里的值类型：
//    UI/CaptureScreen.swift · UI/SettingsScreen.swift · UI/DiagnosticsScreen.swift
//

import CoreGraphics
import Foundation

// MARK: - 显示方向

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
    var minZ: Float
    var maxZ: Float
    var uniqueness: Float
    var maxDisparity: Int
    var matcher: Matcher
    var lrCheck: Bool
    var p1: Float
    var p2: Float
    var yOffset: Float
    var flip: Bool

    /// 默认值来自 **C++ 的参数表**（core/src/params.cpp → SVParameterTable），
    /// Swift 侧不再各写一份 —— 以前 `PipelineSettings`、这里、`stereo_config_default`
    /// 三处各有一份默认值。
    init() {
        func d(_ key: String) -> Float { SVParameterTable.lookup(key)?.defaultValue ?? 0 }
        func b(_ key: String) -> Bool { d(key) != 0 }
        minZ = d("min_z")
        maxZ = d("max_z")
        uniqueness = d("uniqueness")
        maxDisparity = Int(d("max_disparity"))
        let m = Int(d("matcher"))
        matcher = Matcher.allCases.indices.contains(m) ? Matcher.allCases[m] : .sgm
        lrCheck = b("lr_check")
        p1 = d("p1")
        p2 = d("p2")
        yOffset = d("y_offset")
        flip = b("flip_baseline")
    }
}

extension SettingsDraft {
    /// 按 C++ 参数表的 key 读写。
    ///
    /// 设置界面用它把"表里的一条参数"直接接到控件上，于是**取值范围与步长只有表里一份**；
    /// 枚举按 `Matcher.allCases` 的下标存取（顺序与表里的 enum_labels 一致）。
    subscript(key key: String) -> Float? {
        get {
            switch key {
            case "min_z": return minZ
            case "max_z": return maxZ
            case "uniqueness": return uniqueness
            case "max_disparity": return Float(maxDisparity)
            case "matcher": return Float(Matcher.allCases.firstIndex(of: matcher) ?? 0)
            case "lr_check": return lrCheck ? 1 : 0
            case "p1": return p1
            case "p2": return p2
            case "y_offset": return yOffset
            case "flip_baseline": return flip ? 1 : 0
            default: return nil
            }
        }
        set {
            guard let v = newValue else { return }
            switch key {
            case "min_z": minZ = v
            case "max_z": maxZ = v
            case "uniqueness": uniqueness = v
            case "max_disparity": maxDisparity = Int(v)
            case "matcher":
                let i = Int(v)
                matcher = Matcher.allCases.indices.contains(i) ? Matcher.allCases[i] : matcher
            case "lr_check": lrCheck = v != 0
            case "p1": p1 = v
            case "p2": p2 = v
            case "y_offset": yOffset = v
            case "flip_baseline": flip = v != 0
            default: break
            }
        }
    }
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
