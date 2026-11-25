// stereo/sgm.hpp — 跨平台立体匹配核心：Census / SGM / WTA / 深度
//
// 这里的每个函数都对应 StereoVision/Shaders.metal 里的一个 kernel，
// 语义逐条对齐（包括哨兵值、饱和、截断、边界处理），先用标量实现，
// 通过 tools/gen_golden.py 生成的 .npy 向量做逐位对拍，之后再向量化。
//
// 设计约束：
//   * 零 Apple 依赖（不 include simd/Metal/CoreGraphics），Windows/Linux 可编
//   * 不做动态分配（缓冲区由调用方传入），便于后面接线程池与池化
//   * 不抛异常，不依赖 RTTI
//   * 浮点一律 float32，且不要合并 FMA（见 README：跨平台逐位一致的前提）

#pragma once

#include <cstddef>
#include <cstdint>

namespace stereo {

/// Census 描述子的位数（7x7 窗口去掉中心 = 48）。
inline constexpr int kCensusBits = 48;

/// 代价体积里"搜索越界"的标记值（= 48，即 48 位全部不同）。
inline constexpr uint8_t kCostInvalid = 48;

/// 视差无效的约定值（下游所有模块都认这个）。
inline constexpr float kInvalidDisp = 0.0f;

/// 深度无效的约定值。
inline constexpr float kInvalidDepth = 0.0f;

/// 对应 Shaders.metal 的 SGMParams（字段顺序也保持一致，便于将来放进共享头）。
struct SgmParams {
    int W = 0;               ///< 宽度（像素）
    int H = 0;               ///< 高度（像素）
    int D = 0;               ///< 视差数（32 / 64 / 96）
    int dir = 1;             ///< +1：右图在左图右边（视差为正）
    unsigned P1 = 8;         ///< 1 像素小台阶的罚项
    unsigned P2 = 96;        ///< 深度跳变的罚项（随图像梯度自适应）
    float uniqueness = 0.1f; ///< 唯一性阈值
    int lrCheck = 1;         ///< 是否做左右一致性检查
};

// ---------------------------------------------------------------------------
// Shaders.metal: census()
// ---------------------------------------------------------------------------

/// RGB → 亮度。rgb 是紧排的 3 通道；stride_bytes 是一行的字节数（= w*3 或更大）。
void rgb_to_luma(const uint8_t* rgb, int w, int h, int stride_bytes, float* luma);

/// 7x7 Census：48 个邻居 vs 中心，压成 uint64 的低 48 位。
///
/// 位序：bit k 对应第 k 个邻居，遍历顺序是 dy 外层、dx 内层、跳过中心。
/// Metal 版把 bit0..31 写进 dst.x、bit32..47 写进 dst.y，在 XOR+popcount 下等价。
/// 邻居坐标**钳到图像边缘**（clamp，不是跳过）。
void census(const float* luma, int w, int h, uint64_t* out);

// ---------------------------------------------------------------------------
// Shaders.metal: costVolume()
// ---------------------------------------------------------------------------

/// 匹配代价 = Census 汉明距离，uchar 存储，布局 AoS [(y*W+x)*D + d]（不要改）。
/// 越界（0 <= x - dir*d < W 不成立）的格子填 kCostInvalid。
void cost_volume(const uint64_t* cl, const uint64_t* cr, const SgmParams& p, uint8_t* C);

// ---------------------------------------------------------------------------
// Shaders.metal: sgmPath() / sgm_all_paths()
// ---------------------------------------------------------------------------

/// 沿一个方向 ((dx,dy) ∈ {(±1,0), (0,±1)}) 做路径聚合，结果写进 S。
///
/// first == true 时覆盖写成 L；否则饱和累加 min(S + L, 65535)。
/// 递推（与 kernel 一致）：
///     P2' = max(P1+1, (unsigned)((float)P2 / (1 + 16*|I - I_prev|)))
///     m   = min(prev[d], min(prev[d-1], prev[d+1]) + P1, min_d(prev) + P2')
///     L   = C + m - min_d(prev)
/// 其中 prev[] 两端是 0x3FFFFFFF 哨兵，模拟视差越界。
void sgm_path(const uint8_t* C, uint16_t* S, const float* luma,
              const SgmParams& p, int dx, int dy, bool first);

/// 4 条路径：(+x, -x, +y, -y)，顺序与 StereoEngine.swift 一致。
void sgm_all_paths(const uint8_t* C, uint16_t* S, const float* luma, const SgmParams& p);

// ---------------------------------------------------------------------------
// Shaders.metal: wtaRight() / wtaLeft()
// ---------------------------------------------------------------------------

/// 右视差图：只有整数 WTA，无亚像素、无有效性判断，仅供左右一致性检查。
void wta_right(const uint16_t* S, const SgmParams& p, float* dispR);

/// 左视差图：WTA + 唯一性 + 左右一致性 + 抛物亚像素，一次遍历全做完。
/// 无效输出 kInvalidDisp(0.0f)。
void wta_left(const uint16_t* S, const float* dispR, const SgmParams& p, float* disp);

// ---------------------------------------------------------------------------
// Shaders.metal: toDepth()
// ---------------------------------------------------------------------------

/// 视差 → 深度：3x3 中值（在视差域做，因为 1/d 的中值 ≠ 深度的中值），
/// 少于 5 个有效邻居整块判无效；越出 [minZ, maxZ] 亦为无效。
void to_depth(const float* disp, int w, int h, float fB, float minZ, float maxZ, float* depth);

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

/// popcount64：MSVC 与 gcc/clang 各走内建。
inline int popcount64(uint64_t v) {
#if defined(_MSC_VER)
    return static_cast<int>(__popcnt64(v));
#else
    return __builtin_popcountll(v);
#endif
}

}  // namespace stereo
