//
//  StereoCalibrator.swift
//  StereoVision
//
//  第 1 步的自动化：找让两路行对齐得最好的 (flip, dy)。
//
//  **搜索策略在 C++**（core/src/calibrate.cpp：2×25 个候选 + 4 次细化 + 取最小），
//  **打分留在 GPU**（下面 `score`：rectify + census + match 三个 kernel）。
//  这样分工的理由很实际：CPU 上算一遍打分要 60~80 ms，54 次就是 4 秒；
//  而"该试哪些候选、怎么细化、怎么比较"才是算法，那部分在 C++ 里。
//
//  打分固定用 `resources.scoringSlot`（slots[0]），与改造前一致。
//  标定和正常帧处理都在 CameraController 的 dataQueue 上串行执行，
//  所以不会和当帧轮换到的 slot 冲突（见 docs/engine-backend-baseline.md §2）。
//

import Foundation
import Metal

final class StereoCalibrator {
    private let context: MetalContext
    private let resources: MetalFrameResources
    private let geometry: StereoGeometrySolver
    private let rectifier: MetalRectifier
    /// 打分用块匹配（与 `Matcher.block` 同一个实现）。
    private let matcher: MetalBlockMatcher
    /// 打分时读回 cost 纹理的复用缓冲（0.9 MB，避免每帧分配）。
    private var costScratch: [Float] = []

    init(context: MetalContext,
         resources: MetalFrameResources,
         geometry: StereoGeometrySolver,
         rectifier: MetalRectifier,
         matcher: MetalBlockMatcher) {
        self.context = context
        self.resources = resources
        self.geometry = geometry
        self.rectifier = rectifier
        self.matcher = matcher
    }

    /// 对一对帧做自动标定。返回 nil 之外的情况都由 C++ 的候选搜索决定；
    /// 搜索全部失败时保留原参数（与改造前一致）。
    func calibrate(_ inputs: FrameInputs, _ settings: PipelineSettings) -> (flip: Bool, dy: Float) {
        guard let r = SVStereoCore.calibrate({ [self] flipped, dy in
            self.score(flipped: flipped, dy: dy, inputs: inputs, s: settings)
        }, flip0: settings.flip, dy0: settings.yOffset) else { return (settings.flip, settings.yOffset) }
        return (r.flipped, r.yOffset)
    }

    /// 一次打分：两路按候选几何重采样 → Census → 块匹配代价 → 取非负值的平均。
    /// 返回 `.infinity` 表示这次打分失败（参数非法 / 没有有效像素）。
    private func score(flipped: Bool, dy: Float, inputs: FrameInputs, s: PipelineSettings) -> Float {
        let slot = resources.scoringSlot
        guard let g = geometry.solve(rig: inputs.rig,
                                     wide: inputs.wideIntrinsics, ultra: inputs.ultraIntrinsics,
                                     sourceWidth: Float(inputs.wide.width),
                                     sourceHeight: Float(inputs.wide.height),
                                     flip: flipped, yOffset: dy),
              let cb = context.queue.makeCommandBuffer(),
              let enc = cb.makeComputeCommandEncoder() else { return .infinity }

        // 编码顺序与改造前逐行一致：两次矫正写同一个目标纹理，中间各插一次 Census。
        rectifier.encode(into: enc, source: inputs.wide, destination: slot.right, transform: g.MW)
        matcher.encodeCensus(into: enc, source: slot.right, destination: slot.censusL)
        rectifier.encode(into: enc, source: inputs.ultra, destination: slot.right, transform: g.MU)
        matcher.encodeCensus(into: enc, source: slot.right, destination: slot.censusR)
        matcher.encodeMatch(into: enc, input: slot.matchInput(left: slot.right), geometry: g, settings: s)
        enc.endEncoding()
        cb.commit()
        cb.waitUntilCompleted()

        let n = StereoWork.width * StereoWork.height
        if costScratch.count != n { costScratch = [Float](repeating: 0, count: n) }
        costScratch.withUnsafeMutableBytes {
            slot.cost.getBytes($0.baseAddress!, bytesPerRow: StereoWork.width * 4,
                               from: MTLRegionMake2D(0, 0, StereoWork.width, StereoWork.height), mipmapLevel: 0)
        }
        // 归约在 Bridge 里做（Swift 侧不做逐元素运算）。
        let mean = SVExporter.meanNonNegative(costScratch, count: n)
        return mean.isFinite ? mean : .infinity
    }
}
