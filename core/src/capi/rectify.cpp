// capi/rectify.cpp — 第 2 步：矫正几何 / 重采样 / 深度回转
//
// 领域：stereo_rectify.h。这三个函数都不碰 ctx（无状态、天然线程安全），
// 所以本文件里没有 fail()、也没有 context 访问。

#include "capi_internal.hpp"

extern "C" {

stereo_status stereo_rectify_geometry(const stereo_rectify_input *in,
                                      stereo_rectify_output *out) {
    if (in == nullptr || out == nullptr) return STEREO_INVALID_ARGUMENT;

    stereo::RectifyInput ri = capi::to_rectify_input(*in);

    stereo::RectifyOutput g;
    if (!stereo::rectify_geometry(ri, &g)) return STEREO_INVALID_ARGUMENT;

    capi::from_mat3(g.to_wide_image, out->to_wide_image);
    capi::from_mat3(g.to_ultra_image, out->to_ultra_image);
    capi::from_mat3(g.to_rect_image, out->to_rect_image);
    out->k.fx = g.K.fx;
    out->k.fy = g.K.fy;
    out->k.cx = g.K.cx;
    out->k.cy = g.K.cy;
    out->dir = g.dir;
    out->focal_baseline = g.focal_baseline;
    out->baseline_mm = g.baseline_mm;
    out->baseline_deg = g.baseline_deg;
    capi::from_mat3(g.Rl, out->rl);
    capi::from_mat3(g.Rr, out->rr);
    for (int i = 0; i < 12; ++i) {
        out->p_left[i] = g.P_left[i];
        out->p_right[i] = g.P_right[i];
    }
    for (int i = 0; i < 16; ++i) out->q[i] = g.Q[i];
    return STEREO_OK;
}

stereo_status stereo_rectify_image(const stereo_image *src, const float m[9],
                                   stereo_image *dst) {
    if (!capi::valid_image(src) || !capi::valid_image(dst) || m == nullptr) {
        return STEREO_INVALID_ARGUMENT;
    }
    // dst->data 会被写入（头文件已说明调用方必须传可写缓冲）。
    return stereo::rectify_image(stereo::view_of(*src), capi::to_mat3(m),
                                 stereo::mutable_view_of(*dst))
               ? STEREO_OK
               : STEREO_INVALID_ARGUMENT;
}

stereo_status stereo_unrectify_depth(const float *src, int src_stride_px, int src_width,
                                     int src_height, const float m[9], float *dst,
                                     int dst_stride_px, int dst_width, int dst_height) {
    if (src == nullptr || dst == nullptr || m == nullptr) return STEREO_INVALID_ARGUMENT;
    return stereo::unrectify_depth(src, src_width, src_height, src_stride_px, capi::to_mat3(m),
                                   dst, dst_width, dst_height, dst_stride_px)
               ? STEREO_OK
               : STEREO_INVALID_ARGUMENT;
}

} /* extern "C" */
