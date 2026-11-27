// stereo/workspace.hpp — 跨帧复用的缓冲区所有者（**不是算法原语**）
//
// 为什么单独一个头：算法文件（sgm/analyze/calibrate）刻意"不做动态分配"，缓冲区由调用方
// 传进来；而 C API 与 CPU 流水线都需要"尺寸不变就不重新分配"的稳态零分配行为。
// 把这份所有权集中在这里，两边共用同一套结构，不会各写一份 ensure 逻辑。
//
// 分组 = 领域（见 structure_v3_plan.md §4.4）：
//   MatchBuffers      匹配（luma / census / cost / sum / 视差）
//   AnalyzeBuffers    连通域分析 + 准星采样 scratch
//   CalibrateBuffers  自动标定 scratch
//   FrameBuffers      process_frame 的重采样图 / 视差 / 矫正坐标系深度 / 输入转换
//   Workspace         上面四个的组合；C API 的 context 与 C++ 的 process_frame 都用它
//
// **延迟分配**：所有 vector 默认空。只用到某个领域的前端不会为别的领域付内存
// （例如只要分析功能的调用方不会分配 640×360×D 的 cost/sum）。
//
// 注意：这些结构**不是线程安全的**。一个线程一个 Workspace，或自己加锁。

#pragma once

#include <cstdint>
#include <vector>

#include "stereo/analyze.hpp"
#include "stereo/calibrate.hpp"
#include "stereo/rectify.hpp"      // ImageView 转换
#include "stereo/stereo_types.h"  // stereo_image ↔ ImageView
#include "stereo/sgm.hpp"

namespace stereo {

/// 一次匹配的跨帧缓冲。ensure(w,h,d) 在尺寸不变时是空操作。
struct MatchBuffers {
    int W = 0, H = 0, D = 0;
    std::vector<float> luma_l, luma_r;
    std::vector<uint64_t> census_l, census_r;
    std::vector<uint8_t> cost;
    std::vector<uint16_t> sum;
    std::vector<float> disp_r, disp_l;

    bool ensure(int w, int h, int d);
    /// 参数变了 → 下一帧重新准备缓冲区（等价于改造前把 W/H/D 清 0）。
    void invalidate() { W = H = D = 0; }
    MatchScratch scratch();
};

/// 连通域分析 + 准星采样用的跨帧缓冲。
struct AnalyzeBuffers {
    int gw = 0, gh = 0;
    std::vector<float> grid;
    std::vector<int> labels, stack, remap;
    std::vector<unsigned> comps;
    std::vector<float> samples;

    bool ensure(int grid_w, int grid_h);
    AnalyzeScratch scratch();
};

/// 自动标定用的跨帧缓冲。
struct CalibrateBuffers {
    int W = 0, H = 0;
    std::vector<uint8_t> rect_l, rect_r;
    std::vector<float> luma_l, luma_r;
    std::vector<uint64_t> census_l, census_r;

    bool ensure(int w, int h);
    CalibrateScratch scratch();
};

/// `process_frame` 的中间图像与兜底面块缓冲。
struct FrameBuffers {
    int W = 0, H = 0;
    /// 两张矫正图（BGRA8，行跨度 = W*4）
    std::vector<uint8_t> rect_l, rect_r;
    /// 视差 + 矫正坐标系深度（W*H）
    std::vector<float> disp, depth_rect;
    /// 调用方只要 labels、不要 blobs 时的兜底面块记录
    std::vector<BlobRecord> blobs;
    /// 输入不是 BGRA8 时的转换缓冲（按**原始图**尺寸）
    int src_w = 0, src_h = 0;
    std::vector<uint8_t> in_l, in_r;

    bool ensure(int w, int h);
    bool ensure_input(int w, int h);
    bool ensure_blobs(int n);
};

/// 所有领域的缓冲组合。C API 的 context 持有一个，C++ 调用者也可以自己建一个。
struct Workspace {
    MatchBuffers match;
    AnalyzeBuffers analyze;
    CalibrateBuffers calibrate;
    FrameBuffers frame;
};

/// 把 C 的 `stereo_image` 看成算法层的 `ImageView` / `MutableImageView`。
/// 不做任何拷贝；生命周期由调用方保证。
ImageView view_of(const stereo_image& img);
MutableImageView mutable_view_of(stereo_image& img);

}  // namespace stereo
