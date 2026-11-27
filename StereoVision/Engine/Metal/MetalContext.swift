//
//  MetalContext.swift
//  StereoVision
//
//  Metal 的公共上下文：device + command queue + CVMetalTextureCache。
//
//  这里只做"资源与搬运"，不含任何算法阶段：pipeline state 在 MetalPipelines，
//  Slot / DepthTarget / PixelBufferPool 在 MetalFrameResources，kernel 调度在各自的
//  backend 里。
//

import CoreVideo
import Metal

final class MetalContext {
    let device: MTLDevice
    /// 引擎唯一的命令队列：帧处理提交的 command buffer 都走它（串行提交，顺序有保证）。
    let queue: MTLCommandQueue
    private let cache: CVMetalTextureCache

    init(device: MTLDevice) {
        self.device = device
        queue = device.makeCommandQueue()!
        var c: CVMetalTextureCache?
        CVMetalTextureCacheCreate(nil, nil, device, nil, &c)
        cache = c!
    }

    /// CVPixelBuffer → MTLTexture（借来的 view）。
    ///
    /// 返回的 `CVMetalTexture` 必须被持有到 GPU 用完：调用方把它塞进
    /// `StereoFrame.retained`，与底层的 `CMSampleBuffer` 一起活到这一帧被替换为止。
    func texture(_ pb: CVPixelBuffer, writable: Bool = false) -> (MTLTexture, CVMetalTexture)? {
        let attrs = writable ? [kCVMetalTextureUsage: MTLTextureUsage([.shaderRead, .shaderWrite]).rawValue] as CFDictionary : nil
        var cv: CVMetalTexture?
        CVMetalTextureCacheCreateTextureFromImage(nil, cache, pb, attrs, .bgra8Unorm, CVPixelBufferGetWidth(pb), CVPixelBufferGetHeight(pb), 0, &cv)
        guard let cv, let tex = CVMetalTextureGetTexture(cv) else { return nil }
        return (tex, cv)
    }
}
