//
//  SystemDepthProcessor.swift
//  StereoVision
//
//  iPhone 系统深度路径（Apple Dual-Wide / LiDAR）。
//
//  **它不是 SGM 的一个后端**：没有自研极线矫正、没有匹配、没有视差→深度，
//  所以也不进入 StereoMode 的组合。系统给的深度图只做一次范围过滤
//  （core/src/cloud.cpp 的 depth_filter，经 SVExporter 调用），然后直接当深度图用。
//
//  目标缓冲按深度图尺寸缓存 3 份轮换；尺寸变了就整组重建。
//

import AVFoundation
import CoreVideo
import Metal

final class SystemDepthProcessor {
    private let context: MetalContext
    private let resources: MetalFrameResources

    init(context: MetalContext, resources: MetalFrameResources) {
        self.context = context
        self.resources = resources
    }

    /// - Parameter fovDeg: 宽相机的水平视场角，只在系统没给 `cameraCalibrationData` 时用于回退。
    func process(_ video: CMSampleBuffer, _ depthData: AVDepthData,
                 _ settings: PipelineSettings, fovDeg: Float) -> StereoFrame? {
        guard let pb = CMSampleBufferGetImageBuffer(video), let (tex, cv) = context.texture(pb) else { return nil }
        // 统一成 Float32 深度（Disparity 格式必须先转换，否则不能当米用）。
        let data = depthData.depthDataType == kCVPixelFormatType_DepthFloat32
            ? depthData
            : depthData.converting(toDepthDataType: kCVPixelFormatType_DepthFloat32)
        let map = data.depthDataMap
        let w = CVPixelBufferGetWidth(map), h = CVPixelBufferGetHeight(map)
        let target = resources.nextAppleTarget(width: w, height: h)

        CVPixelBufferLockBaseAddress(map, .readOnly)
        let src = CVPixelBufferGetBaseAddress(map)!
        let srcStride = CVPixelBufferGetBytesPerRow(map)
        let dst = target.buffer.contents()
        // 逐像素的 minZ/maxZ 归一化在 C++（core/src/cloud.cpp 的 depth_filter）。
        _ = SVExporter.filterDepth(src.assumingMemoryBound(to: Float.self),
                                   srcStride: srcStride / MemoryLayout<Float>.size,
                                   dst: dst.assumingMemoryBound(to: Float.self),
                                   dstStride: target.bytesPerRow / MemoryLayout<Float>.size,
                                   width: w, height: h, minZ: settings.minZ, maxZ: settings.maxZ)
        CVPixelBufferUnlockBaseAddress(map, .readOnly)

        let K = CameraCalibrationReader.depthIntrinsics(data, width: w, height: h, fovDeg: fovDeg)
        return StereoFrame(color: tex, right: nil, target: target, K: K, image: pb,
                           gpuMs: 0, baselineMM: 0, retained: [cv])
    }
}
