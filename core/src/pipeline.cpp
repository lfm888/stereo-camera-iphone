// pipeline.cpp — 第 2..5 步 C++ 组合（从 stereo_process_frame 搬来）
//
// 搬家原则（structure_v3_plan.md §4.3）：**只改位置，不改顺序与数值**。
//   * 组合顺序：几何 → 重采样×2 → 匹配 → 视差→深度 → 回转 →（可选）连通域分析；
//   * 缓冲复用：全部走 `Workspace`，尺寸不变不重新分配；
//   * 失败顺序与错误串：与 C 版本逐条一致（C 门面写进 last_error）；
//   * 计时点：与改造前同一位置（rectify / match / depth / unrectify / analyze / total）。
//
// 2.00：第 4 步收成 process_disparity（一次入口、两步实现），process_frame 也走它，
// 于是 iOS 的 CPU 混合路径与 CLI/Qt 共用同一段代码。

#include "stereo/pipeline.hpp"

#include "stereo/color.hpp"
#include "stereo/params.hpp"   // config_default

#include <chrono>
#include <cstring>

namespace stereo {
namespace {

using PfClock = std::chrono::steady_clock;

inline double ms_since(PfClock::time_point t0) {
    return std::chrono::duration<double, std::milli>(PfClock::now() - t0).count();
}

inline void set_error(FrameError* error, FrameStatus status, const char* message) {
    if (error == nullptr) return;
    error->status = status;
    error->message = message;
}

}  // namespace

bool process_disparity(const float* disp, int disp_stride_px, int width, int height, float fB,
                       float min_z, float max_z, const Mat3& m_up, float* depth,
                       int depth_stride_px, float* depth_rect_scratch, double* depth_ms,
                       double* unrectify_ms) {
    if (disp == nullptr || depth == nullptr || depth_rect_scratch == nullptr) return false;
    if (width <= 0 || height <= 0) return false;
    if (disp_stride_px <= 0) disp_stride_px = width;
    if (depth_stride_px <= 0) depth_stride_px = width;

    const PfClock::time_point t_depth = PfClock::now();
    to_depth(disp, disp_stride_px, width, height, fB, min_z, max_z, depth_rect_scratch, width);
    if (depth_ms != nullptr) *depth_ms = ms_since(t_depth);

    const PfClock::time_point t_unrect = PfClock::now();
    const bool ok = unrectify_depth(depth_rect_scratch, width, height, width, m_up, depth,
                                    width, height, depth_stride_px);
    if (unrectify_ms != nullptr) *unrectify_ms = ms_since(t_unrect);
    return ok;
}

bool process_frame(Workspace& ws, stereo_config& cfg, const FrameRequest& req,
                   const FrameOutput& out, FrameResult& result,
                   FrameTimings* timings, FrameError* error) {
    auto fail = [error](FrameStatus status, const char* msg) {
        set_error(error, status, msg);
        return false;
    };

    const int W = req.geometry.work_width;
    const int H = req.geometry.work_height;
    if (W <= 0 || H <= 0) {
        return fail(FrameStatus::invalid_argument, "process_frame: bad work size");
    }

    const PfClock::time_point t_all = PfClock::now();

    // ---- 第 2 步的几何 ----
    RectifyOutput g;
    if (!rectify_geometry(req.geometry, &g)) {
        return fail(FrameStatus::invalid_argument, "process_frame: bad geometry");
    }

    if (!ws.frame.ensure(W, H)) {
        return fail(FrameStatus::out_of_memory, "process_frame: out of memory");
    }

    // 匹配参数：与当前配置相同就跳过（否则每帧都会重分配缓冲区）。
    // 与改造前的 stereo_configure 一样：只有"确实不同"时才校验并应用。
    stereo_config want{};
    if (req.config != nullptr) {
        want = *req.config;
    } else {
        config_default(want);
    }
    if (std::memcmp(&want, &cfg, sizeof want) != 0) {
        if (want.max_disp < 4 || want.max_disp > 96) {
            return fail(FrameStatus::invalid_argument, "process_frame: bad config");
        }
        if (want.p1 < 0 || want.p2 < 0) {
            return fail(FrameStatus::invalid_argument, "process_frame: bad config");
        }
        cfg = want;
        ws.match.invalidate();
    }

    // ---- 第 2 步：两路重采样 ----
    const PfClock::time_point t_rect = PfClock::now();

    // rectify_image 不做格式转换（见 rectify.hpp），所以这里先统一成 BGRA8。
    // 相机原始帧就是 BGRA8，这条分支只在别的平台喂 RGB/灰度图时才走。
    // 必须在函数作用域里声明：left/right 会指向它们，不能让它俩随 if 块结束而销毁。
    ImageView sl{};
    ImageView sr{};
    const ImageView* left = &req.left;
    const ImageView* right = &req.right;
    if (left->format != kFormatBGRA8 || right->format != kFormatBGRA8) {
        if (left->width != right->width || left->height != right->height) {
            return fail(FrameStatus::invalid_argument,
                        "process_frame: left/right size mismatch");
        }
        if (!ws.frame.ensure_input(left->width, left->height)) {
            return fail(FrameStatus::out_of_memory, "process_frame: out of memory");
        }
        sl = *left;
        sr = *right;
        sl.data = ws.frame.in_l.data();
        sl.stride = left->width * 4;
        sl.format = kFormatBGRA8;
        sr.data = ws.frame.in_r.data();
        sr.stride = right->width * 4;
        sr.format = kFormatBGRA8;
        if (!to_bgra8(*left, ws.frame.in_l.data())) {
            return fail(FrameStatus::unsupported_format,
                        "process_frame: unsupported left format");
        }
        if (!to_bgra8(*right, ws.frame.in_r.data())) {
            return fail(FrameStatus::unsupported_format,
                        "process_frame: unsupported right format");
        }
        left = &sl;
        right = &sr;
    }

    MutableImageView rl{};
    rl.data = ws.frame.rect_l.data();
    rl.width = W;
    rl.height = H;
    rl.stride = W * 4;
    rl.format = kFormatBGRA8;
    MutableImageView rr = rl;
    rr.data = ws.frame.rect_r.data();

    if (!rectify_image(*left, g.to_wide_image, rl)) {
        return fail(FrameStatus::internal_error, "process_frame: rectify left failed");
    }
    if (!rectify_image(*right, g.to_ultra_image, rr)) {
        return fail(FrameStatus::internal_error, "process_frame: rectify right failed");
    }
    if (timings != nullptr) timings->rectify = ms_since(t_rect);

    // ---- 第 3 步：匹配 ----
    const PfClock::time_point t_match = PfClock::now();

    const int D = cfg.max_disp;
    if (!ws.match.ensure(W, H, D)) {
        return fail(FrameStatus::out_of_memory, "process_frame: out of memory");
    }

    // 只读视图（to_luma 只需要读）：MutableImageView 与 ImageView 是不同类型，显式构造。
    const ImageView rlv{rl.data, rl.width, rl.height, rl.stride, rl.format};
    const ImageView rrv{rr.data, rr.width, rr.height, rr.stride, rr.format};
    if (!to_luma(rlv, ws.match.luma_l.data()) || !to_luma(rrv, ws.match.luma_r.data())) {
        return fail(FrameStatus::unsupported_format, "process_frame: match failed");
    }

    SgmParams p;
    p.W = W;
    p.H = H;
    p.D = D;
    p.dir = g.dir;
    p.P1 = static_cast<unsigned>(cfg.p1);
    p.P2 = static_cast<unsigned>(cfg.p2);
    p.uniqueness = cfg.uniqueness;
    p.lrCheck = cfg.lr_check;
    match_luma(ws.match.luma_l.data(), ws.match.luma_r.data(), p, ws.match.scratch(),
               ws.frame.disp.data(), W);
    if (timings != nullptr) timings->match = ms_since(t_match);

    // ---- 第 4 步：视差 → 深度 → 回转（process_disparity：与 iOS 的 CPU 路径同一入口）----
    double depth_ms = 0.0, unrectify_ms = 0.0;
    if (!process_disparity(ws.frame.disp.data(), W, W, H, g.focal_baseline, req.min_z, req.max_z,
                           g.to_rect_image, out.depth, out.depth_stride_px,
                           ws.frame.depth_rect.data(), &depth_ms, &unrectify_ms)) {
        return fail(FrameStatus::internal_error, "process_frame: unrectify failed");
    }
    if (timings != nullptr) {
        timings->depth = depth_ms;
        timings->unrectify = unrectify_ms;
    }

    // ---- 视差输出（可选）----
    if (out.disp != nullptr) {
        const int ds = out.disp_stride_px > 0 ? out.disp_stride_px : W;
        for (int y = 0; y < H; ++y) {
            std::memcpy(out.disp + static_cast<size_t>(y) * static_cast<size_t>(ds),
                        ws.frame.disp.data() + static_cast<size_t>(y) * static_cast<size_t>(W),
                        static_cast<size_t>(W) * sizeof(float));
        }
    }

    // ---- 第 5 步：连通域分析（可选；在**回转后**的深度图上，与 App 的做法一致）----
    // blobs 与 labels 都不给 = 调用方只要深度，跳过这一步（省掉降采样+连通域的开销）。
    if (out.blobs != nullptr || out.labels != nullptr) {
        const PfClock::time_point t_analyze = PfClock::now();
        AnalyzeParams ap = req.analysis;
        ap.fx = g.K.fx;   // 反投影用矫正后的内参（MUp 保持像素尺度，所以这就是对的）
        ap.fy = g.K.fy;
        ap.cx = g.K.cx;
        ap.cy = g.K.cy;
        if (ap.max_blobs <= 0) ap.max_blobs = 24;
        if (ap.min_cells <= 0) ap.min_cells = 40;

        // 只要 labels、不要 blobs 时，用 workspace 里的兜底数组承接面块记录
        BlobRecord* blobs = out.blobs;
        int capacity = out.blob_capacity;
        if (blobs == nullptr || capacity <= 0) {
            if (!ws.frame.ensure_blobs(ap.max_blobs)) {
                return fail(FrameStatus::out_of_memory, "process_frame: out of memory");
            }
            blobs = ws.frame.blobs.data();
            capacity = static_cast<int>(ws.frame.blobs.size());
        }

        int gw = 0, gh = 0;
        analyze_grid_size(W, H, &gw, &gh);
        if (!ws.analyze.ensure(gw, gh)) {
            return fail(FrameStatus::out_of_memory, "process_frame: out of memory");
        }

        const float fallback_rect[4] = {0.0f, 0.0f, 1.0f, 1.0f};
        const float* center_rect = req.center_rect != nullptr ? req.center_rect : fallback_rect;
        const int depth_stride = out.depth_stride_px > 0 ? out.depth_stride_px : W;
        AnalyzeScratch scratch = ws.analyze.scratch();
        if (!analyze_depth(out.depth, depth_stride, W, H, ap, center_rect, req.center_q,
                           blobs, capacity, out.labels, &scratch, &result.analysis)) {
            return fail(FrameStatus::internal_error, "process_frame: analyze failed");
        }
        if (timings != nullptr) timings->analyze = ms_since(t_analyze);
    } else {
        result.analysis = AnalyzeResult{};
    }

    // ---- 输出几何信息 ----
    result.k = g.K;
    result.focal_baseline = g.focal_baseline;
    result.dir = g.dir;

    if (timings != nullptr) timings->total = ms_since(t_all);
    return true;
}

}  // namespace stereo
