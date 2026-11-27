//
//  StereoEngine.swift
//  StereoVision
//
//  **唯一的流程编排者**。
//
//      CMSampleBuffer(左右)
//          → FrameInputProvider      纹理 + 每帧内参 + 已配置的双摄参数
//          → StereoGeometrySolver    SVRectification.solve → core/src/geometry.cpp
//          → RectificationBackend    MetalRectifier
//          → MatchingBackend         MetalSGMMatcher / MetalBlockMatcher / CppSGMMatcher
//          → DepthBackend            MetalDepthProcessor / CppDepthProcessor
//          → StereoFrame             → Pipeline → Analyzer → FrameReport
//
//  这个文件里**没有** kernel 调度细节（在各 backend）、**没有** C++ 指针细节
//  （在 CppSGMMatcher / CppDepthProcessor / CPUImageReadback）、也没有 PSO/纹理的创建
//  （在 Metal/MetalPipelines.swift、Metal/MetalFrameResources.swift）。
//
//  两条产品路径只是后端组合不同（StereoMode），流程本身只有这一份；
//  系统深度（AVDepthData）走 SystemDepthProcessor，不属于双目后端。
//  全 CPU 编排在 C++ 的 `stereo_process_frame`，与本文件无关，也不在此调用。
//

import AVFoundation
import CoreVideo
import Metal
import QuartzCore

final class StereoEngine: @unchecked Sendable {
    /// 工作分辨率 / 最大视差的别名（真正的定义在 StereoTypes.swift 的 StereoWork）。
    static let workW = StereoWork.width
    static let workH = StereoWork.height
    static let maxD = StereoWork.maxDisparity

    private let context: MetalContext
    private let resources: MetalFrameResources
    private let inputProvider: FrameInputProvider
    private let geometrySolver: StereoGeometrySolver
    private let rectifier: RectificationBackend
    private let matchers: [Matcher: MatchingBackend]
    private let metalDepth: MetalDepthBackend
    private let calibrator: StereoCalibrator
    private let systemDepth: SystemDepthProcessor
    private let cppMatcher: CppSGMMatcher?
    private let cppDepth: CppDepthProcessor?

    /// C++ 核心上下文：CPU 匹配、视差→深度、深度回转、点选测距、连通域分析共用它。
    /// 不是 lazy —— 即使走 Metal 路径，准星读数与面块分析也要用它。
    /// 为 nil 表示 stereo_create 失败（只会因为内存不足），此时只剩 Metal 路径能跑。
    let core: SVStereoCore?

    var device: MTLDevice { context.device }
    var hasExtrinsics: Bool { inputProvider.hasExtrinsics }

    init(device: MTLDevice) {
        let ctx = MetalContext(device: device)
        let pipelines = MetalPipelines(device: device)
        let res = MetalFrameResources(device: device)
        let provider = FrameInputProvider(context: ctx)
        let solver = StereoGeometrySolver()
        let rect = MetalRectifier(pipelines: pipelines)
        let sgm = MetalSGMMatcher(context: ctx, pipelines: pipelines)
        let block = MetalBlockMatcher(pipelines: pipelines)
        let cal = StereoCalibrator(context: ctx, resources: res, geometry: solver,
                                   rectifier: rect, matcher: block)
        // 第 3/4 步的 C++ context（兼做点选测距与连通域分析的公用上下文）。
        let c = SVStereoCore(maxDisparity: 64, p1: 8, p2: 96, uniqueness: 0.1, lrCheck: true)

        context = ctx
        resources = res
        inputProvider = provider
        geometrySolver = solver
        rectifier = rect
        matchers = [.sgm: sgm, .block: block]
        metalDepth = MetalDepthProcessor(pipelines: pipelines)
        calibrator = cal
        systemDepth = SystemDepthProcessor(context: ctx, resources: res)
        core = c
        cppMatcher = c.map { CppSGMMatcher(core: $0) }
        cppDepth = c.map { CppDepthProcessor(core: $0) }
    }

    /// 相机会话建好后由 `CameraController.onDevices` 调用。
    func configure(wide: AVCaptureDevice, ultra: AVCaptureDevice) {
        inputProvider.configure(wide: wide, ultra: ultra)
    }

    // MARK: - 双目流程（唯一入口）

    /// 按后端组合跑一帧双目。
    ///
    /// - Parameters:
    ///   - mode: `.metal`（全 GPU，一帧一条 command buffer）或 `.cppHybrid`
    ///     （Metal 矫正 + 读回 + C++ 匹配 + C++ 深度）。
    func processStereoFrame(_ wide: CMSampleBuffer, _ ultra: CMSampleBuffer,
                            _ settings: PipelineSettings, mode: StereoMode) -> StereoFrame? {
        switch mode {
        case .metal: return processMetal(wide, ultra, settings)
        case .cppHybrid: return processCppHybrid(wide, ultra, settings)
        }
    }

    /// Metal 双目：矫正 → census×2 → 匹配 → toDepth → unrectifyDepth，**一条** command buffer。
    private func processMetal(_ wide: CMSampleBuffer, _ ultra: CMSampleBuffer,
                              _ s: PipelineSettings) -> StereoFrame? {
        guard let p = prepare(wide, ultra, s), let matcher = matchers[s.matcher],
              let cb = context.queue.makeCommandBuffer(),
              let enc = cb.makeComputeCommandEncoder() else { return nil }

        encodeRectification(enc, p)
        matcher.encodeCensus(into: enc, source: p.leftTexture, destination: p.slot.censusL)
        matcher.encodeCensus(into: enc, source: p.slot.right, destination: p.slot.censusR)
        matcher.encodeMatch(into: enc, input: p.slot.matchInput(left: p.leftTexture),
                            geometry: p.geometry, settings: s)
        metalDepth.encode(into: enc, disparity: p.slot.disp, rectifiedDepth: p.slot.depthRolled,
                          target: p.slot.target, geometry: p.geometry, settings: s)
        enc.endEncoding()
        cb.commit()
        cb.waitUntilCompleted()
        let ms = (cb.gpuEndTime - cb.gpuStartTime) * 1000
        return makeFrame(p, gpuMs: ms, retained: p.inputs.keep + [p.leftCVTexture, wide])
    }

    /// C++ 混合：Metal 矫正（一个同步点）→ 读回 + C++ 匹配 → C++ 深度（全部同步）。
    ///
    /// 计时口径与改造前一致：readbackMs 含「锁定 + 两次临时分配 + getBytes」，
    /// depthMs 含「临时缓冲分配 + toDepth + unrectify」。
    private func processCppHybrid(_ wide: CMSampleBuffer, _ ultra: CMSampleBuffer,
                                  _ s: PipelineSettings) -> StereoFrame? {
        guard let matcher = cppMatcher, let depth = cppDepth else { return nil }
        // 参数不合法时先失败，不浪费一块 pixel buffer（与改造前一致）。
        guard matcher.configure(s) else { return nil }
        guard let p = prepare(wide, ultra, s) else { return nil }

        // ---- 1) 矫正：GPU ----
        guard let cb = context.queue.makeCommandBuffer(),
              let enc = cb.makeComputeCommandEncoder() else { return nil }
        encodeRectification(enc, p)
        enc.endEncoding()
        cb.commit()
        cb.waitUntilCompleted()
        let rectifyMs = (cb.gpuEndTime - cb.gpuStartTime) * 1000

        // ---- 2) 读回像素 + C++ 匹配：CPU ----
        let readbackT0 = CACurrentMediaTime()
        guard let image = CPUImageReadback.make(pixelBuffer: p.leftBuffer, texture: p.slot.right,
                                                width: StereoWork.width, height: StereoWork.height) else { return nil }
        let readbackMs = (CACurrentMediaTime() - readbackT0) * 1000

        let matchT0 = CACurrentMediaTime()
        let matched = matcher.match(image, direction: p.geometry.dir)
        let matchMs = (CACurrentMediaTime() - matchT0) * 1000
        guard matched else { return nil }

        // ---- 3) 视差 → 深度 → 回转：全部在 C++ ----
        let depthT0 = CACurrentMediaTime()
        let ok = depth.makeDepth(disparity: image.disparity, disparityStride: image.width,
                                 width: image.width, height: image.height,
                                 geometry: p.geometry, settings: s, target: p.slot.target)
        let depthMs = (CACurrentMediaTime() - depthT0) * 1000
        guard ok else { return nil }

        return makeFrame(p, gpuMs: rectifyMs, gpuRectifyMs: rectifyMs,
                         readbackMs: readbackMs, matchMs: matchMs, depthMs: depthMs,
                         retained: p.inputs.keep + [p.leftCVTexture, wide])
    }

    // MARK: - 两条路径共用的部分

    /// 一帧的中间状态：输入、左图输出缓冲、轮换到的资源、几何。
    private struct Prepared {
        let inputs: FrameInputs
        let leftBuffer: CVPixelBuffer
        let leftTexture: MTLTexture
        let leftCVTexture: CVMetalTexture
        let slot: MetalSlot
        let geometry: StereoGeometry
    }

    /// 输入 → 左图输出缓冲 → slot → 几何。
    ///
    /// 顺序与改造前一致：先在 pool 里取一块左图缓冲，再轮换 slot，最后解几何
    /// （几何失败时这几样自然释放；slotIndex 已经前进过，也和以前一样）。
    private func prepare(_ wide: CMSampleBuffer, _ ultra: CMSampleBuffer, _ s: PipelineSettings) -> Prepared? {
        guard let inp = inputProvider.inputs(wide, ultra) else { return nil }
        guard let lpb = resources.makeRectifiedLeftBuffer(),
              let (lTex, lcv) = context.texture(lpb, writable: true) else { return nil }
        let slot = resources.nextSlot()
        guard let g = geometrySolver.solve(rig: inp.rig,
                                           wide: inp.wideIntrinsics, ultra: inp.ultraIntrinsics,
                                           sourceWidth: Float(inp.wide.width),
                                           sourceHeight: Float(inp.wide.height),
                                           flip: s.flip, yOffset: s.yOffset) else { return nil }
        return Prepared(inputs: inp, leftBuffer: lpb, leftTexture: lTex, leftCVTexture: lcv,
                        slot: slot, geometry: g)
    }

    /// 两路矫正（宽 → 左图缓冲，超广角 → slot.right）。两条路径的编码顺序相同。
    private func encodeRectification(_ enc: MTLComputeCommandEncoder, _ p: Prepared) {
        rectifier.encode(into: enc, source: p.inputs.wide,
                         destination: p.leftTexture, transform: p.geometry.MW)
        rectifier.encode(into: enc, source: p.inputs.ultra,
                         destination: p.slot.right, transform: p.geometry.MU)
    }

    /// 组帧。两条路径的字段完全一致，只有计时字段不同。
    ///
    /// `retained` 由调用方给出：CVMetalTexture 与宽相机 CMSampleBuffer 必须和这一帧同寿，
    /// 否则底层的 CVPixelBuffer 被回收后纹理立刻失效。
    private func makeFrame(_ p: Prepared, gpuMs: Double, gpuRectifyMs: Double = 0,
                           readbackMs: Double = 0, matchMs: Double = 0, depthMs: Double = 0,
                           retained: [AnyObject]) -> StereoFrame {
        let g = p.geometry
        return StereoFrame(color: p.inputs.wide, pairLeft: p.leftTexture, right: p.slot.right,
                           target: p.slot.target, K: g.K, image: p.inputs.wideBuffer,
                           gpuMs: gpuMs, baselineMM: g.baselineMM, baselineDeg: g.baselineDeg,
                           pairLeftPB: p.leftBuffer, dir: g.dir,
                           readbackMs: readbackMs, matchMs: matchMs, depthMs: depthMs,
                           gpuRectifyMs: gpuRectifyMs, retained: retained)
    }

    // MARK: - 标定与系统深度（不同数据来源，各有独立入口）

    /// 第 1 步的自动化：找让两路行对齐最好的 (flip, dy)。
    /// 搜索在 C++、打分在 GPU，细节见 StereoCalibrator。
    func calibrate(_ wide: CMSampleBuffer, _ ultra: CMSampleBuffer,
                   _ s: PipelineSettings) -> (flip: Bool, dy: Float) {
        guard let inp = inputProvider.inputs(wide, ultra) else { return (s.flip, s.yOffset) }
        return calibrator.calibrate(inp, s)
    }

    /// iPhone 系统深度路径（不是 SGM 后端，不经过几何/匹配/视差转深度）。
    func depthData(_ video: CMSampleBuffer, _ depthData: AVDepthData,
                   _ s: PipelineSettings) -> StereoFrame? {
        systemDepth.process(video, depthData, s, fovDeg: inputProvider.wideFieldOfView)
    }
}
