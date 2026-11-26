// stereo/rectify.hpp — 第 2 步的**重采样**部分 + 第 4 步的深度回转
//
// 对应 Shaders.metal 的两个 kernel，语义逐条复刻：
//   rectify()         → rectify_image()
//   unrectifyDepth()  → unrectify_depth()
//
// 这两个函数是"平台无关版"的实现：iOS 上 GPU 路径继续用 Metal kernel（更快），
// Windows/Linux 上没有 Metal，就走这里。两条路径的输入输出完全相同，
// 所以同一帧在两边算出的深度图可以直接并排比较（见 core/tests/geometry_test.cpp
// 里对重采样不变量的检查）。
//
// 与 Metal 版**允许**的差异：
//   * Metal 的 `filter::linear` 用 8 bit 定点权重，这里是 float 权重
//     → 单通道可能差 ±1 LSB。整流后的图是 8 bit，这个差被量化吃掉，不必对拍。
//   * 采样越界一律按 `address::clamp_to_zero` 处理（贡献 0），与 kernel 一致。

#pragma once

#include <cstdint>

#include "stereo/geometry.hpp"

namespace stereo {

/// 像素格式。数值与 core/include/stereo/stereo_c_api.h 的 enum stereo_format 一致。
enum ImageFormat {
    kFormatRGB8 = 0,
    kFormatBGR8 = 1,
    kFormatGray8 = 2,
    kFormatBGRA8 = 3,
    kFormatRGBA8 = 4,
};

/// 只读图像视图。stride 是**字节**行跨度。
struct ImageView {
    const uint8_t* data = nullptr;
    int width = 0;
    int height = 0;
    int stride = 0;
    int format = kFormatBGRA8;
};

/// 可写图像视图。stride 是**字节**行跨度。
struct MutableImageView {
    uint8_t* data = nullptr;
    int width = 0;
    int height = 0;
    int stride = 0;
    int format = kFormatBGRA8;
};

/// 每像素字节数；格式不认识返回 0。
int format_channels(int format);

/// 用单应 `m` 把 src 重采样到 dst。
///
/// dst 可以与 src 尺寸不同，但**格式必须相同**：这里按字节处理通道，
/// 不做任何格式转换，BGRA8→RGBA8 这类调用会把颜色写错。
///
/// 与 Metal kernel 一致：每个目标像素做 4 次 (0.25|0.75, 0.25|0.75) 的子采样，
/// 每次双线性（clamp_to_zero）取值，再取平均。
///
/// 返回 false：格式不认识或两端不一致、尺寸非法、指针为空。
bool rectify_image(const ImageView& src, const Mat3& m, const MutableImageView& dst);

/// 把在**矫正后**坐标系里的深度图映射回**宽相机**坐标系（unrectifyDepth）。
///
/// 与 Metal kernel 一致：目标像素取 (x+0.5, y+0.5) 过单应，再**向下取整**读源像素，
/// 越界或 q.z <= 1e-6 写 0。这是纯几何回转，不做任何插值。
///
/// 返回 false：尺寸非法或指针为空。
bool unrectify_depth(const float* src, int src_width, int src_height, int src_stride_px,
                     const Mat3& m, float* dst, int dst_width, int dst_height,
                     int dst_stride_px);

}  // namespace stereo
