//
//  Camera/CameraCalibrationReader.swift
//  StereoVision
//
//  四步管线里的**第 1 步：双目标定**。
//
//      Swift 这一层只做一件事：把 iOS 已经算好的相机参数**读出来**。
//      iPhone 的 constituent 相机出厂就标定过，AVFoundation 直接给：
//        * 内参  K          ← 逐帧附件 kCMSampleBufferAttachmentKey_CameraIntrinsicMatrix
//                              （没有时按 activeFormat.videoFieldOfView 推一个针孔模型）
//        * 外参  R, t       ← AVCaptureDevice.extrinsicMatrix(from:to:)（t 单位毫米）
//        * 深度内参         ← AVDepthData.cameraCalibrationData（含 referenceDimensions 归一化）
//
//  这个文件的边界，请严格守住：
//    ✅ 从 CMSampleBuffer / AVCaptureDevice / AVDepthData 读参数
//    ✅ 把系统给的内参换算到「实际图像像素」（referenceDimensions、FOV 回退）
//    ❌ 任何几何推导（矫正旋转、单应、视差→深度）—— 那些在 core/src/geometry.cpp，
//       也就是第 2 步。这里只搬数，不解方程。
//
//  以后要加前置相机 / 深度相机：在这个文件里加一个 `CameraCalibrationReader` 的
//  `static func`（或给 `DVDepthData` 那一条加一支），然后把内参交给
//  `StereoEngine` / `SVRectifyInput` 即可。不要在这里写任何几何推导。
//

import AVFoundation
import CoreVideo
import simd

// MARK: - 数据的形状

/// 针孔内参，单位是**原始图像像素**。
///
/// 这是全 App 唯一的 Intrinsics 定义：DepthSceneAnalyzer / MetalSceneRenderer / 导出 meta.json 都用它，
/// 交给 C++ 时按字段拆进 `stereo_rectify_input.wide/ultra`。
struct Intrinsics: Sendable {
    var fx: Float = 1
    var fy: Float = 1
    var cx: Float = 0
    var cy: Float = 0
}

/// 一台相机读到的标定信息。
///
/// 内参的单位是**这一帧的原始图像像素**；要换分辨率就按 `新宽/原宽` 缩放
/// （C++ 的 rectification 就是这么做的，见 `RectifyInput.src_width`）。
struct CameraCalibration: Sendable {
    var intrinsics: Intrinsics
    var fovDeg: Float
}

/// 两台相机之间的外参：`X_to = rotation * X_from + translation`。
struct StereoExtrinsics: Sendable {
    var rotation: simd_float3x3 = matrix_identity_float3x3
    /// 单位与基线相同（AVFoundation 给毫米）。
    var translation: SIMD3<Float> = SIMD3(12, 0, 0)
    /// `false` = 系统没给外参，上面是回退值（此时不该相信深度）。
    var fromSystem: Bool = false
}

/// 双摄整套参数：第 1 步的最终产物。
struct CameraRig: Sendable {
    var wide: CameraCalibration
    var ultra: CameraCalibration
    var extrinsics: StereoExtrinsics

    var hasExtrinsics: Bool { extrinsics.fromSystem }
}

// MARK: - 读取（唯一的 AVFoundation 接触面）

/// 相机参数的读取入口。全部是静态方法，无状态。
enum CameraCalibrationReader {

    /// 从一帧视频里读内参。
    ///
    /// 优先用逐帧附件（`kCMSampleBufferAttachmentKey_CameraIntrinsicMatrix`）——
    /// 它跟着当前 format 与实际输出分辨率走，比设备级 `activeFormat` 更有针对性；
    /// 拿不到时按视场角推一个正方形像素的针孔模型（`f = w / (2 tan(fov/2))`）。
    ///
    /// - Parameters:
    ///   - size: 这一帧的像素尺寸（缩放后分辨率的帧要传缩放后的尺寸）。
    ///   - fov: 该相机的 `activeFormat.videoFieldOfView`（度，水平）。
    static func intrinsics(of sampleBuffer: CMSampleBuffer,
                            fov: Float,
                            size: SIMD2<Float>) -> CameraCalibration {
        let fallback = pinhole(fovDeg: fov, size: size)
        let k: Intrinsics
        if let data = CMGetAttachment(sampleBuffer,
                                      key: kCMSampleBufferAttachmentKey_CameraIntrinsicMatrix,
                                      attachmentModeOut: nil) as? Data {
            // AVFoundation 给的是列主序的 3x3：m[0][0]=fx, m[1][1]=fy, m[2][0]=cx, m[2][1]=cy
            let m = data.withUnsafeBytes { $0.loadUnaligned(as: simd_float3x3.self) }
            k = Intrinsics(fx: m[0][0], fy: m[1][1], cx: m[2][0], cy: m[2][1])
        } else {
            k = fallback
        }
        return CameraCalibration(intrinsics: k, fovDeg: fov)
    }

    /// 直接从设备读（不看具体一帧）。"还没开流就要知道参数"的场合用它。
    static func calibration(of device: AVCaptureDevice) -> CameraCalibration {
        let fov = device.activeFormat.videoFieldOfView
        let dims = CMVideoFormatDescriptionGetDimensions(device.activeFormat.formatDescription)
        let size = SIMD2<Float>(Float(dims.width), Float(dims.height))
        return CameraCalibration(intrinsics: pinhole(fovDeg: fov, size: size), fovDeg: fov)
    }

    /// 两台相机的外参。`from` / `to` 是 constituent device。
    ///
    /// 返回的 `rotation`/`translation` 满足 `X_to = R * X_from + t`（t 单位毫米）。
    /// 系统不给时返回 `fromSystem == false` 与一个 12 mm 的回退基线——那是"未知"的
    /// 标记，不是可用的标定值，`FramePipeline` 会据此在 HUD 上提示。
    static func extrinsics(from source: AVCaptureDevice,
                           to target: AVCaptureDevice) -> StereoExtrinsics {
        var e = StereoExtrinsics()
        guard let data = AVCaptureDevice.extrinsicMatrix(from: source, to: target) else { return e }
        let m = data.withUnsafeBytes { $0.loadUnaligned(as: simd_float4x3.self) }
        e.rotation = simd_float3x3(columns: (m.columns.0, m.columns.1, m.columns.2))
        e.translation = m.columns.3
        e.fromSystem = simd_length(e.translation) > 1
        if !e.fromSystem {
            e.rotation = matrix_identity_float3x3
            e.translation = SIMD3(12, 0, 0)
        }
        return e
    }

    /// 双摄整套参数（宽 + 超广角）。
    static func rig(wide: AVCaptureDevice, ultra: AVCaptureDevice) -> CameraRig {
        CameraRig(wide: calibration(of: wide),
                  ultra: calibration(of: ultra),
                  extrinsics: extrinsics(from: ultra, to: wide))
    }

    /// 系统深度图自带的内参（LiDAR / Apple Dual-Wide 都走这里）。
    ///
    /// `cameraCalibrationData` 的内参是相对 `intrinsicMatrixReferenceDimensions` 的，
    /// 而深度图本身可能被裁过（高度方向裁），所以要按宽度缩放再补一个垂直偏移。
    static func depthIntrinsics(_ depth: AVDepthData, width: Int, height: Int,
                               fovDeg: Float) -> Intrinsics {
        guard let cal = depth.cameraCalibrationData else {
            return pinhole(fovDeg: fovDeg, size: SIMD2(Float(width), Float(height)))
        }
        let m = cal.intrinsicMatrix
        let ref = cal.intrinsicMatrixReferenceDimensions
        let sc = Float(width) / Float(ref.width)
        let off = (Float(ref.height) * sc - Float(height)) / 2
        return Intrinsics(fx: m[0][0] * sc, fy: m[1][1] * sc,
                          cx: m[2][0] * sc, cy: m[2][1] * sc - off)
    }

    /// 针孔回退：正方形像素，主点在正中。
    static func pinhole(fovDeg: Float, size: SIMD2<Float>) -> Intrinsics {
        let f = size.x / (2 * tan(fovDeg * .pi / 360))
        return Intrinsics(fx: f, fy: f, cx: size.x / 2, cy: size.y / 2)
    }
}
