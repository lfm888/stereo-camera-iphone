// stereo/sample.cpp — 分位数采样
//
// 逐行对应改造前 Swift 的 StereoFrame.sample(_:q:)，包括那几处看起来"随意"的
// 取整方向：它们决定了同一个矩形在两条实现里抽出同一批像素，别顺手"修正"。

#include "stereo/sample.hpp"

#include <algorithm>
#include <cmath>

namespace stereo {

bool sample_quantile(const float* depth, int stride, int width, int height,
                     float x0, float y0, float x1, float y1, float q,
                     float* scratch, int scratch_capacity, float* out) {
    if (depth == nullptr || scratch == nullptr || out == nullptr) return false;
    if (width <= 0 || height <= 0) return false;
    if (stride <= 0) stride = width;
    if (q < 0.0f) q = 0.0f;
    if (q > 1.0f) q = 1.0f;

    // 归一化 → 像素，向零取整（与 Swift 的 Int(...) 一致），再钳到图内。
    int px0 = static_cast<int>(x0 * static_cast<float>(width));
    int px1 = static_cast<int>(x1 * static_cast<float>(width));
    int py0 = static_cast<int>(y0 * static_cast<float>(height));
    int py1 = static_cast<int>(y1 * static_cast<float>(height));
    if (px0 < 0) px0 = 0;
    if (py0 < 0) py0 = 0;
    if (px1 > width) px1 = width;
    if (py1 > height) py1 = height;
    if (px1 <= px0 || py1 <= py0) return false;

    const int area = (px1 - px0) * (py1 - py0);
    int step = static_cast<int>(std::sqrt(static_cast<double>(area) / 400.0));
    if (step < 1) step = 1;

    int n = 0;
    for (int y = py0; y < py1; y += step) {
        const float* row = depth + static_cast<size_t>(y) * static_cast<size_t>(stride);
        for (int x = px0; x < px1; x += step) {
            const float v = row[x];
            if (!(v > 0.0f)) continue;
            if (n >= scratch_capacity) return false;   // scratch 太小，宁可报错也不静默截断
            scratch[n++] = v;
        }
    }
    if (n < kSampleMinCount) return false;

    // 只要第 k 个顺序统计量，不需要全排序。
    int k = static_cast<int>(static_cast<float>(n - 1) * q);
    if (k < 0) k = 0;
    if (k >= n) k = n - 1;
    std::nth_element(scratch, scratch + k, scratch + n);
    *out = scratch[k];
    return true;
}

}  // namespace stereo
