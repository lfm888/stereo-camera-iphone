import AVFoundation
import CoreGraphics
import CoreVideo
import QuartzCore
import os

// 每帧在相机数据队列上跑的管线（不含界面、不含 @Observable）。
//
//   相机回调 → Pipeline.stereo / depthData → StereoEngine 出一帧 → finish()
//   （引擎的后端组合由 PipelineSettings.stereoMode 决定，流程只有一条）
//            → C++ 深度分析 → publish(FrameReport) → AppModel.apply（主线程）
//
// 原来和 AppModel 写在同一个文件里。拆出来的好处：
//   - 这里的代码跑在后台队列，AppModel 里的代码跑在主线程，一个文件不再混着两种线程语义
//   - Pipeline 不再依赖任何界面文件的类型（以前它要 import DiagnosticsScreen 里的 StageDiag）

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

final class Pipeline: @unchecked Sendable {
    let engine: StereoEngine
    let store = FrameStore()
    let settings = OSAllocatedUnfairLock(initialState: PipelineSettings())
    var publish: ((FrameReport) -> Void)?
    private let analyzer = Analyzer()
    private var stamps: [Double] = []

    init(engine: StereoEngine) { self.engine = engine }

    func stereo(_ wide: CMSampleBuffer, _ ultra: CMSampleBuffer) {
        var s = settings.withLock { st in
            defer { st.calibrate = false }
            return st
        }
        var cal: (flip: Bool, dy: Float)?
        if s.calibrate {
            let c = engine.calibrate(wide, ultra, s)
            s.flip = c.flip
            s.yOffset = c.dy
            settings.withLock { $0.flip = c.flip; $0.yOffset = c.dy }
            cal = c
        }
        guard let f = engine.processStereoFrame(wide, ultra, s, mode: s.stereoMode) else { return }
        finish(f, cal, s.cpuMatcher ? .cpuStereo : .metalStereo)
    }

    func depthData(_ video: CMSampleBuffer, _ depth: AVDepthData) {
        let s = settings.withLock { $0 }
        guard let f = engine.depthData(video, depth, s) else { return }
        finish(f, nil, .systemDepth)
    }

    /// 从一帧里取出各阶段的耗时。只拷数字；文案在 DiagnosticsScreen.swift。
    private static func timings(of f: StereoFrame) -> StageTimings {
        StageTimings(gpuMs: f.gpuMs, cpuMs: f.cpuMs,
                     gpuRectifyMs: f.gpuRectifyMs, readbackMs: f.readbackMs,
                     matchMs: f.matchMs, depthMs: f.depthMs)
    }

    private func finish(_ f: StereoFrame, _ cal: (flip: Bool, dy: Float)?, _ kind: PipelineKind) {
        store.latest = f
        let now = CACurrentMediaTime()
        stamps.append(now)
        stamps.removeAll { now - $0 > 1 }
        // 深度连通域分析在 C++（core/src/analyze.cpp），这里只拿结果。
        let a = analyzer.analyze(f, using: engine.core)
        publish?(FrameReport(analysis: a, fps: Double(stamps.count), kind: kind,
                             timings: Self.timings(of: f),
                             baselineMM: f.baselineMM, baselineDeg: f.baselineDeg,
                             hasExtrinsics: engine.hasExtrinsics, K: f.K,
                             depthSize: CGSize(width: f.width, height: f.height),
                             aspect: CGFloat(f.color.width) / CGFloat(f.color.height), calibration: cal))
    }
}
