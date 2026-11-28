// capi/pipeline.cpp — 第 2..5 步全流水线 + 视差→深度→回转的**翻译层**
//
// 领域：stereo_pipeline.h。本体在 core/src/pipeline.cpp；这里只做：
// 校验入参 → POD → C++ 结构 → 调用 → 结果写回 POD。不出现像素循环，也不出现算法。

#include "capi_internal.hpp"

#include "stereo/pipeline.hpp"

namespace {

stereo_status map_status(stereo::FrameStatus status) {
    switch (status) {
        case stereo::FrameStatus::ok: return STEREO_OK;
        case stereo::FrameStatus::invalid_argument: return STEREO_INVALID_ARGUMENT;
        case stereo::FrameStatus::out_of_memory: return STEREO_OUT_OF_MEMORY;
        case stereo::FrameStatus::unsupported_format: return STEREO_UNSUPPORTED_FORMAT;
        case stereo::FrameStatus::internal_error: return STEREO_INTERNAL_ERROR;
    }
    return STEREO_INTERNAL_ERROR;
}

}  // namespace

extern "C" {

stereo_status stereo_process_disparity(const float *disp, int disp_stride_px, int width,
                                       int height, float fB, float min_z, float max_z,
                                       const float m_up[9], float *depth, int depth_stride_px,
                                       float *depth_rect_scratch) {
    if (disp == nullptr || depth == nullptr || m_up == nullptr || depth_rect_scratch == nullptr) {
        return STEREO_INVALID_ARGUMENT;
    }
    if (width <= 0 || height <= 0) return STEREO_INVALID_ARGUMENT;
    return stereo::process_disparity(disp, disp_stride_px, width, height, fB, min_z, max_z,
                                     capi::to_mat3(m_up), depth, depth_stride_px,
                                     depth_rect_scratch)
               ? STEREO_OK
               : STEREO_INVALID_ARGUMENT;
}

stereo_status stereo_process_frame(stereo_context *ctx, const stereo_frame_input *in,
                                   stereo_frame_output *out, stereo_frame_timings *timings) {
    if (ctx == nullptr) return STEREO_INVALID_ARGUMENT;
    if (in == nullptr || out == nullptr) {
        return capi::fail(ctx, "process_frame: in/out is NULL");
    }
    if (!capi::valid_image(in->left) || !capi::valid_image(in->right)) {
        return capi::fail(ctx, "process_frame: left/right image invalid");
    }
    if (in->geometry == nullptr) return capi::fail(ctx, "process_frame: geometry is NULL");
    if (out->depth == nullptr) return capi::fail(ctx, "process_frame: depth buffer is NULL");

    const int W = in->geometry->work_width;
    const int H = in->geometry->work_height;
    if (W <= 0 || H <= 0) return capi::fail(ctx, "process_frame: bad work size");

    if (timings != nullptr) *timings = stereo_frame_timings{};

    // ---- POD → C++ ----
    stereo::FrameRequest req;
    req.left = stereo::view_of(*in->left);
    req.right = stereo::view_of(*in->right);
    req.geometry = capi::to_rectify_input(*in->geometry);
    req.config = in->config;
    req.min_z = in->min_z;
    req.max_z = in->max_z;
    req.center_rect = in->center_rect;
    req.center_q = in->center_q;
    req.analysis.fx = in->analysis.fx;
    req.analysis.fy = in->analysis.fy;
    req.analysis.cx = in->analysis.cx;
    req.analysis.cy = in->analysis.cy;
    req.analysis.max_blobs = in->analysis.max_blobs;
    req.analysis.min_cells = in->analysis.min_cells;

    stereo::FrameOutput fo;
    fo.depth = out->depth;
    fo.depth_stride_px = out->depth_stride_px;
    fo.disp = out->disp;
    fo.disp_stride_px = out->disp_stride_px;
    fo.labels = out->labels;

    // blobs：C 侧是 stereo_blob[]，C++ 侧写 BlobRecord[]；需要时先落到 workspace 再拷回去。
    const bool want_blobs = out->blobs != nullptr && out->blob_capacity > 0;
    if (want_blobs) {
        if (!ctx->ws.frame.ensure_blobs(out->blob_capacity)) {
            return capi::fail(ctx, "process_frame: out of memory", STEREO_OUT_OF_MEMORY);
        }
        fo.blobs = ctx->ws.frame.blobs.data();
        fo.blob_capacity = out->blob_capacity;
    }

    stereo::FrameResult result;
    stereo::FrameTimings local_timing;
    stereo::FrameTimings *tp = timings != nullptr ? &local_timing : nullptr;
    stereo::FrameError error;
    if (!stereo::process_frame(ctx->ws, ctx->cfg, req, fo, result, tp, &error)) {
        return capi::fail(ctx, error.message, map_status(error.status));
    }

    // ---- C++ → POD ----
    if (want_blobs) {
        capi::copy_blobs(ctx->ws.frame.blobs.data(), result.analysis.blob_count, out->blobs);
    }
    out->k.fx = result.k.fx;
    out->k.fy = result.k.fy;
    out->k.cx = result.k.cx;
    out->k.cy = result.k.cy;
    out->focal_baseline = result.focal_baseline;
    out->direction = result.dir;
    out->analysis.blob_count = result.analysis.blob_count;
    out->analysis.grid_width = result.analysis.grid_width;
    out->analysis.grid_height = result.analysis.grid_height;
    out->analysis.center = result.analysis.center;
    out->analysis.has_center = result.analysis.has_center ? 1 : 0;

    if (timings != nullptr) {
        timings->rectify = local_timing.rectify;
        timings->match = local_timing.match;
        timings->depth = local_timing.depth;
        timings->unrectify = local_timing.unrectify;
        timings->analyze = local_timing.analyze;
        timings->total = local_timing.total;
    }
    ctx->err[0] = '\0';
    return STEREO_OK;
}

} /* extern "C" */
