import AVFoundation
import CoreVideo
import Metal
import QuartzCore
import simd

final class StereoFrame: @unchecked Sendable {
    let color: MTLTexture
    let pairLeft: MTLTexture?
    let right: MTLTexture?
    let depth: MTLTexture
    let width: Int, height: Int
    let K: Intrinsics
    let image: CVPixelBuffer
    let gpuMs: Double
    /// 分段计时（仅 C++ 路径有值，Metal 路径全为 0）。
    ///
    /// 三个字段加起来就是 `cpuMs`，诊断页每一行都有真实数字，不留横杠：
    ///   readbackMs  像素读回（CVPixelBuffer 锁定 + MTLTexture.getBytes）
    ///   matchMs     census → costVolume → sgmPath×4 → wta（C++ 匹配）
    ///   depthMs     toDepth → unrectify（C++ 深度）
    let readbackMs: Double
    let matchMs: Double
    let depthMs: Double
    /// CPU 一段的总和。用算的，避免两个地方各自累加而走样。
    var cpuMs: Double { readbackMs + matchMs + depthMs }
    /// 矫正这一段（两路单应的 GPU 重采样）。
    let gpuRectifyMs: Double
    let baselineMM: Float
    let baselineDeg: Float
    /// 已矫正的左图（rectify 的输出，32BGRA）。给 C++ 后端读回像素用。
    let pairLeftPB: CVPixelBuffer?
    /// 匹配方向：+1 表示右图的同名点在左图右侧（来自 geometry 的 t.x）。
    let dir: Int32
    private let buffer: MTLBuffer
    private let rowStride: Int
    private let retained: [AnyObject]

    init(color: MTLTexture, pairLeft: MTLTexture? = nil, right: MTLTexture?, target: DepthTarget, K: Intrinsics, image: CVPixelBuffer, gpuMs: Double, baselineMM: Float, baselineDeg: Float = 0, pairLeftPB: CVPixelBuffer? = nil, dir: Int32 = 1, readbackMs: Double = 0, matchMs: Double = 0, depthMs: Double = 0, gpuRectifyMs: Double = 0, retained: [AnyObject]) {
        self.color = color
        self.pairLeft = pairLeft
        self.right = right
        self.depth = target.texture
        self.buffer = target.buffer
        self.width = target.width
        self.height = target.height
        self.rowStride = target.bytesPerRow / 4
        self.K = K
        self.image = image
        self.gpuMs = gpuMs
        self.readbackMs = readbackMs
        self.matchMs = matchMs
        self.depthMs = depthMs
        self.gpuRectifyMs = gpuRectifyMs
        self.baselineMM = baselineMM
        self.baselineDeg = baselineDeg
        self.pairLeftPB = pairLeftPB
        self.dir = dir
        self.retained = retained
    }

    /// 深度图在 CPU 侧的指针与**像素**行跨度（指向共享 MTLBuffer，C++ 直接读写）。
    /// 点选测距、连通域分析、导出都走这两个。
    var depthPointer: UnsafeMutablePointer<Float> {
        buffer.contents().assumingMemoryBound(to: Float.self)
    }
    var depthStride: Int { rowStride }
}

final class DepthTarget {
    let buffer: MTLBuffer
    let texture: MTLTexture
    let width: Int, height: Int, bytesPerRow: Int

    init(device: MTLDevice, width: Int, height: Int) {
        let align = device.minimumLinearTextureAlignment(for: .r32Float)
        bytesPerRow = (width * 4 + align - 1) / align * align
        buffer = device.makeBuffer(length: bytesPerRow * height, options: .storageModeShared)!
        let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .r32Float, width: width, height: height, mipmapped: false)
        d.usage = [.shaderRead, .shaderWrite]
        d.storageMode = .shared
        texture = buffer.makeTexture(descriptor: d, offset: 0, bytesPerRow: bytesPerRow)!
        self.width = width
        self.height = height
    }
}

final class StereoEngine: @unchecked Sendable {
    static let workW = 640, workH = 360, maxD = 96

    let device: MTLDevice
    private let queue: MTLCommandQueue
    private let rectifyPSO, censusPSO, matchPSO, depthPSO: MTLComputePipelineState
    private let unrectPSO: MTLComputePipelineState
    private let costPSO, pathPSO, wtaRightPSO, wtaLeftPSO: MTLComputePipelineState
    private let costBuffer, sumBuffer: MTLBuffer
    private let dispRight: MTLTexture
    private var cache: CVMetalTextureCache!
    private var leftPool: CVPixelBufferPool!
    private let slots: [Slot]
    private var appleTargets: [DepthTarget] = []
    private var slotIndex = 0

    /// 第 1 步（Calibration.swift）读到的双摄参数。只要 session 建起来就有值，
    /// `hasExtrinsics == false` 表示系统没给外参、用的是回退值。
    private var rig: CameraRig?
    var hasExtrinsics: Bool { rig?.hasExtrinsics ?? false }

    /// C++ 核心上下文：CPU 匹配、视差→深度、深度回转、点选测距、连通域分析共用它。
    /// 不是 lazy —— 即使走 Metal 路径，准星读数与面块分析也要用它。
    /// 为 nil 表示 stereo_create 失败（只会因为内存不足），此时只剩 Metal 路径能跑。
    let core: SVStereoCore?

    private final class Slot {
        let right, censusL, censusR, disp, cost, depthRolled: MTLTexture
        /// 深度图目标（背后是共享 MTLBuffer，C++ 的 unrectify_depth 直接写进去）。
        let target: DepthTarget

        init(device: MTLDevice, w: Int, h: Int) {
            func tex(_ f: MTLPixelFormat, shared: Bool = false) -> MTLTexture {
                let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: f, width: w, height: h, mipmapped: false)
                d.usage = [.shaderRead, .shaderWrite]
                d.storageMode = shared ? .shared : .private
                return device.makeTexture(descriptor: d)!
            }
            right = tex(.rgba8Unorm, shared: true)
            censusL = tex(.rg32Uint)
            censusR = tex(.rg32Uint)
            disp = tex(.r32Float)
            depthRolled = tex(.r32Float)
            cost = tex(.r32Float, shared: true)
            target = DepthTarget(device: device, width: w, height: h)
        }
    }

    struct Geometry {
        var MW: simd_float3x3, MU: simd_float3x3
        var MUp: simd_float3x3
        var K: Intrinsics
        var dir: Int32
        var fB: Float
        var baselineMM: Float
        var baselineDeg: Float
    }

    init(device: MTLDevice) {
        self.device = device
        // 第 3/4 步的 C++ context（兼做点选测距与连通域分析的公用上下文）。
        core = SVStereoCore(maxDisparity: 64, p1: 8, p2: 96, uniqueness: 0.1, lrCheck: true)
        queue = device.makeCommandQueue()!
        let lib = device.makeDefaultLibrary()!
        func pso(_ n: String) -> MTLComputePipelineState { try! device.makeComputePipelineState(function: lib.makeFunction(name: n)!) }
        rectifyPSO = pso("rectify")
        censusPSO = pso("census")
        matchPSO = pso("match")
        depthPSO = pso("toDepth")
        unrectPSO = pso("unrectifyDepth")
        costPSO = pso("costVolume")
        pathPSO = pso("sgmPath")
        wtaRightPSO = pso("wtaRight")
        wtaLeftPSO = pso("wtaLeft")
        let cells = Self.workW * Self.workH * Self.maxD
        costBuffer = device.makeBuffer(length: cells, options: .storageModePrivate)!
        sumBuffer = device.makeBuffer(length: cells * 2, options: .storageModePrivate)!
        let rd = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .r32Float, width: Self.workW, height: Self.workH, mipmapped: false)
        rd.usage = [.shaderRead, .shaderWrite]
        rd.storageMode = .private
        dispRight = device.makeTexture(descriptor: rd)!
        CVMetalTextureCacheCreate(nil, nil, device, nil, &cache)
        let attrs: [String: Any] = [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
            kCVPixelBufferWidthKey as String: Self.workW,
            kCVPixelBufferHeightKey as String: Self.workH,
            kCVPixelBufferMetalCompatibilityKey as String: true,
            kCVPixelBufferIOSurfacePropertiesKey as String: [:] as [String: Any],
        ]
        CVPixelBufferPoolCreate(nil, nil, attrs as CFDictionary, &leftPool)
        slots = (0..<3).map { _ in Slot(device: device, w: Self.workW, h: Self.workH) }
    }

    func configure(wide: AVCaptureDevice, ultra: AVCaptureDevice) {
        rig = CalibrationSource.rig(wide: wide, ultra: ultra)
    }

    private func texture(_ pb: CVPixelBuffer, writable: Bool = false) -> (MTLTexture, CVMetalTexture)? {
        let attrs = writable ? [kCVMetalTextureUsage: MTLTextureUsage([.shaderRead, .shaderWrite]).rawValue] as CFDictionary : nil
        var cv: CVMetalTexture?
        CVMetalTextureCacheCreateTextureFromImage(nil, cache, pb, attrs, .bgra8Unorm, CVPixelBufferGetWidth(pb), CVPixelBufferGetHeight(pb), 0, &cv)
        guard let cv, let tex = CVMetalTextureGetTexture(cv) else { return nil }
        return (tex, cv)
    }

    /// 读一帧的内参。第 1 步的实际入口是 Calibration.swift，这里只是补上角色与视场角。
    private func intrinsics(_ sb: CMSampleBuffer, role: CameraRole, fov: Float, size: SIMD2<Float>) -> Intrinsics {
        CalibrationSource.intrinsics(of: sb, role: role, fov: fov, size: size).intrinsics
    }

    /// 第 2 步：极线矫正的几何。
    ///
    /// 数学全部在 C++（core/src/geometry.cpp），这里只负责把第 1 步读到的内参/外参
    /// 装箱交给 SVRectification，再把结果换回本文件用的 simd 类型。
    /// 返回 nil 表示参数不可用（外参为零、尺寸非法），调用方应该丢掉这一帧。
    func geometry(KW: Intrinsics, KU: Intrinsics, srcW: Float, flip: Bool, dy: Float) -> Geometry? {
        guard let rig else { return nil }
        let input = SVRectifyInput()
        input.wideIntrinsics = SIMD4(KW.fx, KW.fy, KW.cx, KW.cy)
        input.ultraIntrinsics = SIMD4(KU.fx, KU.fy, KU.cx, KU.cy)
        input.rotation = rig.extrinsics.rotation
        input.translation = rig.extrinsics.translation
        input.sourceWidth = srcW
        input.workWidth = Self.workW
        input.workHeight = Self.workH
        input.flip = flip
        input.yOffset = dy
        guard let r = SVRectification.solve(input) else { return nil }
        let k = r.intrinsics
        return Geometry(MW: r.rectifiedToWide, MU: r.rectifiedToUltra, MUp: r.wideToRectified,
                        K: Intrinsics(fx: k.x, fy: k.y, cx: k.z, cy: k.w), dir: Int32(r.direction),
                        fB: r.focalBaseline, baselineMM: r.baselineMM, baselineDeg: r.baselineDegrees)
    }

    private func dispatch(_ enc: MTLComputeCommandEncoder, _ pso: MTLComputePipelineState, _ textures: [MTLTexture], buffers: [MTLBuffer] = [], bytes: UnsafeRawPointer? = nil, length: Int = 0) {
        enc.setComputePipelineState(pso)
        for (i, t) in textures.enumerated() { enc.setTexture(t, index: i) }
        for (i, b) in buffers.enumerated() { enc.setBuffer(b, offset: 0, index: i) }
        if let bytes { enc.setBytes(bytes, length: length, index: buffers.count) }
        let tw = pso.threadExecutionWidth
        let th = max(1, pso.maxTotalThreadsPerThreadgroup / tw)
        enc.dispatchThreads(MTLSize(width: Self.workW, height: Self.workH, depth: 1), threadsPerThreadgroup: MTLSize(width: tw, height: th, depth: 1))
    }

    private func encodeRectify(_ enc: MTLComputeCommandEncoder, _ src: MTLTexture, _ dst: MTLTexture, _ M: simd_float3x3) {
        var p = RectParams(M: M, srcSize: SIMD2(Float(src.width), Float(src.height)))
        dispatch(enc, rectifyPSO, [src, dst], bytes: &p, length: MemoryLayout<RectParams>.stride)
    }

    private func encodeMatch(_ enc: MTLComputeCommandEncoder, _ slot: Slot, _ g: Geometry, _ s: PipelineSettings) {
        var m = MatchParams(maxDisp: Int32(s.maxDisparity), dir: g.dir, uniqueness: s.uniqueness, maxCost: 1200)
        dispatch(enc, matchPSO, [slot.censusL, slot.censusR, slot.disp, slot.cost], bytes: &m, length: MemoryLayout<MatchParams>.stride)
    }

    private func encodeSGM(_ enc: MTLComputeCommandEncoder, _ slot: Slot, _ left: MTLTexture, _ g: Geometry, _ s: PipelineSettings) {
        let D = min(Self.maxD, max(32, s.maxDisparity / 32 * 32))
        var p = SGMParams(W: Int32(Self.workW), H: Int32(Self.workH), D: Int32(D), dir: g.dir, dx: 0, dy: 0,
                          P1: UInt32(s.p1), P2: UInt32(s.p2), first: 1, uniqueness: s.uniqueness, lrCheck: s.lrCheck ? 1 : 0, pad: 0)
        let len = MemoryLayout<SGMParams>.stride
        dispatch(enc, costPSO, [slot.censusL, slot.censusR], buffers: [costBuffer], bytes: &p, length: len)
        enc.setComputePipelineState(pathPSO)
        enc.setBuffer(costBuffer, offset: 0, index: 0)
        enc.setBuffer(sumBuffer, offset: 0, index: 1)
        enc.setTexture(left, index: 0)
        for (i, (dx, dy)) in [(1, 0), (-1, 0), (0, 1), (0, -1)].enumerated() {
            p.dx = Int32(dx)
            p.dy = Int32(dy)
            p.first = i == 0 ? 1 : 0
            enc.setBytes(&p, length: len, index: 2)
            enc.dispatchThreadgroups(MTLSize(width: dx != 0 ? Self.workH : Self.workW, height: 1, depth: 1),
                                     threadsPerThreadgroup: MTLSize(width: D, height: 1, depth: 1))
        }
        dispatch(enc, wtaRightPSO, [dispRight], buffers: [sumBuffer], bytes: &p, length: len)
        dispatch(enc, wtaLeftPSO, [dispRight, slot.disp], buffers: [sumBuffer], bytes: &p, length: len)
    }

    private struct Inputs {
        let wTex: MTLTexture, uTex: MTLTexture
        let wpb: CVPixelBuffer, upb: CVPixelBuffer
        let KW: Intrinsics, KU: Intrinsics
        let keep: [AnyObject]
    }

    private func inputs(_ wide: CMSampleBuffer, _ ultra: CMSampleBuffer) -> Inputs? {
        guard let wpb = CMSampleBufferGetImageBuffer(wide), let upb = CMSampleBufferGetImageBuffer(ultra),
              let (wTex, wcv) = texture(wpb), let (uTex, ucv) = texture(upb) else { return nil }
        let ws = SIMD2(Float(wTex.width), Float(wTex.height)), us = SIMD2(Float(uTex.width), Float(uTex.height))
        return Inputs(wTex: wTex, uTex: uTex, wpb: wpb, upb: upb,
                      KW: intrinsics(wide, role: .wide, fov: rig?.wide.fovDeg ?? 70, size: ws),
                      KU: intrinsics(ultra, role: .ultraWide, fov: rig?.ultra.fovDeg ?? 108, size: us),
                      keep: [wcv, ucv])
    }

    func stereo(_ wide: CMSampleBuffer, _ ultra: CMSampleBuffer, _ s: PipelineSettings) -> StereoFrame? {
        guard let inp = inputs(wide, ultra) else { return nil }
        var lpb: CVPixelBuffer?
        CVPixelBufferPoolCreatePixelBuffer(nil, leftPool, &lpb)
        guard let lpb, let (lTex, lcv) = texture(lpb, writable: true), let cb = queue.makeCommandBuffer(),
              let enc = cb.makeComputeCommandEncoder() else { return nil }
        slotIndex = (slotIndex + 1) % slots.count
        let slot = slots[slotIndex]
        guard let g = geometry(KW: inp.KW, KU: inp.KU, srcW: Float(inp.wTex.width), flip: s.flip, dy: s.yOffset) else { return nil }
        encodeRectify(enc, inp.wTex, lTex, g.MW)
        encodeRectify(enc, inp.uTex, slot.right, g.MU)
        dispatch(enc, censusPSO, [lTex, slot.censusL])
        dispatch(enc, censusPSO, [slot.right, slot.censusR])
        if s.matcher == .sgm { encodeSGM(enc, slot, lTex, g, s) } else { encodeMatch(enc, slot, g, s) }
        var d = DepthParams(fB: g.fB, minZ: s.minZ, maxZ: s.maxZ, pad: 0)
        dispatch(enc, depthPSO, [slot.disp, slot.depthRolled], bytes: &d, length: MemoryLayout<DepthParams>.stride)
        var u = RectParams(M: g.MUp, srcSize: .zero)
        dispatch(enc, unrectPSO, [slot.depthRolled, slot.target.texture], bytes: &u, length: MemoryLayout<RectParams>.stride)
        enc.endEncoding()
        cb.commit()
        cb.waitUntilCompleted()
        let ms = (cb.gpuEndTime - cb.gpuStartTime) * 1000
        // The view shows the wide camera image itself (upright, whatever the lens layout) with the
        // depth map un-rolled onto it; the rectified pair stays available for the epipolar check.
        let retained: [AnyObject] = inp.keep + [lcv, wide]
        return StereoFrame(color: inp.wTex, pairLeft: lTex, right: slot.right, target: slot.target, K: g.K, image: inp.wpb,
                           gpuMs: ms, baselineMM: g.baselineMM, baselineDeg: g.baselineDeg,
                           pairLeftPB: lpb, dir: g.dir, retained: retained)
    }

    /// 与 `stereo(_:_:_:)` 同一条管线，但把 Census / SGM / WTA 交给 C++ 核心在 CPU 上跑。
    ///
    /// 分工（也就是诊断页上那张表）：
    ///   矫正                GPU（沿用 Metal kernel，输入输出不变）
    ///   读回像素            CPU（CVPixelBuffer 锁定 + MTLTexture.getBytes）
    ///   Census/SGM/WTA      CPU（core/src/sgm.cpp）
    ///   toDepth/unrectify   GPU（沿用 Metal kernel）
    func stereoCPU(_ wide: CMSampleBuffer, _ ultra: CMSampleBuffer, _ s: PipelineSettings) -> StereoFrame? {
        guard let inp = inputs(wide, ultra), let core else { return nil }
        guard core.configure(maxDisparity: s.maxDisparity, p1: s.p1, p2: s.p2,
                             uniqueness: s.uniqueness, lrCheck: s.lrCheck) else { return nil }

        var lpb: CVPixelBuffer?
        CVPixelBufferPoolCreatePixelBuffer(nil, leftPool, &lpb)
        guard let lpb, let (lTex, lcv) = texture(lpb, writable: true) else { return nil }

        slotIndex = (slotIndex + 1) % slots.count
        let slot = slots[slotIndex]
        guard let g = geometry(KW: inp.KW, KU: inp.KU, srcW: Float(inp.wTex.width), flip: s.flip, dy: s.yOffset) else { return nil }
        let w = Self.workW, h = Self.workH

        // ---- 1) 矫正：GPU ----
        guard let cb1 = queue.makeCommandBuffer(), let enc1 = cb1.makeComputeCommandEncoder() else { return nil }
        encodeRectify(enc1, inp.wTex, lTex, g.MW)
        encodeRectify(enc1, inp.uTex, slot.right, g.MU)
        enc1.endEncoding()
        cb1.commit()
        cb1.waitUntilCompleted()
        let rectifyMs = (cb1.gpuEndTime - cb1.gpuStartTime) * 1000

        // ---- 2) 读回像素 + C++ 匹配：CPU ----
        // 计时范围：分配 + 锁定 + getBytes。分配也计入，因为“每帧两次 malloc”本身就是这一段的开销。
        let readbackT0 = CACurrentMediaTime()
        CVPixelBufferLockBaseAddress(lpb, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(lpb, .readOnly) }
        guard let base = CVPixelBufferGetBaseAddress(lpb) else { return nil }

        // 左图来自 32BGRA 的 CVPixelBuffer，右图是 .rgba8Unorm 纹理 —— 字节序不同，格式要分开声明。
        let leftStride = CVPixelBufferGetBytesPerRow(lpb)
        let rightStride = w * 4
        let rightBytes = UnsafeMutablePointer<UInt8>.allocate(capacity: rightStride * h)
        let disp = UnsafeMutablePointer<Float>.allocate(capacity: w * h)
        defer {
            rightBytes.deallocate()
            disp.deallocate()
        }
        slot.right.getBytes(rightBytes, bytesPerRow: rightStride,
                            from: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0)
        let readbackMs = (CACurrentMediaTime() - readbackT0) * 1000

        let t0 = CACurrentMediaTime()
        let ok = core.matchLeft(base.assumingMemoryBound(to: UInt8.self),
                                leftStride: leftStride, leftFormat: .bgra8,
                                right: UnsafePointer(rightBytes),
                                rightStride: rightStride, rightFormat: .rgba8,
                                width: w, height: h, direction: Int(g.dir),
                                disparity: disp)
        let matchMs = (CACurrentMediaTime() - t0) * 1000
        guard ok else { return nil }

        // ---- 3) 视差 → 深度 → 回转：全部在 C++（第 4 步）----
        //
        // 以前这一段要再起一个 command buffer（上传视差 + toDepth + unrectify 两个 kernel），
        // 现在整段都是 CPU：stereo::to_depth 写进一块临时缓冲，stereo::unrectify_depth
        // 直接写进 target 背后的**共享 MTLBuffer**，一次上传/回读都不需要。
        let depthT0 = CACurrentMediaTime()
        let depthRaw = UnsafeMutablePointer<Float>.allocate(capacity: w * h)
        defer { depthRaw.deallocate() }
        // ObjC 导入器会把 `depthFromDisparity:` 按 SE-0005 改写成 `depth(fromDisparity:)`，
        // 这里必须用改写后的名字，否则 Swift 5 语言模式下是 error。
        guard core.depth(fromDisparity: disp, dispStride: w, width: w, height: h, fB: g.fB,
                         minZ: s.minZ, maxZ: s.maxZ,
                         depth: depthRaw, depthStride: w) else { return nil }
        guard SVStereoCore.unrectifyDepth(depthRaw, srcStride: w, width: w, height: h,
                                          transform: g.MUp,
                                          dst: slot.target.buffer.contents().assumingMemoryBound(to: Float.self),
                                          dstStride: slot.target.bytesPerRow / 4,
                                          dstWidth: w, dstHeight: h) else { return nil }
        let depthMs = (CACurrentMediaTime() - depthT0) * 1000

        let retained: [AnyObject] = inp.keep + [lcv, wide]
        return StereoFrame(color: inp.wTex, pairLeft: lTex, right: slot.right, target: slot.target, K: g.K, image: inp.wpb,
                           gpuMs: rectifyMs, baselineMM: g.baselineMM, baselineDeg: g.baselineDeg,
                           pairLeftPB: lpb, dir: g.dir,
                           readbackMs: readbackMs, matchMs: matchMs, depthMs: depthMs,
                           gpuRectifyMs: rectifyMs, retained: retained)
    }

    func calibrate(_ wide: CMSampleBuffer, _ ultra: CMSampleBuffer, _ s: PipelineSettings) -> (flip: Bool, dy: Float) {
        guard let inp = inputs(wide, ultra) else { return (s.flip, s.yOffset) }
        let slot = slots[0]
        let n = Self.workW * Self.workH
        var costs = [Float](repeating: 0, count: n)

        func score(flip: Bool, dy: Float) -> Float {
            guard let g = geometry(KW: inp.KW, KU: inp.KU, srcW: Float(inp.wTex.width), flip: flip, dy: dy),
                  let cb = queue.makeCommandBuffer(), let enc = cb.makeComputeCommandEncoder() else { return .infinity }
            encodeRectify(enc, inp.wTex, slot.right, g.MW)
            dispatch(enc, censusPSO, [slot.right, slot.censusL])
            encodeRectify(enc, inp.uTex, slot.right, g.MU)
            dispatch(enc, censusPSO, [slot.right, slot.censusR])
            encodeMatch(enc, slot, g, s)
            enc.endEncoding()
            cb.commit()
            cb.waitUntilCompleted()
            costs.withUnsafeMutableBytes {
                slot.cost.getBytes($0.baseAddress!, bytesPerRow: Self.workW * 4, from: MTLRegionMake2D(0, 0, Self.workW, Self.workH), mipmapLevel: 0)
            }
            var sum: Float = 0, cnt: Float = 0
            for c in costs where c >= 0 { sum += c; cnt += 1 }
            return cnt > 0 ? sum / cnt : .infinity
        }

        var best = (flip: s.flip, dy: s.yOffset, score: Float.infinity)
        for flip in [false, true] {
            for i in -12...12 {
                let v = score(flip: flip, dy: Float(i))
                if v < best.score { best = (flip, Float(i), v) }
            }
        }
        for d in [-0.5, -0.25, 0.25, 0.5] as [Float] {
            let v = score(flip: best.flip, dy: best.dy + d)
            if v < best.score { best = (best.flip, best.dy + d, v) }
        }
        return (best.flip, best.dy)
    }

    func depthData(_ video: CMSampleBuffer, _ depthData: AVDepthData, _ s: PipelineSettings) -> StereoFrame? {
        guard let pb = CMSampleBufferGetImageBuffer(video), let (tex, cv) = texture(pb) else { return nil }
        let data = depthData.depthDataType == kCVPixelFormatType_DepthFloat32 ? depthData : depthData.converting(toDepthDataType: kCVPixelFormatType_DepthFloat32)
        let map = data.depthDataMap
        let w = CVPixelBufferGetWidth(map), h = CVPixelBufferGetHeight(map)
        if appleTargets.first?.width != w || appleTargets.first?.height != h {
            appleTargets = (0..<3).map { _ in DepthTarget(device: device, width: w, height: h) }
        }
        slotIndex = (slotIndex + 1) % appleTargets.count
        let target = appleTargets[slotIndex]
        CVPixelBufferLockBaseAddress(map, .readOnly)
        let src = CVPixelBufferGetBaseAddress(map)!
        let srcStride = CVPixelBufferGetBytesPerRow(map)
        let dst = target.buffer.contents()
        // 逐像素的 minZ/maxZ 归一化在 C++（core/src/cloud.cpp 的 depth_filter）。
        _ = SVExporter.filterDepth(src.assumingMemoryBound(to: Float.self),
                                   srcStride: srcStride / MemoryLayout<Float>.size,
                                   dst: dst.assumingMemoryBound(to: Float.self),
                                   dstStride: target.bytesPerRow / MemoryLayout<Float>.size,
                                   width: w, height: h, minZ: s.minZ, maxZ: s.maxZ)
        CVPixelBufferUnlockBaseAddress(map, .readOnly)

        let K = CalibrationSource.depthIntrinsics(data, width: w, height: h, fovDeg: rig?.wide.fovDeg ?? 70)
        return StereoFrame(color: tex, right: nil, target: target, K: K, image: pb, gpuMs: 0, baselineMM: 0, retained: [cv])
    }
}

struct RectParams { var M: simd_float3x3; var srcSize: SIMD2<Float> }
struct MatchParams { var maxDisp: Int32; var dir: Int32; var uniqueness: Float; var maxCost: Float }
struct DepthParams { var fB: Float; var minZ: Float; var maxZ: Float; var pad: Int32 }
struct SGMParams {
    var W: Int32, H: Int32, D: Int32, dir: Int32, dx: Int32, dy: Int32
    var P1: UInt32, P2: UInt32
    var first: Int32
    var uniqueness: Float
    var lrCheck: Int32, pad: Int32
}
