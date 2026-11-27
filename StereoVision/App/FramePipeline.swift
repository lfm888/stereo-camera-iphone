//
//  App/FramePipeline.swift
//  StereoVision
//
//  每帧在相机数据队列上跑的**调度**类（不含界面、不含 @Observable、不含数据结构定义）。
//
//   相机回调 → FramePipeline.stereo / depthData → StereoEngine 出一帧 → finish()
//            （引擎的后端组合由 PipelineSettings.stereoMode 决定，流程只有一条）
//            → C++ 深度分析 → publish(FrameReport) → AppModel.apply（主线程）
//
//  数据结构（PipelineSettings / PipelineKind / StageTimings / FrameReport / FrameStore）
//  在 Contracts/PipelineContracts.swift：它们要跨层共享，放在调度类所在文件里会让
//  "引擎只依赖数据结构"这件事看不出来。
//
//  线程：本文件的代码只在 CameraSessionController 的 dataQueue 上跑；
//  AppModel 的代码只在主线程跑。一个文件里不再混着两种线程语义。
//

import AVFoundation
import CoreGraphics
import CoreVideo
import QuartzCore
import os

// MARK: - 管线

final class FramePipeline: @unchecked Sendable {
    let engine: StereoEngine
    let store = FrameStore()
    let settings = OSAllocatedUnfairLock(initialState: PipelineSettings())
    var publish: ((FrameReport) -> Void)?
    private let analyzer = DepthSceneAnalyzer()
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

    /// 从一帧里取出各阶段的耗时。只拷数字；文案在 UI/DiagnosticsScreen.swift。
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
