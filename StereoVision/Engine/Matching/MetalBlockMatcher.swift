//
//  MetalBlockMatcher.swift
//  StereoVision
//
//  块匹配（`Matcher.block`）的 Metal 实现：Shaders.metal 的 `match` kernel
//  （5×5 窗口、96 档视差、唯一性 + 亚像素）。
//
//  与 SGM 分开成两个类，是因为资源与生命周期完全不同：
//  Block 不需要 cost volume / 路径累加缓冲，只写 disparity 和 cost 两张纹理。
//
//  注意：标定打分也用这个 matcher（打分比的是 cost 图的平均值），
//  所以改动这里会同时影响 `Matcher.block` 与自动标定。
//

import Metal

final class MetalBlockMatcher: MatchingBackend {
    private let pipelines: MetalPipelines
    private let dispatch: MetalDispatch

    init(pipelines: MetalPipelines, dispatch: MetalDispatch = MetalDispatch()) {
        self.pipelines = pipelines
        self.dispatch = dispatch
    }

    func encodeCensus(into encoder: MTLComputeCommandEncoder, source: MTLTexture, destination: MTLTexture) {
        dispatch.encode(encoder, pipelines.census, textures: [source, destination])
    }

    func encodeMatch(into encoder: MTLComputeCommandEncoder, input: MetalMatchInput,
                     geometry g: StereoGeometry, settings s: PipelineSettings) {
        // maxCost = 1200 与改造前一致：cost 纹理里是「归一化代价」，无效像素写 -1。
        var m = MatchParams(maxDisp: Int32(s.maxDisparity), dir: g.dir,
                            uniqueness: s.uniqueness, maxCost: 1200)
        dispatch.encode(encoder, pipelines.match,
                        textures: [input.censusLeft, input.censusRight, input.disparity, input.cost],
                        bytes: &m, length: MemoryLayout<MatchParams>.stride)
    }
}
