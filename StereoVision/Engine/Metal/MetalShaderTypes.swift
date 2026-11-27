//
//  MetalShaderTypes.swift
//  StereoVision
//
//  Metal kernel 的参数结构。
//
//  **ABI 必须与 StereoVision/Shaders.metal 里的同名 struct 逐字段一致**
//  （字段顺序、类型、对齐）。改这里就要同步改 shader，反之亦然。
//  这些类型只属于 Metal 阶段实现，不进通用编排层，也不给 C++ 看。
//

import simd

struct RectParams {
    var M: simd_float3x3
    var srcSize: SIMD2<Float>
}

struct MatchParams {
    var maxDisp: Int32
    var dir: Int32
    var uniqueness: Float
    var maxCost: Float
}

struct DepthParams {
    var fB: Float
    var minZ: Float
    var maxZ: Float
    var pad: Int32
}

struct SGMParams {
    var W: Int32, H: Int32, D: Int32, dir: Int32, dx: Int32, dy: Int32
    var P1: UInt32, P2: UInt32
    var first: Int32
    var uniqueness: Float
    var lrCheck: Int32, pad: Int32
}
