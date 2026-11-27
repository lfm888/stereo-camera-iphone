//
//  FrameInputProvider.swift
//  StereoVision
//
//  相机输入适配层：把 CMSampleBuffer 换成 Metal 纹理，并补上这一帧的内参。
//
//  边界：
//    ✅ 从 CMSampleBuffer 取 CVPixelBuffer → MTLTexture（经 CVMetalTextureCache）
//    ✅ 读这一帧的内参（CalibrationSource.intrinsics，第 1 步）
//    ✅ 持有已配置的 CameraRig（外参来自 AVCaptureDevice.extrinsicMatrix）
//    ❌ 任何几何推导 —— 那是 StereoGeometrySolver → core/src/geometry.cpp
//
//  `rig` 由 CameraController 的 session 队列写入（`onDevices`），由相机数据队列读取。
//  改造前就是这个情形（没有额外同步），本轮不改线程模型，只把状态收进这一处。
//

import AVFoundation
import CoreVideo
import Metal

/// 一帧的原始输入：两台相机的纹理 + 内参 + 已配置的双摄参数。
struct FrameInputs {
    let wide: MTLTexture
    let ultra: MTLTexture
    let wideBuffer: CVPixelBuffer
    let ultraBuffer: CVPixelBuffer
    let wideIntrinsics: Intrinsics
    let ultraIntrinsics: Intrinsics
    /// 已配置的双摄参数；nil = 还没收到 `configure`（几何求解会失败，这一帧丢掉）。
    let rig: CameraRig?
    /// 这一帧必须一起活到 GPU 用完的借来对象（CVMetalTexture）。
    let keep: [AnyObject]
}

final class FrameInputProvider {
    private let context: MetalContext
    private var rig: CameraRig?

    init(context: MetalContext) {
        self.context = context
    }

    /// 相机会话建好后由 `CameraController.onDevices` 调用。
    func configure(wide: AVCaptureDevice, ultra: AVCaptureDevice) {
        rig = CalibrationSource.rig(wide: wide, ultra: ultra)
    }

    var hasExtrinsics: Bool { rig?.hasExtrinsics ?? false }

    /// 宽相机的水平视场角（系统深度内参的回退值用的是它）。
    var wideFieldOfView: Float { rig?.wide.fovDeg ?? 70 }

    /// 同步一对帧 → 纹理、内参、外参。
    ///
    /// 拿不到图像缓冲或纹理缓存创建失败时返回 nil，调用方丢掉这一帧。
    func inputs(_ wide: CMSampleBuffer, _ ultra: CMSampleBuffer) -> FrameInputs? {
        guard let wpb = CMSampleBufferGetImageBuffer(wide), let upb = CMSampleBufferGetImageBuffer(ultra),
              let (wTex, wcv) = context.texture(wpb), let (uTex, ucv) = context.texture(upb) else { return nil }
        let ws = SIMD2(Float(wTex.width), Float(wTex.height))
        let us = SIMD2(Float(uTex.width), Float(uTex.height))
        return FrameInputs(wide: wTex, ultra: uTex,
                           wideBuffer: wpb, ultraBuffer: upb,
                           wideIntrinsics: intrinsics(of: wide, fov: rig?.wide.fovDeg ?? 70, size: ws),
                           ultraIntrinsics: intrinsics(of: ultra, fov: rig?.ultra.fovDeg ?? 108, size: us),
                           rig: rig,
                           keep: [wcv, ucv])
    }

    /// 读一帧的内参。第 1 步的实际入口是 Calibration.swift，这里只补上角色与视场角。
    private func intrinsics(of sb: CMSampleBuffer, fov: Float, size: SIMD2<Float>) -> Intrinsics {
        CalibrationSource.intrinsics(of: sb, fov: fov, size: size).intrinsics
    }
}
