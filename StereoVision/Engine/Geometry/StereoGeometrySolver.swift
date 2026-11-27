//
//  StereoGeometrySolver.swift
//  StereoVision
//
//  第 2 步：极线矫正的几何。
//
//  数学全部在 C++（core/src/geometry.cpp），这里只负责把第 1 步读到的内参/外参
//  装箱交给 SVRectification（ObjC 门面），再把结果换回 Swift 侧的类型。
//  **不要在 Swift 里复制几何公式**，也不要把公式搬进 Metal —— 矫正 kernel 只消费这里的结果。
//

import simd

struct StereoGeometrySolver {
    let workWidth: Int
    let workHeight: Int

    init(workWidth: Int = StereoWork.width, workHeight: Int = StereoWork.height) {
        self.workWidth = workWidth
        self.workHeight = workHeight
    }

    /// 解一次矫正几何。
    ///
    /// - Parameters:
    ///   - rig: 双摄参数（外参在这里面）。nil 表示还没配置，直接失败。
    ///   - wide/ultra: 两台相机在**原始图像像素**下的内参。
    ///   - sourceWidth/Height: 原始图像尺寸（内参是按它给的，C++ 会按比例缩到工作分辨率）。
    ///   - flip/dy: 自动标定给出的基线方向与行偏移。
    /// - Returns: nil 表示参数不可用（外参为零、尺寸非法），调用方应该丢掉这一帧。
    func solve(rig: CameraRig?, wide: Intrinsics, ultra: Intrinsics,
               sourceWidth: Float, sourceHeight: Float,
               flip: Bool, yOffset: Float) -> StereoGeometry? {
        guard let rig else { return nil }
        let input = SVRectifyInput()
        input.wideIntrinsics = SIMD4(wide.fx, wide.fy, wide.cx, wide.cy)
        input.ultraIntrinsics = SIMD4(ultra.fx, ultra.fy, ultra.cx, ultra.cy)
        input.rotation = rig.extrinsics.rotation
        input.translation = rig.extrinsics.translation
        input.sourceWidth = sourceWidth
        input.sourceHeight = sourceHeight
        input.workWidth = workWidth
        input.workHeight = workHeight
        input.flip = flip
        input.yOffset = yOffset
        guard let r = SVRectification.solve(input) else { return nil }
        let k = r.intrinsics
        return StereoGeometry(MW: r.rectifiedToWide, MU: r.rectifiedToUltra, MUp: r.wideToRectified,
                              K: Intrinsics(fx: k.x, fy: k.y, cx: k.z, cy: k.w), dir: Int32(r.direction),
                              fB: r.focalBaseline, baselineMM: r.baselineMM, baselineDeg: r.baselineDegrees)
    }
}
