//
//  MetalFrameResources.swift
//  StereoVision
//
//  Metal 侧的**资源池**：Slot（每帧轮换的纹理集合）、深度目标、左图 PixelBufferPool。
//
//  这里只有资源，没有算法：谁写哪张纹理由对应的 backend 决定，
//  "什么时候可以复用"由 StereoEngine 的提交顺序保证（一帧一条 command buffer，
//  waitUntilCompleted 之后才组帧；三条产品路径共用一条相机队列，串行执行）。
//

import CoreVideo
import Metal

/// 深度图目标：一块共享 MTLBuffer + 一张 r32Float 纹理（同一块内存的两个视图）。
///
/// 之所以是共享内存：GPU 路径由 `unrectifyDepth` kernel 写，C++ 混合路径由
/// `SVStereoCore.unrectifyDepth` 直接写；之后 Analyzer（准星/连通域）、点选测距、
/// 导出都在 CPU 上读同一块。行跨度按设备的最小线性纹理对齐补齐，`bytesPerRow` 是字节。
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

/// 一帧用到的纹理集合。三份轮换，避免和上一帧还没显示完的资源抢同一批纹理。
final class MetalSlot {
    /// 矫正后的超广角图（.rgba8Unorm，shared —— C++ 读回要用）
    let right, censusL, censusR, disp, cost, depthRolled: MTLTexture
    /// 深度图目标（背后是共享 MTLBuffer）
    let target: DepthTarget

    init(device: MTLDevice, width: Int, height: Int) {
        func tex(_ f: MTLPixelFormat, shared: Bool = false) -> MTLTexture {
            let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: f, width: width, height: height, mipmapped: false)
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
        target = DepthTarget(device: device, width: width, height: height)
    }
}

final class MetalFrameResources {
    let slots: [MetalSlot]
    private let device: MTLDevice
    private var appleTargets: [DepthTarget] = []
    private var slotIndex = 0
    private var leftPool: CVPixelBufferPool!

    init(device: MTLDevice, width: Int = StereoWork.width, height: Int = StereoWork.height) {
        self.device = device
        slots = (0..<3).map { _ in MetalSlot(device: device, width: width, height: height) }
        let attrs: [String: Any] = [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
            kCVPixelBufferWidthKey as String: width,
            kCVPixelBufferHeightKey as String: height,
            kCVPixelBufferMetalCompatibilityKey as String: true,
            kCVPixelBufferIOSurfacePropertiesKey as String: [:] as [String: Any],
        ]
        CVPixelBufferPoolCreate(nil, nil, attrs as CFDictionary, &leftPool)
    }

    /// 下一个 slot（轮换）。与改造前的 `(slotIndex + 1) % slots.count` 一致。
    ///
    /// 调用方必须保证这一帧的 CPU/GPU 读取结束之前，这个 slot 不会被再次轮换到 ——
    /// 3 份 slot 就是给"显示 + 分析还在读上一帧"留的余量。
    func nextSlot() -> MetalSlot {
        slotIndex = (slotIndex + 1) % slots.count
        return slots[slotIndex]
    }

    /// 标定打分固定用 slots[0]（与改造前一致）。标定与帧处理同在相机队列上串行。
    var scoringSlot: MetalSlot { slots[0] }

    /// 矫正后的左图输出缓冲（rectify 的目标，C++ 读回也读它）。
    func makeRectifiedLeftBuffer() -> CVPixelBuffer? {
        var pb: CVPixelBuffer?
        CVPixelBufferPoolCreatePixelBuffer(nil, leftPool, &pb)
        return pb
    }

    /// 系统深度路径的目标（尺寸随深度图变，变了就重建 3 份）。
    /// 与自研双目共用同一个 slotIndex 轮换，行为与改造前一致。
    func nextAppleTarget(width: Int, height: Int) -> DepthTarget {
        if appleTargets.first?.width != width || appleTargets.first?.height != height {
            appleTargets = (0..<3).map { _ in DepthTarget(device: device, width: width, height: height) }
        }
        slotIndex = (slotIndex + 1) % appleTargets.count
        return appleTargets[slotIndex]
    }
}
