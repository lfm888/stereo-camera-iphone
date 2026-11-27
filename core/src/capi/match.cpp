// capi/match.cpp — 第 3/4 步：立体匹配与视差转深度
//
// 领域：stereo_match.h。缓冲区来自 ctx->ws.match（尺寸不变时复用）；
// 逐像素的亮度化与匹配本体都在 stereo:: 里，这里只有校验与换乘。

#include "capi_internal.hpp"

extern "C" {

int stereo_match(stereo_context *ctx,
                 const stereo_image *left,
                 const stereo_image *right,
                 int dir,
                 float *disp,
                 int disp_stride) {
    if (ctx == nullptr) return -1;
    if (disp == nullptr) return capi::fail(ctx, "stereo_match: disp is NULL");
    if (!capi::usable(left) || !capi::usable(right)) {
        return capi::fail(ctx, "stereo_match: bad left/right image");
    }
    if (left->width != right->width || left->height != right->height) {
        return capi::fail(ctx, "stereo_match: left/right size mismatch");
    }
    if (dir != 1 && dir != -1) return capi::fail(ctx, "stereo_match: dir must be +1 or -1");

    const int W = left->width, H = left->height, D = ctx->cfg.max_disp;
    if (disp_stride <= 0) disp_stride = W;
    if (!ctx->ws.match.ensure(W, H, D)) return capi::fail(ctx, "stereo_match: out of memory");

    if (!stereo::to_luma(stereo::view_of(*left), ctx->ws.match.luma_l.data())) {
        return capi::fail(ctx, "stereo_match: bad left format");
    }
    if (!stereo::to_luma(stereo::view_of(*right), ctx->ws.match.luma_r.data())) {
        return capi::fail(ctx, "stereo_match: bad right format");
    }

    stereo::SgmParams p;
    p.W = W;
    p.H = H;
    p.D = D;
    p.dir = dir;
    p.P1 = static_cast<unsigned>(ctx->cfg.p1);
    p.P2 = static_cast<unsigned>(ctx->cfg.p2);
    p.uniqueness = ctx->cfg.uniqueness;
    p.lrCheck = ctx->cfg.lr_check;

    stereo::match_luma(ctx->ws.match.luma_l.data(), ctx->ws.match.luma_r.data(), p,
                       ctx->ws.match.scratch(), disp, disp_stride);
    ctx->err[0] = '\0';
    return 0;
}

int stereo_disparity_to_depth(stereo_context *ctx,
                              const float *disp,
                              int disp_stride,
                              int width,
                              int height,
                              float fB,
                              float min_z,
                              float max_z,
                              float *depth,
                              int depth_stride) {
    if (ctx == nullptr) return -1;
    if (disp == nullptr || depth == nullptr) return capi::fail(ctx, "to_depth: NULL buffer");
    if (width <= 0 || height <= 0) return capi::fail(ctx, "to_depth: bad size");
    if (disp_stride <= 0) disp_stride = width;
    if (depth_stride <= 0) depth_stride = width;

    // to_depth 支持行跨度，所以不需要搬进紧凑缓冲区。
    stereo::to_depth(disp, disp_stride, width, height, fB, min_z, max_z, depth, depth_stride);
    return 0;
}

} /* extern "C" */
