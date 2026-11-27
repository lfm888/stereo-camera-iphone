//
//  CppDepthProcessor.swift
//  StereoVision
//
//  深度阶段的 C++ 实现（`.cppHybrid` 模式）：
//    core.depth(fromDisparity:)      视差 → 深度（3×3 中值 + 范围截断，core/src/sgm.cpp）
//    SVStereoCore.unrectifyDepth     深度回转（core/src/rectify.cpp，逐条对齐 unrectifyDepth kernel）
//
//  两步都是同步调用。第二步**直接写进 DepthTarget 背后的共享 MTLBuffer**，
//  所以不需要一次上传/回读（改造前就是这样，注释里说"再起一个 command buffer"是旧实现）。
//
//  临时缓冲（depthRaw）在这里分配、调用返回前释放；分配计入 depthMs（口径与改造前一致）。
//

import Foundation
import Metal

final class CppDepthProcessor {
    private let core: SVStereoCore

    init(core: SVStereoCore) {
        self.core = core
    }

    func makeDepth(disparity: UnsafePointer<Float>,
                   disparityStride: Int,
                   width: Int,
                   height: Int,
                   geometry g: StereoGeometry,
                   settings s: PipelineSettings,
                   target: DepthTarget) -> Bool {
        let depthRaw = UnsafeMutablePointer<Float>.allocate(capacity: width * height)
        defer { depthRaw.deallocate() }

        // ObjC 导入器会把 `depthFromDisparity:` 按 SE-0005 改写成 `depth(fromDisparity:)`，
        // 这里必须用改写后的名字，否则 Swift 5 语言模式下是 error。
        guard core.depth(fromDisparity: disparity, dispStride: disparityStride,
                         width: width, height: height, fB: g.fB,
                         minZ: s.minZ, maxZ: s.maxZ,
                         depth: depthRaw, depthStride: width) else { return false }

        return SVStereoCore.unrectifyDepth(depthRaw, srcStride: width, width: width, height: height,
                                           transform: g.MUp,
                                           dst: target.buffer.contents().assumingMemoryBound(to: Float.self),
                                           dstStride: target.bytesPerRow / 4,
                                           dstWidth: width, dstHeight: height)
    }
}
