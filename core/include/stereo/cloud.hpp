// stereo/cloud.hpp — 导出用的像素运算：点云 PLY、深度裁剪、16 位毫米打包、跨度拷贝
//
// 这四件事原来都在 Swift 里逐像素跑（Capture.ply / StereoEngine.depthData / depthCopy）。
// 拍一张 640×360 的照片要格式化约 14 万行字符串，是导出卡顿的主因；
// 放进 C++ 之后 Swift 侧一个像素循环都不剩。
//
// 语义逐条对齐改造前的 Swift 实现（包括颜色取样用的 BGRA 分量顺序、%.4f 的位数）。

#pragma once

#include <cstddef>
#include <cstdint>

#include "stereo/geometry.hpp"

namespace stereo {

/// 反投影成 ASCII PLY 的**最坏情况**缓冲大小（用来做粗估）。
///
/// 实际用法建议：先传 `out == nullptr` 问一次精确尺寸（见 `point_cloud_ply`），
/// 再按那个尺寸分配 —— 精确值通常只有最坏情况的 1/3。
size_t point_cloud_ply_capacity(int width, int height);

/// 深度图 + 彩色图 → ASCII PLY 点云（相机坐标系：x 右、y 下、z 前，单位米）。
///
/// - `depth` 带**像素**行跨度；值为 0 或负的像素跳过（无效深度的约定见 sgm.hpp）。
/// - `rgba` 是原始相机图（BGRA8，行跨度为**字节**），按比例取样当颜色；
///   `rgba_width/height` 与深度图尺寸不同也没关系（整帧彩色 + 降采样深度的常见组合）。
/// - `out == nullptr`：只算需要的字节数写进 `out_bytes`，返回 true。
/// - 缓冲不够：`out_bytes` 写需要多少，返回 false（不写越界）。
bool point_cloud_ply(const float* depth, int depth_stride, int width, int height,
                     const Intrinsics& k,
                     const uint8_t* rgba, int rgba_stride, int rgba_width, int rgba_height,
                     char* out, size_t capacity, size_t* out_bytes);

/// 深度裁剪：非有限值、或超出 [min_z, max_z] 的写 0。src/dst 允许不同跨度。
///
/// 对应原来 StereoEngine.depthData 里那个逐像素循环（系统深度图的 minZ/maxZ 归一化）。
bool depth_filter(const float* src, int src_stride, float* dst, int dst_stride,
                  int width, int height, float min_z, float max_z);

/// 深度 → 16 位毫米（导出 depth_mm.png 用）。超出范围钳到 0..65535，四舍五入。
bool depth_to_u16_mm(const float* depth, int stride, int width, int height, uint16_t* out);

/// 带跨度 → 紧凑的 float 拷贝（把 MTLBuffer 的行跨度拆掉）。
bool copy_depth(const float* src, int src_stride, int width, int height, float* dst);

}  // namespace stereo
