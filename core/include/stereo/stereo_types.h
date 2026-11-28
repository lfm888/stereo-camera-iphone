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
 *  1.50 增加 CPU 全流水线 stereo_process_frame 与参数描述表
 *  2.00 API 约定统一（破坏性）：stereo_status 返回码、stride 单位后缀、
 *       无状态函数去掉 ctx、stereo_process_disparity、参数表硬范围、
 *       运行时版本号 stereo_api_version() */
#define STEREO_API_VERSION 200

/**
 * 统一返回码：0 = 成功，其它都是**失败**。
 *
 * 2.00 起的约定：
 *   * "有效样本不足"这类**正常业务结果**不占错误码，用输出参数表达
 *     （例如 `stereo_sample_depth` 的 `out_has_value`）；
 *   * 无 ctx 的函数没有 last_error，通用说明用 `stereo_status_string()`；
 *   * 有 ctx 的函数仍可用 `stereo_last_error()` 取具体原因。
 */
typedef enum stereo_status {
    STEREO_OK = 0,
    STEREO_INVALID_ARGUMENT = 1,   /**< NULL / 尺寸非法 / 数值越界 */
    STEREO_OUT_OF_MEMORY = 2,      /**< 缓冲分配失败 */
    STEREO_UNSUPPORTED_FORMAT = 3, /**< 像素格式或行跨度不认识 */
    STEREO_BUFFER_TOO_SMALL = 4,   /**< 调用方缓冲不够（out_bytes 会给需求量） */
    STEREO_INTERNAL_ERROR = 5      /**< 算法内部拒绝（不应发生；有 ctx 时看 last_error） */
} stereo_status;

/** 返回码的通用说明（进程生命周期内有效，永远非 NULL）。 */
const char *stereo_status_string(stereo_status status);

/** 运行时版本号（= STEREO_API_VERSION）。为动态库与多语言绑定准备。 */
int stereo_api_version(void);

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
    int stride_bytes; /**< **字节**行跨度（不是像素） */
    int format;       /* enum stereo_format */
} stereo_image;

/** 内参：单位是**原始图像像素**。 */
typedef struct stereo_intrinsics {
    float fx, fy, cx, cy;
} stereo_intrinsics;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* STEREO_TYPES_H */
