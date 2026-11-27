//
//  StereoConfiguration.swift
//  StereoVision
//
//  "用哪套后端"是**配置**，不是又一条复制出来的全流程。
//
//  产品上的三种模式（见 STEREO_ENGINE_BACKEND_REFACTOR_PLAN.md §5）：
//
//    .metal      rectification=Metal, matching=Metal(SGM|Block), depth=Metal
//    .cppHybrid  rectification=Metal, readback=CPU, matching=C++, depth=C++
//    系统深度    走 SystemDepthProcessor，不经过几何/匹配/视差转深度，所以不在这里
//
//  未来接 ONNX 时只增加一个 MatchingBackend 实现 + 一个配置项，
//  不要再写一条 `stereoONNX(...)` 全流程函数。
//

/// 双目流程的后端组合。
enum StereoMode: Sendable {
    /// 矫正 / 匹配 / 深度全部在 Metal：一帧一条 command buffer。
    case metal
    /// Metal 矫正 + 像素读回 + C++ 匹配 + C++ 深度回转。
    ///
    /// 注意命名：这**不是**全 CPU 流水线。全 CPU 编排在 C++ 的 `stereo_process_frame`，
    /// 那是给非 Apple 前端的入口，iOS 不走它。
    case cppHybrid
}

extension PipelineSettings {
    /// 当前设置对应的后端组合。
    ///
    /// `cpuMatcher` 由 AppModel 依据 `DepthSource` 推入（`.cpuCore` → true），
    /// 不放进设置页的 picker。
    var stereoMode: StereoMode { cpuMatcher ? .cppHybrid : .metal }
}
