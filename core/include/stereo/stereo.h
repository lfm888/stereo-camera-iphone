/*
 * stereo/stereo.h — 跨平台立体匹配库的**纯 C 接口**（三层架构的边界）
 *
 *   第一层  C++（core/src/*.cpp）       : census / costVolume / sgmPath / wta / depth
 *   第二层  C / Objective-C++（中间层）  : 生命周期、像素格式、与平台框架（CoreVideo/Metal）的接触面
 *   第三层  Swift / 任意语言             : 通过本头文件直接调用（有桥接头即可）
 *
 * 这个头文件是唯一的对外契约：不 include 任何 C++ 头、不出现 C++ 类型、
 * 所有结构体都是 POD。Windows (MSVC)、Linux (gcc)、iOS/macOS (Apple clang) 共用。
 *
 * 线程安全：stereo_context 不是线程安全的。一个 context 一次只在一个线程里用；
 * 多路并行就建多个 context（内部的临时缓冲区跟着 context 走）。
 */
#ifndef STEREO_STEREO_H
#define STEREO_STEREO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 版本号：主版本 * 100 + 次版本。 */
#define STEREO_API_VERSION 100

/** 不透明句柄，内部是 C++ 的 stereo_context。 */
typedef struct stereo_context stereo_context;

/** 像素格式。通道顺序按内存里的字节顺序算。 */
enum stereo_format {
    STEREO_FORMAT_RGB8 = 0,
    STEREO_FORMAT_BGR8 = 1,
    STEREO_FORMAT_GRAY8 = 2,
    STEREO_FORMAT_BGRA8 = 3,
    STEREO_FORMAT_RGBA8 = 4
};

/**
 * 一幅图。
 *
 * stride 是**字节**行跨度（不一定等于 width * 每像素字节数，CoreVideo 的
 * CVPixelBuffer 和 MTLTexture 都给对齐过的行跨度）。
 * data 指向左上角第一个像素。
 */
typedef struct stereo_image {
    const uint8_t *data;
    int width;
    int height;
    int stride;
    int format; /* enum stereo_format */
} stereo_image;

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
int stereo_configure(stereo_context *ctx, const stereo_config *cfg);

/** 最近一次失败的原因（永远返回非 NULL 的字符串，没有错误时是空串）。 */
const char *stereo_last_error(const stereo_context *ctx);

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

#endif /* STEREO_STEREO_H */
