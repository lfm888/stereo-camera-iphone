//
//  stereo/stereo_calibrate.h — 第 1 步的自动化：在 flip × dy 上搜索行对齐。
//  
//  `stereo_calibrate` 的 scratch 挂在 ctx 上（CPU 打分）；
//  `stereo_calibrate_search` 由平台提供打分回调（iOS 用 GPU 打分）。
//

#ifndef STEREO_CALIBRATE_H
#define STEREO_CALIBRATE_H

#include "stereo/stereo_types.h"
#include "stereo/stereo_context.h"
#include "stereo/stereo_rectify.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 第 1 步的自动化：极线自动标定                                         */
/*                                                                     */
/* 在 flip × dy 上搜索"行对得最齐"的那组参数。需要暴力跑 55 次打分，   */
/* scratch 挂在 ctx 上复用。                                            */
/* ------------------------------------------------------------------ */

/** 自动标定参数。 */
typedef struct stereo_calibrate_params {
    int max_disp;   /**< 视差搜索上限（与匹配器一致，如 64） */
    int pixel_step; /**< 打分抽样步长：1 = 全部像素，2 = 每 2 行/列（推荐） */
    int flip0;      /**< 初值；没有更好的候选就返回它 */
    float dy0;      /**< 同上 */
} stereo_calibrate_params;

/** 自动标定结果。 */
typedef struct stereo_calibrate_result {
    int flip;         /**< 0 / 1 */
    float dy;         /**< 行偏移（工作分辨率像素） */
    float score;      /**< 平均 WTA 代价，越小越对齐 */
    int evaluations;  /**< 实际打分次数（正常是 55） */
} stereo_calibrate_result;

/**
 * 自动标定。
 *
 * `geometry` 是第 1 步读到的内参/外参（里面的 flip / dy 只作初值）；
 * `wide` / `ultra` 必须是**原始未矫正**的 BGRA 帧，尺寸 = geometry->src_width × 高。
 *
 * 成功返回 0。ctx 为 scratch 宿主（不是线程安全的，和 stereo_match 一样）。
 */
int stereo_calibrate(stereo_context *ctx, const stereo_rectify_input *geometry,
                     const stereo_image *wide, const stereo_image *ultra,
                     const stereo_calibrate_params *params, stereo_calibrate_result *out);

/**
 * 打分回调：给一个候选 (flip, dy)，返回平均匹配代价（越小越对齐）。失败返回 INFINITY。
 *
 * iOS 用它把打分留在 GPU（一次打分 ~3 ms），于是 54 次搜索只要 ~0.2 s；
 * 而 stereo_calibrate 的 CPU 打分在 640×360 上要 4~6 s。
 */
typedef float (*stereo_score_fn)(void *user, int flip, float dy);

/**
 * 只跑搜索：候选序列 / 细化 / 比较全在这里，打分由 `score` 提供。
 *
 * 成功返回 0；score 为空或全部打分失败返回非 0。
 */
int stereo_calibrate_search(stereo_score_fn score, void *user,
                            const stereo_calibrate_params *params,
                            stereo_calibrate_result *out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* STEREO_CALIBRATE_H */
