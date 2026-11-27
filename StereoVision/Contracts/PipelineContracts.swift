//
//  Contracts/PipelineContracts.swift
//  StereoVision
//
//  引擎 / 分析 / 渲染共享的数据模型（**只有值类型与一个线程安全容器**）。
//
//  这一层的作用是切断"引擎 ↔ 界面"的直接依赖：
//    - 引擎只认 `PipelineSettings` / `StereoGeometry` / `StereoFrame`；
//    - 后台管线只交出 `StageTimings` / `FrameReport`；
//    - 界面只读 `CaptureUIState`（在 UIContracts.swift）与 `PipelineKind`。
//
//  红线（见 stereovision_agent.md §0）：本目录不允许出现 SwiftUI 或
//  AVFoundation 的 import —— 这里只放能在任何平台编译的数据定义。
//
//  共享词汇（ViewMode / Matcher）也在这里：它们同时被设置页、诊断页、引擎参数使用，
//  但都不依赖任何 UI 框架。
//

import CoreGraphics
import Foundation

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
        case .pointCloud: 7   // 不走 displayFragment，MetalSceneRenderer 里单独分支
        }
    }
}

enum Matcher: String, CaseIterable, Identifiable, Sendable {
    case sgm, block
    var id: Self { self }
    var title: String { self == .sgm ? "SGM 4 paths" : "Block matching 5×5" }
}

// MARK: - 管线参数

struct PipelineSettings: Sendable {
    var matcher: Matcher
    var lrCheck: Bool
    var p1: Int
    var p2: Int
    var minZ: Float
    var maxZ: Float
    var uniqueness: Float
    var maxDisparity: Int
    var yOffset: Float
    var flip: Bool
    var orient = DisplayOrientation.landscapeRight
    var calibrate = true
    /// true = 匹配交给 C++ 核心（CPU）；false = 走 Metal 的 SGM。
    /// 由 AppModel 依据 DepthSource 推入，不放进设置页的 picker。
    var cpuMatcher = false

    /// 引擎参数的初值来自 **C++ 的参数表**（core/src/params.cpp）：
    /// 以前这里、`SettingsDraft`、`stereo_config_default` 三处各写一份，容易走样。
    init() {
        let d = SettingsDraft()
        matcher = d.matcher
        lrCheck = d.lrCheck
        p1 = Int(d.p1)
        p2 = Int(d.p2)
        minZ = d.minZ
        maxZ = d.maxZ
        uniqueness = d.uniqueness
        maxDisparity = d.maxDisparity
        yOffset = d.yOffset
        flip = d.flip
    }
}

// MARK: - 管线输出

/// 这一帧走的是哪条管线（决定诊断页上那张表）。
enum PipelineKind: Sendable {
    case metalStereo
    case cpuStereo
    case systemDepth
}

/// 一帧里各阶段的耗时（毫秒）。
///
/// 只放数字。阶段叫什么、跑在 GPU 还是 CPU、备注文案，由诊断页按 `PipelineKind` 决定。
/// 可选项 = 这个阶段只在某些管线里才有计时。
struct StageTimings: Sendable {
    var gpuMs: Double = 0
    /// CPU 一段的总和（读回 + 匹配 + 深度）；Metal 路径为 0。
    var cpuMs: Double = 0
    var gpuRectifyMs: Double? = nil
    var readbackMs: Double? = nil
    var matchMs: Double? = nil
    var depthMs: Double? = nil
}

struct FrameReport: Sendable {
    var analysis: Analysis
    var fps: Double
    var kind: PipelineKind
    var timings: StageTimings
    var baselineMM: Float
    var baselineDeg: Float
    var hasExtrinsics: Bool
    var K: Intrinsics
    var depthSize: CGSize
    var aspect: CGFloat
    var calibration: (flip: Bool, dy: Float)?
}

// MARK: - 帧缓存与管线

final class FrameStore: @unchecked Sendable {
    private let lock = NSLock()
    private var frame: StereoFrame?
    var latest: StereoFrame? {
        get { lock.withLock { frame } }
        set { lock.withLock { frame = newValue } }
    }
}
