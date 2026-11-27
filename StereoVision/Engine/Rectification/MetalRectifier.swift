//
//  MetalRectifier.swift
//  StereoVision
//
//  矫正阶段的 Metal 实现：Shaders.metal 的 `rectify` kernel（4 次子采样 + 双线性）。
//
//  与改造前逐项一致：目标尺寸 = 工作分辨率，srcSize 用**源纹理**的尺寸，
//  参数结构是 RectParams（ABI 见 MetalShaderTypes.swift）。
//

import Metal
import simd

final class MetalRectifier: RectificationBackend {
    private let pipeline: MTLComputePipelineState
    private let dispatch: MetalDispatch

    init(pipelines: MetalPipelines, dispatch: MetalDispatch = MetalDispatch()) {
        pipeline = pipelines.rectify
        self.dispatch = dispatch
    }

    func encode(into encoder: MTLComputeCommandEncoder,
                source: MTLTexture,
                destination: MTLTexture,
                transform: simd_float3x3) {
        var p = RectParams(M: transform, srcSize: SIMD2(Float(source.width), Float(source.height)))
        dispatch.encode(encoder, pipeline, textures: [source, destination],
                        bytes: &p, length: MemoryLayout<RectParams>.stride)
    }
}
