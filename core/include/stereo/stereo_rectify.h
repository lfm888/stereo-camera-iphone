//
//  stereo/stereo_rectify.h — 第 2 步：极线矫正几何 + 重采样 / 深度回转。
//  
//  三个函数都**不碰 ctx**，纯输入→输出，天然线程安全。
//  它们与 Shaders.metal 的 rectify / unrectifyDepth 逐条对齐（给非 Apple 平台用）。
//

#ifndef STEREO_RECTIFY_H
#define STEREO_RECTIFY_H

#include "stereo/stereo_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 第 2 步：极线矫正（几何）                                            */
/*                                                                     */
/* 下面三个函数**不碰 ctx**，纯粹是输入→输出的计算，所以天然线程安全、 */
/* 无生命周期问题。它们对应 Shaders.metal 的 rectify / unrectifyDepth  */
/* kernel，给没有 Metal 的平台（Windows / Linux）用。                    */
/* ------------------------------------------------------------------ */

/** 极线矫正的输入：第 1 步（平台侧）读到的内参与外参，原样搬进来。 */
typedef struct stereo_rectify_input {
    stereo_intrinsics wide;  /**< 宽相机内参 */
    stereo_intrinsics ultra; /**< 超广角内参 */
    float R[9];              /**< 行主序 3x3：X_wide = R * X_ultra + t */
    float t[3];              /**< 与基线同单位（AVFoundation 给毫米） */
    int src_width;           /**< 原始图宽，用来把内参缩放到工作分辨率 */
    int work_width;          /**< 工作分辨率宽（如 640） */
    int work_height;         /**< 工作分辨率高（如 360） */
    int flip;                /**< 非 0：交换基线方向（外参取逆） */
    float dy;                /**< 矫正后行偏移（工作分辨率像素，可亚像素） */
} stereo_rectify_input;

/** 极线矫正的输出。三个矩阵都是齐次像素坐标之间的单应，用之前除以第三分量。 */
typedef struct stereo_rectify_output {
    float to_wide_image[9];  /**< 矫正后左图像素 → 宽相机原始像素（MW） */
    float to_ultra_image[9]; /**< 矫正后左图像素 → 超广角原始像素（MU） */
    float to_rect_image[9];  /**< 宽相机像素 → 矫正后图像素（MUp，深度回转用） */
    stereo_intrinsics k;     /**< 矫正后的共同内参 */
    int dir;                 /**< +1 / -1：同名点在左图的哪一侧 */
    float focal_baseline;    /**< fx * baseline(米)，配合 to_depth 用 */
    float baseline_mm;
    float baseline_deg;      /**< 基线相对宽相机 x 轴的角度（度） */
    float rl[9];             /**< 左矫正旋转（本管线里 rl == rr） */
    float rr[9];             /**< 右矫正旋转 */
    float p_left[12];        /**< 3x4 投影矩阵 K[I|0]，行主序 */
    float p_right[12];       /**< 3x4 投影矩阵 K[I|(-dir*b,0,0)]，行主序 */
    float q[16];             /**< 4x4：把 (x, y, 非负视差, 1) 齐次映射成矫正后 3D（米） */
} stereo_rectify_output;

/**
 * 解极线矫正。成功返回 0；参数非法返回非 0（没有 ctx，所以没有错误串）。
 *
 * 前提（用错了不会报错，只会算错）：
 *   * 两路图**已去畸变**（iPhone 的 constituent 相机输出满足）；
 *   * 两路同尺寸（src_width 共用）。
 */
stereo_status stereo_rectify_geometry(const stereo_rectify_input *in, stereo_rectify_output *out);

/**
 * 用单应 m 把 src 重采样到 dst。
 *
 * dst 可以与 src 尺寸不同，但**格式（包括通道顺序）必须与 src 一致**：
 * 这里按字节处理通道，不做格式转换。
 *
 * 与 Metal 的 rectify kernel 等价：4 次 (0.25|0.75)² 子采样 + 双线性（越界补 0）取平均。
 * m 是 stereo_rectify_output 里的 to_wide_image / to_ultra_image。
 *
 * 注意：stereo_image.data 声明为 const（源图的语义），dst->data 会被写入，
 * 调用方必须传一块可写的缓冲区。
 *
 * 成功返回 0。
 */
stereo_status stereo_rectify_image(const stereo_image *src, const float m[9], stereo_image *dst);

/**
 * 深度图回转：把**矫正后**坐标系的深度映射回**宽相机**坐标系（unrectifyDepth）。
 *
 * src/dst 都是 float32，行跨度以**像素**为单位（传 0 表示等于 width）。
 * m 是 stereo_rectify_output.to_rect_image。
 * 语义与 kernel 一致：目标像素 +0.5 过单应，向下取整读源；越界或 q.z<=1e-6 写 0。
 *
 * 成功返回 0。
 */
stereo_status stereo_unrectify_depth(const float *src, int src_stride_px, int src_width,
                                     int src_height, const float m[9], float *dst,
                                     int dst_stride_px, int dst_width, int dst_height);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* STEREO_RECTIFY_H */
