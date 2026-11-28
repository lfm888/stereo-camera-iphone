//
//  DepthBackend.swift
//  StereoVision
//
//  深度阶段的窄接口。这里只有 Metal 版是协议，因为只有它有"异步编码"的形状：
//  往调用方的 command buffer 里编码两个 kernel（toDepth / unrectify），
//  与矫正、匹配共享同一次提交。
//
//  C++ 版（CppDepthProcessor）是同步指针调用、直接写共享 MTLBuffer，
//  没有 encoder 可给；把它塞进同一个协议只会得到一个两套语义都模糊的接口，
//  所以它是具体类型（见 CppDepthProcessor.swift）。阶段边界仍由 `StereoEngine` 编排。
//
//  **可替换的单位是整条管线（`StereoMode`），不是这一个阶段。**
//
//  约定：disparity 单位是像素、无效值 0；depth 单位是米、无效值 0、
//  坐标系由 geometry.K 与 MUp 决定（校正后 / 宽相机）。
//

import Metal

protocol MetalDepthStage {
    /// 视差 → 深度（矫正坐标系）→ 回转（宽相机坐标系，写进 `target.texture`）。
    func encode(into encoder: MTLComputeCommandEncoder,
                disparity: MTLTexture,
                rectifiedDepth: MTLTexture,
                target: DepthTarget,
                geometry: StereoGeometry,
                settings: PipelineSettings)
}
