//
//  MetalDispatch.swift
//  StereoVision
//
//  dispatch/encode 的公共样板：绑 PSO → 绑纹理/缓冲/常量 → 按工作分辨率发线程。
//
//  数值口径与改造前完全一致：网格恒为工作分辨率（640×360），
//  线程组 = threadExecutionWidth × (maxTotalThreadsPerThreadgroup / threadExecutionWidth)。
//  SGM 的 sgmPath 需要自定义 threadgroup，所以不走这里（见 MetalSGMMatcher）。
//

import Metal

struct MetalDispatch {
    let width: Int
    let height: Int

    init(width: Int = StereoWork.width, height: Int = StereoWork.height) {
        self.width = width
        self.height = height
    }

    func encode(_ enc: MTLComputeCommandEncoder, _ pso: MTLComputePipelineState,
                textures: [MTLTexture] = [], buffers: [MTLBuffer] = [],
                bytes: UnsafeRawPointer? = nil, length: Int = 0) {
        enc.setComputePipelineState(pso)
        for (i, t) in textures.enumerated() { enc.setTexture(t, index: i) }
        for (i, b) in buffers.enumerated() { enc.setBuffer(b, offset: 0, index: i) }
        // 常量结构放在缓冲之后的那个 index 上（kernel 里的 [[buffer(n)]] 也是这么编的）。
        if let bytes { enc.setBytes(bytes, length: length, index: buffers.count) }
        let tw = pso.threadExecutionWidth
        let th = max(1, pso.maxTotalThreadsPerThreadgroup / tw)
        enc.dispatchThreads(MTLSize(width: width, height: height, depth: 1),
                            threadsPerThreadgroup: MTLSize(width: tw, height: th, depth: 1))
    }
}
