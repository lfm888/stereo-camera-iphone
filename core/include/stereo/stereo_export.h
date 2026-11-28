//
//  stereo/stereo_export.h — 导出用的像素运算（不碰 ctx，纯计算）。
//  
//  点云 PLY、深度裁剪、16 位毫米打包、跨度拷贝。
//  约定：深度的 stride 是**像素**，彩色图 / 图像的 stride 是**字节**（见字段名与注释）。
//

#ifndef STEREO_EXPORT_H
#define STEREO_EXPORT_H

#include "stereo/stereo_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 导出用的像素运算（不碰 ctx，纯计算）                                    */
/* ------------------------------------------------------------------ */

/** 点云缓冲的**最坏情况**大小（粗估用）。 */
size_t stereo_point_cloud_capacity(int width, int height);

/**
 * 深度图 + 彩色图（BGRA8）→ ASCII PLY 点云（相机坐标系：x 右、y 下、z 前，单位米）。
 *
 * 典型用法（两遍）：
 *   1) out = NULL，拿到 out_bytes（精确尺寸）
 *   2) 按 out_bytes 分配，再调一次
 *
 * 失败时：缓冲不够返回 STEREO_BUFFER_TOO_SMALL，`*out_bytes` 写需求量；
 * 其余参数问题返回 STEREO_INVALID_ARGUMENT。成功返回 STEREO_OK。
 */
stereo_status stereo_point_cloud(const float *depth, int depth_stride_px, int width, int height,
                                 float fx, float fy, float cx, float cy, const uint8_t *rgba,
                                 int rgba_stride_bytes, int rgba_width, int rgba_height, char *out,
                                 size_t capacity, size_t *out_bytes);

/** 深度裁剪：非有限值、或超出 [min_z, max_z] 的写 0。src/dst 允许不同跨度（像素）。 */
stereo_status stereo_depth_filter(const float *src, int src_stride_px, float *dst,
                                  int dst_stride_px, int width, int height, float min_z,
                                  float max_z);

/** 深度 → 16 位毫米（导出 depth_mm.png 用），输入跨度以**像素**为单位。 */
stereo_status stereo_depth_to_u16_mm(const float *depth, int stride_px, int width, int height,
                                     uint16_t *out);

/** 带跨度（像素）→ 紧凑的 float 拷贝。 */
stereo_status stereo_copy_depth(const float *src, int src_stride_px, int width, int height,
                                float *dst);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* STEREO_EXPORT_H */
