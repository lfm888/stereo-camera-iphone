//
//  stereo/stereo_types.h — 全部公开类型的基础定义（纯 C，无依赖）。
//  
//  版本宏、像素格式、图像视图、内参。任何子头都可以安全地只包含它。
//  约定：图像 `stride` 是**字节**；内参单位是**原始图像像素**。
//

#ifndef STEREO_TYPES_H
#define STEREO_TYPES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 版本号：主版本 * 100 + 次版本。
 *  1.00 首次发布：stereo_match + stereo_disparity_to_depth
 *  1.10 增加极线矫正几何、矫正重采样、深度回转（四步管线里的第 2、4 步）
 *  1.20 增加深度图查询与分析：stereo_sample_depth / stereo_analyze
 *  1.30 增加导出用的像素运算：点云 PLY、深度裁剪 / 打包 / 跨度拷贝
 *  1.40 增加自动标定搜索：stereo_calibrate
 *  1.50 增加 CPU 全流水线 stereo_process_frame 与参数描述表 */
#define STEREO_API_VERSION 150

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

/** 内参：单位是**原始图像像素**。 */
typedef struct stereo_intrinsics {
    float fx, fy, cx, cy;
} stereo_intrinsics;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* STEREO_TYPES_H */
