// capi/calibrate.cpp — 第 1 步的自动化：自动标定搜索
//
// 领域：stereo_calibrate.h。CPU 打分版用 ctx->ws.calibrate 的 scratch；
// 平台打分版（stereo_calibrate_search）无状态。

#include "capi_internal.hpp"

extern "C" {

stereo_status stereo_calibrate(stereo_context *ctx, const stereo_rectify_input *geometry,
                               const stereo_image *wide, const stereo_image *ultra,
                               const stereo_calibrate_params *params,
                               stereo_calibrate_result *out) {
    if (ctx == nullptr || geometry == nullptr || params == nullptr || out == nullptr) {
        return STEREO_INVALID_ARGUMENT;
    }
    if (!capi::valid_image(wide) || !capi::valid_image(ultra)) {
        return capi::fail(ctx, "stereo_calibrate: bad image");
    }

    stereo::RectifyInput ri = capi::to_rectify_input(*geometry);

    const int W = geometry->work_width;
    const int H = geometry->work_height;
    if (W <= 0 || H <= 0) return capi::fail(ctx, "stereo_calibrate: bad work size");

    if (!ctx->ws.calibrate.ensure(W, H)) {
        return capi::fail(ctx, "stereo_calibrate: out of memory", STEREO_OUT_OF_MEMORY);
    }

    stereo::CalibrateParams cp;
    cp.max_disp = params->max_disp;
    cp.pixel_step = params->pixel_step;
    cp.flip0 = params->flip0;
    cp.dy0 = params->dy0;

    stereo::CalibrateScratch sc = ctx->ws.calibrate.scratch();

    stereo::CalibrateResult res;
    if (!stereo::calibrate_rectification(ri, stereo::view_of(*wide), stereo::view_of(*ultra), cp,
                                         &sc, &res)) {
        return capi::fail(ctx, "stereo_calibrate: search failed", STEREO_INTERNAL_ERROR);
    }
    out->flip = res.flip;
    out->dy = res.dy;
    out->score = res.score;
    out->evaluations = res.evaluations;
    ctx->err[0] = '\0';
    return STEREO_OK;
}

stereo_status stereo_calibrate_search(stereo_score_fn score, void *user,
                                      const stereo_calibrate_params *params,
                                      stereo_calibrate_result *out) {
    if (score == nullptr || params == nullptr || out == nullptr) return STEREO_INVALID_ARGUMENT;
    stereo::CalibrateParams cp;
    cp.max_disp = params->max_disp;
    cp.pixel_step = params->pixel_step;
    cp.flip0 = params->flip0;
    cp.dy0 = params->dy0;
    stereo::CalibrateResult res;
    if (!stereo::calibrate_search(score, user, cp, &res)) return STEREO_INTERNAL_ERROR;
    out->flip = res.flip;
    out->dy = res.dy;
    out->score = res.score;
    out->evaluations = res.evaluations;
    return STEREO_OK;
}

} /* extern "C" */
