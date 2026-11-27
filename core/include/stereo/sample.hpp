// stereo/sample.hpp — 深度图上的分位数采样
//
// 用途：点选测距、准星读数、面块测距都用"某个矩形里深度的第 q 分位数"。
// 这是纯数组运算，跟平台无关，所以它在 C++ 里。
//
// 语义（与改造前 Swift 的 `StereoFrame.sample(_:q:)` 完全一致）：
//   * 矩形用**归一化**坐标 [0,1]，左上/右下；四舍五入方向是"向零取整"（Int()）；
//   * 采样步长自适应：step = max(1, (int)sqrt(面积 / 400))，目的是让样本数落在 ~400 量级；
//   * 只收 >0 的像素（0 是"无效深度"的约定值，见 sgm.hpp 的 kInvalidDepth）；
//   * 有效样本 < 6 个就返回失败（不够稳定，调用方应该显示 "—"）。

#pragma once

namespace stereo {

/// 一次采样最多需要多少个 float 的 scratch。
///
/// 步长会随面积增大而增大，所以样本数有上界：小矩形（面积 < 1600）用 step=1，
/// 样本数 = 面积 < 1600；大矩形 step >= 2，样本数 ≈ 面积/step² <= 1600。
inline constexpr int kSampleScratchFloats = 2048;

/// 有效样本少于这个数就判失败。
inline constexpr int kSampleMinCount = 6;

/// 在归一化矩形 [x0,x1] × [y0,y1] 里抽样，取第 q 分位数。
///
/// scratch 至少 `kSampleScratchFloats` 个 float（调用方给，函数内不分配）。
/// 返回 false：参数非法、scratch 太小、或有效样本不足 `kSampleMinCount`。
bool sample_quantile(const float* depth, int stride, int width, int height,
                     float x0, float y0, float x1, float y1, float q,
                     float* scratch, int scratch_capacity, float* out);

}  // namespace stereo
