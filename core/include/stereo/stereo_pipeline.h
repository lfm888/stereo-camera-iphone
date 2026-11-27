//
//  stereo/stereo_pipeline.h — 第 2..5 步的 CPU 全流水线（非 Apple 前端入口）。
//  
//  iOS 的 GPU 快路径不走这里：那条路径把 rectify/census/SGM/toDepth/unrectify
//  串在一个 command buffer 里，拆成阶段反而更慢（原因见 FAQ Q18）。
//

#ifndef STEREO_PIPELINE_H
#define STEREO_PIPELINE_H

#include "stereo/stereo_types.h"
#include "stereo/stereo_context.h"
#include "stereo/stereo_rectify.h"
#include "stereo/stereo_analyze.h"
#include "stereo/stereo_match.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 第 2..5 步的 CPU 全流水线                                             */
/*                                                                     */
/* 一条函数把「矫正 → 匹配 → 深度 → 回转 → 连通域分析」串完。            */
/* 目的：Windows / Linux 的前端不用自己重写调用顺序。                     */
/*                                                                     */
/* iOS 的 GPU 快路径**不走这里**：那条路径把 rectify/census/SGM/toDepth/  */
/* unrectify 串在一个 command buffer 里，拆成回调反而更慢。               */
/* ------------------------------------------------------------------ */

/** 一次全流水线的输入。 */
typedef struct stereo_frame_input {
    const stereo_image *left;        /**< 原始（未矫正）左/宽相机图，任意 stereo_format */
    const stereo_image *right;       /**< 原始右/超广角图 */
    const stereo_rectify_input *geometry; /**< 内参/外参/工作分辨率/flip/dy */
    const stereo_config *config;     /**< 匹配参数（NULL = 用 stereo_config_default） */
    float min_z;                     /**< 深度有效下限（米） */
    float max_z;                     /**< 深度有效上限（米） */
    stereo_analyze_params analysis;  /**< 面块参数；fx/fy/cx/cy 会被矫正后的 K 覆盖 */
    const float *center_rect;        /**< 准星矩形 {x0,y0,x1,y1} 归一化；NULL = 不算 */
    float center_q;                  /**< 准星分位数（0.5 = 中位数） */
} stereo_frame_input;

/** 一次全流水线的输出。调用方按 work_width/height 准备缓冲。 */
typedef struct stereo_frame_output {
    float *disp;            /**< 可选：视差（工作分辨率，**矫正后**坐标系），NULL = 不要 */
    int disp_stride;        /**< 像素跨度；<= 0 表示等于 work_width */
    float *depth;           /**< 必填：深度（工作分辨率，**宽相机**坐标系，米） */
    int depth_stride;       /**< 像素跨度；<= 0 表示等于 work_width */
    stereo_blob *blobs;     /**< 可选：面块数组（NULL = 不要；只要 labels 时会用内部兜底数组） */
    int blob_capacity;
    unsigned char *labels;  /**< 可选：标签图（grid_width*grid_height 字节） */
    stereo_analysis analysis;   /**< 面块数/网格尺寸/准星读数 */
    stereo_intrinsics k;        /**< 矫正后的内参（给下游反投影用） */
    float focal_baseline;       /**< fx * baseline(米) = to_depth 用的 fB */
    int direction;              /**< +1 / -1 */
} stereo_frame_output;

/** 各阶段耗时（毫秒）。只用于诊断，传 NULL 就不计时。 */
typedef struct stereo_frame_timings {
    double rectify;
    double match;
    double depth;
    double unrectify;
    double analyze;
    double total;
} stereo_frame_timings;

/**
 * 第 2..5 步：矫正 → 匹配 → 深度 → 回转 → 连通域分析。
 *
 * 内部就是按顺序调用 stereo_rectify_geometry / stereo_rectify_image ×2 /
 * stereo_match / stereo_disparity_to_depth / stereo_unrectify_depth /
 * stereo_analyze，所以结果与手工按这个顺序调用**逐位相同**（有单测保证）。
 *
 * `config` 与当前 ctx 的参数相同则不会触发缓冲区重分配（稳态零分配）。
 *
 * `out->blobs` 与 `out->labels` **都不给**时跳过第 5 步（连通域分析），
 * `out->analysis` 清零 —— 只想拿深度时能省掉降采样+连通域的开销。
 *
 * 成功返回 0。
 */
int stereo_process_frame(stereo_context *ctx, const stereo_frame_input *in,
                         stereo_frame_output *out, stereo_frame_timings *timings);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* STEREO_PIPELINE_H */
