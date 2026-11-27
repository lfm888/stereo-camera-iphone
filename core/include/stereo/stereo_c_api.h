/*
 * stereo/stereo_c_api.h — 跨平台立体匹配库的**纯 C 接口**（三层架构的边界）
 *
 *   第一层  C++（core/src 下的 .cpp）     : census / costVolume / sgmPath / wta / depth
 *   第二层  C / Objective-C++（中间层）  : 生命周期、像素格式、与平台框架（CoreVideo/Metal）的接触面
 *   第三层  Swift / 任意语言             : 通过本头文件直接调用（有桥接头即可）
 *
 * 这个头文件是唯一的对外契约：不 include 任何 C++ 头、不出现 C++ 类型、
 * 所有结构体都是 POD。Windows (MSVC)、Linux (gcc)、iOS/macOS (Apple clang) 共用。
 *
 * 线程安全：stereo_context 不是线程安全的。一个 context 一次只在一个线程里用；
 * 多路并行就建多个 context（内部的临时缓冲区跟着 context 走）。
 */
#ifndef STEREO_C_API_H
#define STEREO_C_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus      // 这一行是为objC准备的！
extern "C" {
#endif

/** 版本号：主版本 * 100 + 次版本。
 *  1.00 首次发布：stereo_match + stereo_disparity_to_depth
 *  1.10 增加极线矫正几何、矫正重采样、深度回转（四步管线里的第 2、4 步）
 *  1.20 增加深度图查询与分析：stereo_sample_depth / stereo_analyze */
#define STEREO_API_VERSION 120

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

/* ------------------------------------------------------------------ */
/* 第 2 步：极线矫正（几何）                                            */
/*                                                                     */
/* 下面三个函数**不碰 ctx**，纯粹是输入→输出的计算，所以天然线程安全、 */
/* 无生命周期问题。它们对应 Shaders.metal 的 rectify / unrectifyDepth  */
/* kernel，给没有 Metal 的平台（Windows / Linux）用。                    */
/* ------------------------------------------------------------------ */

/** 内参：单位是**原始图像像素**。 */
typedef struct stereo_intrinsics {
    float fx, fy, cx, cy;
} stereo_intrinsics;

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
int stereo_rectify_geometry(const stereo_rectify_input *in, stereo_rectify_output *out);

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
int stereo_rectify_image(const stereo_image *src, const float m[9], stereo_image *dst);

/**
 * 深度图回转：把**矫正后**坐标系的深度映射回**宽相机**坐标系（unrectifyDepth）。
 *
 * src/dst 都是 float32，行跨度以**像素**为单位（传 0 表示等于 width）。
 * m 是 stereo_rectify_output.to_rect_image。
 * 语义与 kernel 一致：目标像素 +0.5 过单应，向下取整读源；越界或 q.z<=1e-6 写 0。
 *
 * 成功返回 0。
 */
int stereo_unrectify_depth(const float *src, int src_stride, int src_width, int src_height,
                           const float m[9], float *dst, int dst_stride, int dst_width,
                           int dst_height);

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

#endif /* STEREO_C_API_H */
