//
//  StereoTypes.swift
//  StereoVision
//
//  引擎各阶段之间传递的**值类型**。这里没有算法、没有 Metal、没有 C++ 细节：
//    StereoWork       工作分辨率与最大视差（所有阶段共用的常量）
//    StereoGeometry   几何求解的结果（单应 / K / 方向 / 基线）
//    StereoFrame      一帧的产物（纹理 + 共享深度缓冲 + 计时字段）
//
//  StereoFrame 的生命周期约定（详见 docs/engine-backend-baseline.md）：
//    * retained 里的对象（CVMetalTexture / CMSampleBuffer）必须活到 GPU 与 C++ 都用完；
//    * depthPointer 指向共享 MTLBuffer，谁读谁负责（跨帧使用要先拷贝，MediaExportManager 就是这么做的）。
//

import CoreVideo
import Metal
import simd

/// 工作分辨率与最大视差。
///
/// 这些数字同时被 Metal kernel、C++ 匹配和导出使用，所以只在这里写一次；
/// `StereoEngine.workW/workH/maxD` 只是它的别名（调用方不必关心从哪来）。
enum StereoWork {
    static let width = 640
    static let height = 360
    /// Metal SGM 的 cost volume 上限（Shaders.metal 的 SGMParams.D 不超过它）。
    static let maxDisparity = 96
}

/// 一帧的几何结果。由 `StereoGeometrySolver` 产出，之后**只读**地传给各阶段。
///
/// 三种矩阵都是齐次像素坐标之间的单应（`p_dst ∝ M * p_src`）：
///   MW   矫正后左图像素 → 宽相机原始像素（rectify 用）
///   MU   矫正后左图像素 → 超广角原始像素（rectify 用）
///   MUp  宽相机像素（工作分辨率、K 坐标系）→ 矫正后图像素（深度回转用）
struct StereoGeometry {
    var MW: simd_float3x3
    var MU: simd_float3x3
    var MUp: simd_float3x3
    /// 矫正后的共同内参 (fx, fy, cx, cy)，深度图就在这个坐标系里。
    var K: Intrinsics
    /// 匹配方向：+1 表示右图的同名点在左图右侧（来自 geometry 的 t.x）。
    var dir: Int32
    /// fx * baseline(米)：直接传给 toDepth 的 fB。
    var fB: Float
    var baselineMM: Float
    var baselineDeg: Float
}

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
