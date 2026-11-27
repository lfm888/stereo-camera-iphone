//
//  MetalPipelines.swift
//  StereoVision
//
//  引擎用到的全部 compute pipeline state。**只在这里创建与持有**：
//  StereoEngine 不再逐个 makeComputePipelineState，各 backend 也不再自己造 PSO。
//

import Metal

final class MetalPipelines {
    /// rectify：单应重采样（矫正）
    let rectify: MTLComputePipelineState
    /// census：7×7 Census 变换（左右各一次）
    let census: MTLComputePipelineState
    /// match：SGM 的块匹配代价版（Matcher.block）
    let match: MTLComputePipelineState
    /// toDepth：视差 → 深度（3×3 中值 + 范围截断）
    let toDepth: MTLComputePipelineState
    /// unrectifyDepth：矫正坐标系 → 宽相机坐标系
    let unrectifyDepth: MTLComputePipelineState
    /// costVolume / sgmPath×4 / wtaRight / wtaLeft：SGM 4 路径
    let costVolume: MTLComputePipelineState
    let sgmPath: MTLComputePipelineState
    let wtaRight: MTLComputePipelineState
    let wtaLeft: MTLComputePipelineState

    init(device: MTLDevice) {
        let lib = device.makeDefaultLibrary()!
        func pso(_ n: String) -> MTLComputePipelineState {
            try! device.makeComputePipelineState(function: lib.makeFunction(name: n)!)
        }
        rectify = pso("rectify")
        census = pso("census")
        match = pso("match")
        toDepth = pso("toDepth")
        unrectifyDepth = pso("unrectifyDepth")
        costVolume = pso("costVolume")
        sgmPath = pso("sgmPath")
        wtaRight = pso("wtaRight")
        wtaLeft = pso("wtaLeft")
    }
}
