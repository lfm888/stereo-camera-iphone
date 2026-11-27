//
//  MetalSGMMatcher.swift
//  StereoVision
//
//  SGM 4 路径的 Metal 实现：costVolume → sgmPath×4 → wtaRight → wtaLeft。
//
//  costBuffer / sumBuffer / dispRight 是**这个 matcher 自己的资源**（改造前挂在
//  StereoEngine 上）。它们每帧复用、不做多份轮换：命令队列串行提交，且一帧的
//  command buffer 完成之后才会再编码下一帧。
//
//  D 的取法与改造前一致：min(96, max(32, maxDisparity / 32 * 32))，向下取到 32 的倍数。
//

import Metal

final class MetalSGMMatcher: MatchingBackend {
    private let pipelines: MetalPipelines
    private let dispatch: MetalDispatch
    private let costBuffer, sumBuffer: MTLBuffer
    private let dispRight: MTLTexture

    init(context: MetalContext, pipelines: MetalPipelines, dispatch: MetalDispatch = MetalDispatch()) {
        self.pipelines = pipelines
        self.dispatch = dispatch
        let cells = StereoWork.width * StereoWork.height * StereoWork.maxDisparity
        costBuffer = context.device.makeBuffer(length: cells, options: .storageModePrivate)!
        sumBuffer = context.device.makeBuffer(length: cells * 2, options: .storageModePrivate)!
        let rd = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .r32Float,
                                                          width: StereoWork.width,
                                                          height: StereoWork.height,
                                                          mipmapped: false)
        rd.usage = [.shaderRead, .shaderWrite]
        rd.storageMode = .private
        dispRight = context.device.makeTexture(descriptor: rd)!
    }

    func encodeCensus(into encoder: MTLComputeCommandEncoder, source: MTLTexture, destination: MTLTexture) {
        dispatch.encode(encoder, pipelines.census, textures: [source, destination])
    }

    func encodeMatch(into encoder: MTLComputeCommandEncoder, input: MetalMatchInput,
                     geometry g: StereoGeometry, settings s: PipelineSettings) {
        let D = min(StereoWork.maxDisparity, max(32, s.maxDisparity / 32 * 32))
        var p = SGMParams(W: Int32(StereoWork.width), H: Int32(StereoWork.height), D: Int32(D),
                          dir: g.dir, dx: 0, dy: 0,
                          P1: UInt32(s.p1), P2: UInt32(s.p2), first: 1,
                          uniqueness: s.uniqueness, lrCheck: s.lrCheck ? 1 : 0, pad: 0)
        let len = MemoryLayout<SGMParams>.stride

        // 代价体：Census 的汉明距离，越界的 d 写 48（与 shader 一致）。
        dispatch.encode(encoder, pipelines.costVolume,
                        textures: [input.censusLeft, input.censusRight],
                        buffers: [costBuffer], bytes: &p, length: len)

        // 4 条路径（右 / 左 / 下 / 上）。第一条覆盖 sumBuffer，后面三条累加。
        encoder.setComputePipelineState(pipelines.sgmPath)
        encoder.setBuffer(costBuffer, offset: 0, index: 0)
        encoder.setBuffer(sumBuffer, offset: 0, index: 1)
        encoder.setTexture(input.left, index: 0)
        for (i, (dx, dy)) in [(1, 0), (-1, 0), (0, 1), (0, -1)].enumerated() {
            p.dx = Int32(dx)
            p.dy = Int32(dy)
            p.first = i == 0 ? 1 : 0
            encoder.setBytes(&p, length: len, index: 2)
            // 每条路径一个 threadgroup：D 个线程，每条扫描线一个 group。
            encoder.dispatchThreadgroups(MTLSize(width: dx != 0 ? StereoWork.height : StereoWork.width,
                                                height: 1, depth: 1),
                                         threadsPerThreadgroup: MTLSize(width: D, height: 1, depth: 1))
        }

        // 右视差（给左视差的左右一致性检查用）→ 左视差（唯一性 + LR + 亚像素）。
        dispatch.encode(encoder, pipelines.wtaRight, textures: [dispRight],
                        buffers: [sumBuffer], bytes: &p, length: len)
        dispatch.encode(encoder, pipelines.wtaLeft, textures: [dispRight, input.disparity],
                        buffers: [sumBuffer], bytes: &p, length: len)
    }
}
