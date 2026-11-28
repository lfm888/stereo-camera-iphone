//
//  stereo/stereo_context.h — 不透明句柄、匹配参数、生命周期与错误串。
//  
//  context 内部持有跨帧复用的缓冲区，所以**不是线程安全的**；
//  需要多线程就一个线程一个 context。
//

#ifndef STEREO_CONTEXT_H
#define STEREO_CONTEXT_H

#include "stereo/stereo_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 不透明句柄，内部是 C++ 的 stereo_context。 */
typedef struct stereo_context stereo_context;

/** 匹配参数。默认值见 stereo_config_default()。 */
typedef struct stereo_config {
    int max_disp;      /**< 最大视差：32 / 64 / 96 */
    int p1;            /**< 1 像素小台阶的罚项（默认 8） */
    int p2;            /**< 深度跳变的罚项（默认 96） */
    float uniqueness;  /**< 唯一性阈值（默认 0.1） */
    int lr_check;      /**< 左右一致性检查（默认 1 = 开） */
    int threads;       /**< 0 = 自动；当前实现恒为单线程，字段为将来预留 */
} stereo_config;

/* ------------------------------------------------------------------ */
/* 生命周期                                                            */
/* ------------------------------------------------------------------ */

/** 创建 context。失败返回 NULL（只会因为内存不足）。 */
stereo_context *stereo_create(void);

/** 销毁 context（可以传 NULL）。 */
void stereo_destroy(stereo_context *ctx);

/** 把 cfg 填成默认值，便于调用方只改关心的字段。 */
void stereo_config_default(stereo_config *cfg);

/** 应用配置。内部的临时缓冲区在第一次 stereo_match 时按尺寸分配。 */
stereo_status stereo_configure(stereo_context *ctx, const stereo_config *cfg);

/** 最近一次失败的原因（永远返回非 NULL 的字符串，没有错误时是空串）。 */
const char *stereo_last_error(const stereo_context *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* STEREO_CONTEXT_H */
