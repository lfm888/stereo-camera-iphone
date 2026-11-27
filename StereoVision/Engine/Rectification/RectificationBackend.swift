//
//  RectificationBackend.swift
//  StereoVision
//
//  矫正阶段的窄接口。
//
//  为什么接口是"往调用方给的 encoder 里编码"而不是 `rectify(...) -> RectifiedPair`：
//  Metal 路径当前**一帧只提交一条 command buffer**（矫正 → census → 匹配 → 深度 → 回转
//  共享同一次提交，`waitUntilCompleted` 只等一次）。如果接口按阶段各自提交，就会变成
//  每阶段一个 command buffer，性能和同步行为都会变。所以接口暴露 encoder，
//  由编排层决定提交时机。
//
//  目前只有一个实现（MetalRectifier）：C++ 的 `stereo_rectify_image` 在 Bridge 里没有暴露，
//  所以不建 CppRectifier（"确有 C++ 矫正 API 时再添加"）。
//

import Metal
import simd

protocol RectificationBackend {
    /// 把 `source` 按单应 `transform` 重采样到 `destination`（两者尺寸相同、格式相同）。
    func encode(into encoder: MTLComputeCommandEncoder,
                source: MTLTexture,
                destination: MTLTexture,
                transform: simd_float3x3)
}
