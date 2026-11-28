// capi/analyze.cpp — 第 5 步：网格尺寸、准星采样、连通域分析
//
// 领域：stereo_analyze.h。
//   * `stereo_analyze_grid` / `stereo_sample_depth` 无状态（2.00 起 sample 也不需要 ctx，
//     scratch 只有 8 KB，放栈上）；
//   * `stereo_analyze` 的 scratch 来自 ctx->ws.analyze（按网格尺寸复用）。

#include "capi_internal.hpp"

#include "stereo/sample.hpp"   // kSampleScratchFloats

#include <new>
#include <vector>

extern "C" {

stereo_status stereo_analyze_grid(int width, int height, int *grid_width, int *grid_height) {
    if (grid_width == nullptr || grid_height == nullptr) return STEREO_INVALID_ARGUMENT;
    if (width <= 0 || height <= 0) return STEREO_INVALID_ARGUMENT;
    stereo::analyze_grid_size(width, height, grid_width, grid_height);
    return STEREO_OK;
}

stereo_status stereo_sample_depth(const float *depth, int depth_stride_px, int width, int height,
                                  float x0, float y0, float x1, float y1, float q,
                                  float *out_value, int *out_has_value) {
    if (out_has_value != nullptr) *out_has_value = 0;
    if (depth == nullptr || out_value == nullptr) return STEREO_INVALID_ARGUMENT;
    if (width <= 0 || height <= 0) return STEREO_INVALID_ARGUMENT;
    if (!(x1 > x0) || !(y1 > y0)) return STEREO_INVALID_ARGUMENT;

    // 2.00：scratch 放栈上（2048 个 float = 8 KB），不再需要 ctx。
    float scratch[stereo::kSampleScratchFloats];
    const bool ok = stereo::sample_quantile(depth, depth_stride_px, width, height, x0, y0, x1, y1,
                                            q, scratch, stereo::kSampleScratchFloats, out_value);
    // "有效样本不足"是正常结果：返回 OK，用 has_value 表达。
    if (out_has_value != nullptr) *out_has_value = ok ? 1 : 0;
    return STEREO_OK;
}

stereo_status stereo_analyze(stereo_context *ctx, const float *depth, int depth_stride_px,
                             int width, int height, const stereo_analyze_params *params,
                             const float center_rect[4], float center_q, stereo_blob *blobs,
                             int blobs_capacity, unsigned char *labels, stereo_analysis *out) {
    if (ctx == nullptr || depth == nullptr || params == nullptr || out == nullptr) {
        return STEREO_INVALID_ARGUMENT;
    }
    if (blobs == nullptr || blobs_capacity <= 0) return STEREO_INVALID_ARGUMENT;
    if (width <= 0 || height <= 0) return STEREO_INVALID_ARGUMENT;

    int gw = 0, gh = 0;
    stereo::analyze_grid_size(width, height, &gw, &gh);
    if (gw <= 0 || gh <= 0) return capi::fail(ctx, "stereo_analyze: bad size");

    if (!ctx->ws.analyze.ensure(gw, gh)) {
        return capi::fail(ctx, "stereo_analyze: out of memory", STEREO_OUT_OF_MEMORY);
    }

    stereo::AnalyzeParams ap;
    ap.fx = params->fx;
    ap.fy = params->fy;
    ap.cx = params->cx;
    ap.cy = params->cy;
    ap.max_blobs = params->max_blobs;
    ap.min_cells = params->min_cells;

    stereo::AnalyzeScratch sc = ctx->ws.analyze.scratch();

    // POD 翻译：stereo_blob 与 stereo::BlobRecord 布局一致，但仍然逐字段拷。
    std::vector<stereo::BlobRecord> recs(static_cast<size_t>(blobs_capacity));
    stereo::AnalyzeResult res;
    if (!stereo::analyze_depth(depth, depth_stride_px, width, height, ap, center_rect, center_q,
                               recs.data(), blobs_capacity, labels, &sc, &res)) {
        return capi::fail(ctx, "stereo_analyze: bad arguments or buffer");
    }
    capi::copy_blobs(recs.data(), res.blob_count, blobs);
    out->blob_count = res.blob_count;
    out->grid_width = res.grid_width;
    out->grid_height = res.grid_height;
    out->center = res.center;
    out->has_center = res.has_center ? 1 : 0;
    ctx->err[0] = '\0';
    return STEREO_OK;
}

} /* extern "C" */
