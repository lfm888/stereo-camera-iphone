//
//  CppSGMMatcher.swift
//  StereoVision
//
//  C++ 匹配后端：Census + costVolume + SGM 4 路径 + WTA 全部在 CPU（core/src/sgm.cpp）。
//
//  输入不是纹理，而是 `CPUImageReadback`（显式的像素读回适配），调用**同步**返回：
//  C++ 不会保留指针，所以调用方可以放心在作用域结束时解锁/释放临时缓冲。
//
//  这是 `.cppHybrid` 模式的匹配阶段。它不是"全 CPU 流水线"：
//  矫正仍然在 GPU（MetalRectifier），匹配前还有一次读回。
//

import Foundation

final class CppSGMMatcher {
    private let core: SVStereoCore

    init(core: SVStereoCore) {
        self.core = core
    }

    /// 把设置推进 C++ context。改 maxDisparity 会重新分配内部缓冲，所以只在参数变化时
    /// 有意义 —— 但改造前也是每帧调用，这里保持同一行为（configure 本身不重建）。
    /// 返回 false 时看 `core.lastError`。
    func configure(_ settings: PipelineSettings) -> Bool {
        core.configure(maxDisparity: settings.maxDisparity, p1: settings.p1, p2: settings.p2,
                       uniqueness: settings.uniqueness, lrCheck: settings.lrCheck)
    }

    /// 在已矫正的一对图上跑一次匹配，结果写进 `readback.disparity`。
    ///
    /// direction = +1 表示右图的同名点在左图右侧（来自 `StereoGeometry.dir`）。
    func match(_ readback: CPUImageReadback, direction: Int32) -> Bool {
        core.matchLeft(readback.leftBase, leftStride: readback.leftStride, leftFormat: .bgra8,
                       right: readback.rightBytes, rightStride: readback.rightStride, rightFormat: .rgba8,
                       width: readback.width, height: readback.height,
                       direction: Int(direction), disparity: readback.disparity)
    }
}
