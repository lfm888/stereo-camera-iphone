//
//  CppDepthProcessor.swift
//  StereoVision
//
//  深度阶段的 C++ 实现（`.cppHybrid` 模式）：视差 → 深度 → 回转。
//
//  API 2.00 起用 core 的 `stereo_process_disparity`（经 Bridge 的 `+processDisparity:`）：
//  一次调用做完 "to_depth + unrectify"，与 `stereo_process_frame` 内部是同一份实现。
//  与 iOS 的 Metal 路径相比，这里唯一的区别是回转发生在 CPU、直接写进 DepthTarget
//  背后的共享 MTLBuffer（一次上传/回读都不需要）。
//
//  它是**无状态**的（2.00 去掉了 sample_depth / disparity_to_depth 的 ctx），
//  所以不再持有 SVStereoCore —— 主线程的查询不再需要第二个 context。
//
//  临时缓冲（depthRect）在这里分配、调用返回前释放；分配计入 depthMs（与改造前一致）。
//

import Foundation
import Metal

final class CppDepthProcessor {
    func makeDepth(disparity: UnsafePointer<Float>,
                   disparityStride: Int,
                   width: Int,
                   height: Int,
                   geometry g: StereoGeometry,
                   settings s: PipelineSettings,
                   target: DepthTarget) -> Bool {
        let depthRect = UnsafeMutablePointer<Float>.allocate(capacity: width * height)
        defer { depthRect.deallocate() }

        return SVStereoCore.processDisparity(disparity,
                                            dispStride: disparityStride,
                                            width: width, height: height,
                                            fB: g.fB, minZ: s.minZ, maxZ: s.maxZ,
                                            transform: g.MUp,
                                            depth: target.buffer.contents()
                                                .assumingMemoryBound(to: Float.self),
                                            depthStride: target.bytesPerRow / 4,
                                            depthRectScratch: depthRect)
    }
}
