//
//  MetalMatchingStage.swift
//  StereoVision
//
//  匹配阶段的窄接口（Metal 侧）。
//
//  阶段之间只传明确的数据对象：几何是 `StereoGeometry`，图像是纹理视图
//  `MetalMatchInput`，参数是 `PipelineSettings`。matcher 不访问相机、不算几何、
//  也不创建整条 pipeline —— 这些都属于输入层 / 几何求解 / 编排层。
//
//  **可替换的单位是整条管线（`StereoMode`），不是这一个阶段。**
//  目前两个实现（SGM / Block）都编码进同一条 command buffer；
//  C++ 匹配走 `CppSGMMatcher`（同步指针调用），不实现这个协议；
//  ONNX 真出现时再按它自己的 session/输入输出建接口，不要提前抽象。
//
//  约定（所有 matcher 一致，与 Shaders.metal、core/src/sgm.cpp 相同）：
//    * disparity 单位是像素，方向由 `StereoGeometry.dir` 决定（+1 = 右图同名点在左图右侧）；
//    * 匹配失败/越界的像素视差写 0；
//    * cost 只给标定打分与调试用，无效像素写 -1。
//

import Metal

/// 匹配阶段读写的资源视图（都来自 `MetalFrameResources` 的 slot）。
struct MetalMatchInput {
    /// 已矫正左图：SGM 的 `sgmPath` 要读它的亮度做 P2 自适应。
    let left: MTLTexture
    let censusLeft: MTLTexture
    let censusRight: MTLTexture
    let disparity: MTLTexture
    let cost: MTLTexture
}

/// Metal 匹配后端。
///
/// 为什么拆成 `encodeCensus` + `encodeMatch` 两步，而不是一个 `encode`：
///   1. Census 是所有 matcher 共有的前置步骤，但**编码时机**归编排层管 ——
///      标定打分要在两次 rectify 之间各插一次 census（两个候选矫正结果写同一个目标纹理），
///      合成一个 encode 就保不住这个顺序；
///   2. SGM 与 Block 的差异因此只落在 `encodeMatch` 这十几行里。
///
/// 未来接 ONNX 时，新增一个实现即可；如果 ONNX Runtime 需要独立的 session / 读回，
/// 由它自己封装，不要在 `StereoEngine` 里再复制一条全流程。
protocol MetalMatchingStage {
    /// 7×7 Census 变换（左右各调用一次）。
    func encodeCensus(into encoder: MTLComputeCommandEncoder, source: MTLTexture, destination: MTLTexture)

    /// 在已算好的 Census 上做匹配，写 `input.disparity`（以及 `input.cost`）。
    func encodeMatch(into encoder: MTLComputeCommandEncoder,
                     input: MetalMatchInput,
                     geometry: StereoGeometry,
                     settings: PipelineSettings)
}

extension MetalSlot {
    /// 当前 slot 的匹配输入视图（除左侧颜色纹理外，其余资源都在 slot 里）。
    func matchInput(left: MTLTexture) -> MetalMatchInput {
        MetalMatchInput(left: left, censusLeft: censusL, censusRight: censusR,
                        disparity: disp, cost: cost)
    }
}
