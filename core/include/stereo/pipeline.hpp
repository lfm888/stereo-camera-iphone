// stereo/pipeline.hpp — 第 2..5 步的 C++ 组合（C 版本是 stereo_process_frame）
//
// 为什么要有 C++ 版本：`process_frame` 的组合逻辑是**行为**，不是边界翻译。
// 放在 `namespace stereo` 里，C++ 调用者（测试、将来的 CLI、非 Apple 前端）不用经过
// C 边界；C 门面（`capi/pipeline.cpp`）只剩参数校验与 POD ↔ C++ 的换乘。
//
// 本阶段的约束：只做"按位置搬家"，组合顺序、缓冲复用策略、计时点、失败顺序
// 全部与改造前逐行一致（frame_test 的"手工 6 步 vs process_frame 逐位一致"是判据）。

#pragma once

#include <cstdint>

#include "stereo/analyze.hpp"
#include "stereo/geometry.hpp"
#include "stereo/rectify.hpp"
#include "stereo/stereo_context.h"   // stereo_config
#include "stereo/workspace.hpp"

namespace stereo {

/// 一帧全流水线的输入（C++ 视图；C 门面负责从 POD 换乘）。
struct FrameRequest {
    ImageView left;                          ///< 原始（未矫正）左/宽相机图，任意格式
    ImageView right;                         ///< 原始右/超广角图
    RectifyInput geometry;                   ///< 内参/外参/工作分辨率/flip/dy
    const stereo_config* config = nullptr;   ///< nullptr = 用默认配置
    float min_z = 0.0f, max_z = 0.0f;
    const float* center_rect = nullptr;      ///< 归一化 {x0,y0,x1,y1}；nullptr = 不写准星
    float center_q = 0.5f;
    AnalyzeParams analysis;                  ///< max_blobs / min_cells（内参会被结果覆盖）
};

/// 一帧全流水线的输出缓冲（全部由调用方准备；与 C 版字段一一对应）。
struct FrameOutput {
    float* depth = nullptr;        ///< 必填，work_width × work_height
    int depth_stride_px = 0;       ///< 像素跨度；<= 0 表示 = work_width
    float* disp = nullptr;         ///< 可选视差输出（矫正坐标系）
    int disp_stride_px = 0;
    BlobRecord* blobs = nullptr;   ///< 可选；nullptr 时用 workspace 的兜底缓冲
    int blob_capacity = 0;
    uint8_t* labels = nullptr;     ///< 可选；grid_width × grid_height
};

/// 结果里的标量部分。
struct FrameResult {
    Intrinsics k;                  ///< 矫正后的共同内参
    float focal_baseline = 0.0f;
    int dir = 1;
    AnalyzeResult analysis;        ///< 跳过分析时为零值
};

/// 各阶段耗时（毫秒）。
struct FrameTimings {
    double rectify = 0, match = 0, depth = 0, unrectify = 0, analyze = 0, total = 0;
};

/// 第 2..5 步：矫正 → 匹配 → 深度 → 回转 → 连通域分析。
///
/// `cfg` 是「当前配置」的引用：
///   * `req.config` 非空且与 `cfg` 不同 → 改用它，并让匹配缓冲失效（等价于
///     `stereo_configure` 把 W/H/D 清 0 的行为）；
///   * `req.config` 为空 → 用参数表的默认值（与改造前一致：即使此前 configure 过，
///     这里也会回到默认值）。
///
/// 返回 false 时 `*error`（若提供）指向静态字符串，可直接转给 `stereo_last_error`。
bool process_frame(Workspace& ws, stereo_config& cfg, const FrameRequest& req,
                   const FrameOutput& out, FrameResult& result,
                   FrameTimings* timings, const char** error);

}  // namespace stereo
