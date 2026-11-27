//
//  stereo/stereo_match.h — 第 3/4 步：立体匹配与视差转深度。
//  
//  输入是两幅**已矫正**的图（iOS 的 GPU 路径自己算视差，不用这里的图接口）。
//  ctx 只作为缓冲区宿主；同一 ctx 不要并发调用。
//

#ifndef STEREO_MATCH_H
#define STEREO_MATCH_H

#include "stereo/stereo_types.h"
#include "stereo/stereo_context.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 算法                                                                */
/* ------------------------------------------------------------------ */

/**
 * 半全局匹配（Census 7x7 + SGM 4 路径 + 唯一性 + 左右一致性 + 抛物亚像素）。
 *
 * left / right 必须是**已矫正**的一对图，且 width/height 相同；
 * dir = +1 表示右图里的同名点在左图的右侧（视差为正），-1 反之。
 *
 * disp 是 float32 的 width*height 矩阵，行跨度 disp_stride 以**像素**为单位
 * （传 0 表示等于 width）。无效像素写 0。
 *
 * 返回 0 成功，非 0 失败（看 stereo_last_error）。
 */
int stereo_match(stereo_context *ctx,
                 const stereo_image *left,
                 const stereo_image *right,
                 int dir,
                 float *disp,
                 int disp_stride);

/**
 * 视差 → 深度：3x3 中值（在视差域做）+ 范围截断。无效像素写 0。
 * fB 的单位是「像素·米」（= f_px * baseline_m），所以输出的 Z 是米。
 */
int stereo_disparity_to_depth(stereo_context *ctx,
                              const float *disp,
                              int disp_stride,
                              int width,
                              int height,
                              float fB,
                              float min_z,
                              float max_z,
                              float *depth,
                              int depth_stride);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* STEREO_MATCH_H */
