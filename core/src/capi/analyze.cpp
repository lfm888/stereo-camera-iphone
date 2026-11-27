// capi/analyze.cpp — 第 5 步：网格尺寸、准星采样、连通域分析
//
// 领域：stereo_analyze.h。scratch 全部来自 ctx->ws.analyze（按网格尺寸复用）。
// 逐像素/连通域本体在 stereo::analyze_depth / sample_quantile 里。

#include "capi_internal.hpp"

#include "stereo/sample.hpp"   // kSampleScratchFloats

#include <new>
#include <vector>

extern "C" {

int stereo_analyze_grid(int width, int height, int *grid_width, int *grid_height) {
    if (grid_width == nullptr || grid_height == nullptr) return -1;
    if (width <= 0 || height <= 0) return -1;
    stereo::analyze_grid_size(width, height, grid_width, grid_height);
    return 0;
}

int stereo_sample_depth(stereo_context *ctx, const float *depth, int depth_stride, int width,
                        int height, float x0, float y0, float x1, float y1, float q,
                        float *out_value) {
    if (ctx == nullptr || depth == nullptr || out_value == nullptr) return -1;
    if (width <= 0 || height <= 0) return -1;
    if (!(x1 > x0) || !(y1 > y0)) return -1;

    if (static_cast<int>(ctx->ws.analyze.samples.size()) < stereo::kSampleScratchFloats) {
        try {
            ctx->ws.analyze.samples.resize(
                static_cast<size_t>(stereo::kSampleScratchFloats));
        } catch (const std::bad_alloc &) {
            // 改造前这里没有 try/catch（会让 C++ 异常越过 C ABI）；OOM 是唯一的差异路径。
            return -1;
        }
    }
    const bool ok = stereo::sample_quantile(
        depth, depth_stride, width, height, x0, y0, x1, y1, q,
        ctx->ws.analyze.samples.data(), static_cast<int>(ctx->ws.analyze.samples.size()),
        out_value);
    // 区分"有效样本不足"（1）和"参数非法"（-1）：前者是正常的 UI 状态。
    return ok ? 0 : 1;
}

int stereo_analyze(stereo_context *ctx, const float *depth, int depth_stride, int width, int height,
                   const stereo_analyze_params *params, const float center_rect[4],
                   float center_q, stereo_blob *blobs, int blobs_capacity, unsigned char *labels,
                   stereo_analysis *out) {
    if (ctx == nullptr || depth == nullptr || params == nullptr || out == nullptr) return -1;
    if (blobs == nullptr || blobs_capacity <= 0) return -1;
    if (width <= 0 || height <= 0) return -1;

    int gw = 0, gh = 0;
    stereo::analyze_grid_size(width, height, &gw, &gh);
    if (gw <= 0 || gh <= 0) return capi::fail(ctx, "stereo_analyze: bad size");

    if (!ctx->ws.analyze.ensure(gw, gh)) {
        return capi::fail(ctx, "stereo_analyze: out of memory");
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
    if (!stereo::analyze_depth(depth, depth_stride, width, height, ap, center_rect, center_q,
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
    return 0;
}

} /* extern "C" */
