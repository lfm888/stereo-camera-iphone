//
//  stereo/stereo_analyze.h — 第 5 步：深度图上的查询与连通域分析。
//  
//  `stereo_analyze_grid` 无需 ctx；`stereo_sample_depth` / `stereo_analyze` 用 ctx
//  只为了复用 scratch（稳态零分配），它们不是无状态函数。
//

#ifndef STEREO_ANALYZE_H
#define STEREO_ANALYZE_H

#include "stereo/stereo_types.h"
#include "stereo/stereo_context.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 深度图上的查询与分析                                                 */
/*                                                                     */
/* 不用 ctx，但内部需要 scratch；这里收了 ctx 只为复用缓冲（稳态零分配）。 */
/* ------------------------------------------------------------------ */

/** 一个深度面块（POD，与 Swift 侧的 Blob 一一对应）。 */
typedef struct stereo_blob {
    int identifier;   /**< 1..blob_count，按格子数从大到小 */
    int cells;        /**< 占多少网格格子 */
    float distance;   /**< 平均深度（米） */
    float x;          /**< 面块中心在相机坐标系里的 X（米） */
    float width;      /**< 物理宽度（米） */
    float rect[4];    /**< 归一化包围盒 x, y, w, h */
} stereo_blob;

/** 分析参数。 */
typedef struct stereo_analyze_params {
    float fx, fy, cx, cy; /**< 深度图内参（像素） */
    int max_blobs;        /**< 最多输出几个面块（App 用 24） */
    int min_cells;        /**< 面块至少占多少格子（App 用 40） */
} stereo_analyze_params;

/** 分析结果的标量部分。面块与标签图走独立参数。 */
typedef struct stereo_analysis {
    int blob_count;
    int grid_width;
    int grid_height;
    float center;       /**< 准星那块深度的分位数 */
    int has_center;     /**< 0 = 有效深度不足，UI 应该显示 "—" */
} stereo_analysis;

/**
 * 网格尺寸（= width/step × height/step，step = max(1, width/160)）。
 * 调用方据此准备 labels 缓冲（grid_width * grid_height 字节）。
 */
int stereo_analyze_grid(int width, int height, int *grid_width, int *grid_height);

/**
 * 在归一化矩形里抽样取 q 分位数（准星读数 / 点选测距）。
 *
 * 返回：0 成功；1 有效样本不足（< 6，UI 显示 "—"）；-1 参数非法。
 * 只收 >0 的像素。
 */
int stereo_sample_depth(stereo_context *ctx, const float *depth, int depth_stride, int width,
                        int height, float x0, float y0, float x1, float y1, float q,
                        float *out_value);

/**
 * 连通域分析：降采样成网格 → 4 邻域连通域 → 保留最大的 max_blobs 个。
 *
 * - center_rect 是准星矩形的归一化 {x0,y0,x1,y1}，center_q 是分位数（0.5 = 中位数），
 *   传 NULL 就不算 center。
 * - blobs 最多写 blobs_capacity 个；labels 写 grid_width*grid_height 字节，可为 NULL。
 * - 返回：0 成功；-1 参数/缓冲非法。
 */
int stereo_analyze(stereo_context *ctx, const float *depth, int depth_stride, int width, int height,
                   const stereo_analyze_params *params, const float center_rect[4],
                   float center_q, stereo_blob *blobs, int blobs_capacity, unsigned char *labels,
                   stereo_analysis *out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* STEREO_ANALYZE_H */
