// capi/export.cpp — 导出用的像素运算（无状态）
//
// 领域：stereo_export.h。全部是"换乘 + 调 stereo::"；点云/裁剪/打包的本体在
// core/src/cloud.cpp。

#include "capi_internal.hpp"

#include "stereo/cloud.hpp"

extern "C" {

size_t stereo_point_cloud_capacity(int width, int height) {
    return stereo::point_cloud_ply_capacity(width, height);
}

stereo_status stereo_point_cloud(const float *depth, int depth_stride_px, int width, int height,
                                 float fx, float fy, float cx, float cy, const uint8_t *rgba,
                                 int rgba_stride_bytes, int rgba_width, int rgba_height, char *out,
                                 size_t capacity, size_t *out_bytes) {
    stereo::Intrinsics k;
    k.fx = fx;
    k.fy = fy;
    k.cx = cx;
    k.cy = cy;
    if (stereo::point_cloud_ply(depth, depth_stride_px, width, height, k, rgba,
                                rgba_stride_bytes, rgba_width, rgba_height, out, capacity,
                                out_bytes)) {
        return STEREO_OK;
    }
    // 两遍调用：out == nullptr 时"失败"只会是参数问题；给了缓冲则是容量不够。
    return out == nullptr ? STEREO_INVALID_ARGUMENT : STEREO_BUFFER_TOO_SMALL;
}

stereo_status stereo_depth_filter(const float *src, int src_stride_px, float *dst,
                                  int dst_stride_px, int width, int height, float min_z,
                                  float max_z) {
    return stereo::depth_filter(src, src_stride_px, dst, dst_stride_px, width, height, min_z,
                                max_z)
               ? STEREO_OK
               : STEREO_INVALID_ARGUMENT;
}

stereo_status stereo_depth_to_u16_mm(const float *depth, int stride_px, int width, int height,
                                     uint16_t *out) {
    return stereo::depth_to_u16_mm(depth, stride_px, width, height, out) ? STEREO_OK
                                                                        : STEREO_INVALID_ARGUMENT;
}

stereo_status stereo_copy_depth(const float *src, int src_stride_px, int width, int height,
                                float *dst) {
    return stereo::copy_depth(src, src_stride_px, width, height, dst) ? STEREO_OK
                                                                     : STEREO_INVALID_ARGUMENT;
}

} /* extern "C" */
