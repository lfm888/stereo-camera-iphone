//
//  MetalDepthProcessor.swift
//  StereoVision
//
//  深度阶段的 Metal 实现：`toDepth`（3×3 中值 + 范围截断）→ `unrectifyDepth`
//  （矫正坐标系映回宽相机坐标系）。
//
//  参数与改造前一致：
//    DepthParams(fB, minZ, maxZ)               fB = fx * baseline(米)
//    RectParams(M = geometry.MUp, srcSize = 0) unrectify 不用 srcSize
//

import Metal

final class MetalDepthProcessor: MetalDepthBackend {
    private let pipelines: MetalPipelines
    private let dispatch: MetalDispatch

    init(pipelines: MetalPipelines, dispatch: MetalDispatch = MetalDispatch()) {
        self.pipelines = pipelines
        self.dispatch = dispatch
    }

    func encode(into encoder: MTLComputeCommandEncoder,
                disparity: MTLTexture,
                rectifiedDepth: MTLTexture,
                target: DepthTarget,
                geometry g: StereoGeometry,
                settings s: PipelineSettings) {
        var d = DepthParams(fB: g.fB, minZ: s.minZ, maxZ: s.maxZ, pad: 0)
        dispatch.encode(encoder, pipelines.toDepth,
                        textures: [disparity, rectifiedDepth],
                        bytes: &d, length: MemoryLayout<DepthParams>.stride)
        var u = RectParams(M: g.MUp, srcSize: .zero)
        dispatch.encode(encoder, pipelines.unrectifyDepth,
                        textures: [rectifiedDepth, target.texture],
                        bytes: &u, length: MemoryLayout<RectParams>.stride)
    }
}
