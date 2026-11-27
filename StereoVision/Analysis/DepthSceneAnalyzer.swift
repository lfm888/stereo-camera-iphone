//
//  DepthSceneAnalyzer.swift
//  StereoVision
//
//  **薄适配层**：把 core/src/analyze.cpp 算出来的深度连通域，翻译成 SwiftUI 用得上的类型。
//
//  这里一行算法都没有 —— 降采样、4 邻域连通域、容差、排序、反投影全在 C++：
//      DepthSceneAnalyzer.analyze()  →  SVStereoCore.analyzeDepth(_:...into:)
//                          →  stereo_analyze()（core/src/stereo_c_api.cpp）
//                          →  analyze_depth()（core/src/analyze.cpp）
//
//  之所以还留一个 Swift 文件：`Analysis` / `Blob` 被 OverlayView（画框）、MetalSceneRenderer（上传标签图）、
//  MediaExportManager（写 meta.json）三处消费，它们需要 CGRect / Swift 数组。转换就在这一层做完。
//

import CoreGraphics

/// 深度连通域切出来的一个「面片块」：3D segmentation 视图用它上色和标注。
struct Blob: Identifiable, Sendable {
    let id: Int
    var rect: CGRect
    var distance: Float
    var x: Float
    var width: Float
    var cells: Int
}

/// 每帧的分析结果。**只来自深度图**，不依赖任何检测/分割模型。
struct Analysis: Sendable {
    var blobs: [Blob] = []
    var labels: [UInt8] = []
    var gridW = 0
    var gridH = 0
    var center: Float?
}

final class DepthSceneAnalyzer {
    /// 复用同一个结果容器：C++ 侧的临时缓冲跟着它走，稳态零分配。
    private let result = SVAnalysis()

    /// 准星采样框（归一化）：画面中心偏上一点，取中位数。
    private static let centerRect = SIMD4<Float>(0.475, 0.455, 0.525, 0.545)
    private static let maxBlobs = 24
    private static let minCells = 40

    /// 分析一帧。`core` 为 nil（C++ context 创建失败）时返回空结果，不崩。
    func analyze(_ f: StereoFrame, using core: SVStereoCore?) -> Analysis {
        var a = Analysis()
        guard let core else { return a }
        let k = f.K
        guard core.analyzeDepth(f.depthPointer,
                                stride: f.depthStride,
                                width: f.width,
                                height: f.height,
                                intrinsics: SIMD4(k.fx, k.fy, k.cx, k.cy),
                                centerRect: Self.centerRect,
                                centerQ: 0.5,
                                maxBlobs: Self.maxBlobs,
                                minCells: Self.minCells,
                                into: result) else { return a }

        a.gridW = result.gridWidth
        a.gridH = result.gridHeight
        if result.hasCenter { a.center = result.center }

        // 面块：C 结构数组 → Swift 值类型数组（一次遍历，内部不再分配）
        let count = result.blobCount
        if count > 0 {
            var blobs = [Blob]()
            blobs.reserveCapacity(count)
            let records = UnsafeBufferPointer(
                start: result.blobs().assumingMemoryBound(to: SVBlobRecord.self), count: count)
            for r in records {
                blobs.append(Blob(id: Int(r.identifier),
                                  rect: CGRect(x: CGFloat(r.rectX), y: CGFloat(r.rectY),
                                               width: CGFloat(r.rectW), height: CGFloat(r.rectH)),
                                  distance: r.distance, x: r.x, width: r.width, cells: Int(r.cells)))
            }
            a.blobs = blobs
        }

        // 标签图：C++ 已经按"保留的块"重排成 1..count，直接给 MetalSceneRenderer 上传
        let n = a.gridW * a.gridH
        if n > 0 {
            a.labels = Array(UnsafeBufferPointer(
                start: result.labels().assumingMemoryBound(to: UInt8.self), count: n))
        }
        return a
    }
}
