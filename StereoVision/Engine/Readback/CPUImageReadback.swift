//
//  CPUImageReadback.swift
//  StereoVision
//
//  "把已矫正的一对图变成 C++ 能读的一块内存" —— 这个适配步骤是**显式**的，
//  不藏在通用 matcher 里（见 docs/engine-backend-refactor.md 的「读回」一节）。
//
//  两种源的格式不一样，所以左右各自带格式声明（由 CppSGMMatcher 使用）：
//    左：rectify 的输出 CVPixelBuffer，32BGRA，行跨度由 CoreVideo 对齐
//    右：rectify 的输出纹理 .rgba8Unorm，行跨度固定 width*4
//
//  生命周期全部收在这个对象里：pixel buffer 的锁定、右图与视差的临时缓冲。
//  C++ 调用是同步的，所以对象存活期间指针一直有效；deinit 时一起释放并解锁。
//  计时口径（readbackMs）由调用方包在 `make` 外面 —— 分配本身也算这一段的开销。
//

import CoreVideo
import Metal

final class CPUImageReadback {
    /// 左图基地址（已锁定的 CVPixelBuffer 内存，32BGRA）。
    let leftBase: UnsafeMutablePointer<UInt8>
    /// 左图字节行跨度。
    let leftStride: Int
    /// 右图紧凑拷贝（.rgba8Unorm 的字节序）。
    let rightBytes: UnsafeMutablePointer<UInt8>
    /// 右图字节行跨度（= width * 4）。
    let rightStride: Int
    /// 视差输出（width * height 个 float，无效像素由 C++ 写 0）。
    let disparity: UnsafeMutablePointer<Float>
    let width: Int
    let height: Int

    private let pixelBuffer: CVPixelBuffer

    /// 锁定左图缓冲、拷贝右图纹理、分配视差缓冲。
    ///
    /// - Parameters:
    ///   - pixelBuffer: 已矫正的左图（rectify 的 CVPixelBuffer 输出）。
    ///   - texture: 已矫正的右图（slot.right，.rgba8Unorm）。
    static func make(pixelBuffer: CVPixelBuffer, texture: MTLTexture, width: Int, height: Int) -> CPUImageReadback? {
        CVPixelBufferLockBaseAddress(pixelBuffer, .readOnly)
        guard let base = CVPixelBufferGetBaseAddress(pixelBuffer) else {
            CVPixelBufferUnlockBaseAddress(pixelBuffer, .readOnly)
            return nil
        }
        let rightStride = width * 4
        let rightBytes = UnsafeMutablePointer<UInt8>.allocate(capacity: rightStride * height)
        let disparity = UnsafeMutablePointer<Float>.allocate(capacity: width * height)
        texture.getBytes(rightBytes, bytesPerRow: rightStride,
                         from: MTLRegionMake2D(0, 0, width, height), mipmapLevel: 0)
        return CPUImageReadback(pixelBuffer: pixelBuffer,
                                leftBase: base.assumingMemoryBound(to: UInt8.self),
                                leftStride: CVPixelBufferGetBytesPerRow(pixelBuffer),
                                rightBytes: rightBytes, rightStride: rightStride,
                                disparity: disparity, width: width, height: height)
    }

    private init(pixelBuffer: CVPixelBuffer, leftBase: UnsafeMutablePointer<UInt8>, leftStride: Int,
                 rightBytes: UnsafeMutablePointer<UInt8>, rightStride: Int,
                 disparity: UnsafeMutablePointer<Float>, width: Int, height: Int) {
        self.pixelBuffer = pixelBuffer
        self.leftBase = leftBase
        self.leftStride = leftStride
        self.rightBytes = rightBytes
        self.rightStride = rightStride
        self.disparity = disparity
        self.width = width
        self.height = height
    }

    deinit {
        rightBytes.deallocate()
        disparity.deallocate()
        CVPixelBufferUnlockBaseAddress(pixelBuffer, .readOnly)
    }
}
