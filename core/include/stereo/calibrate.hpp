// stereo/calibrate.hpp — 自动标定：在 flip × dy 上搜索极线对齐最好的那组参数
//
// 这是原来 StereoVision/StereoEngine.swift 里 `calibrate()` 那 39 行的 C++ 版：
//   for flip in {0,1} × dy in -12...12   → 50 次打分
//   再在当前最优点附近 ±0.25/±0.5 细化   → 4 次打分
//   取平均匹配代价最小的那组 (flip, dy)
//
// 打分**判据**与 GPU 版一致：把两路各自按候选几何重采样 → 灰度 → 7×7 Census →
// 逐视差算汉明距离、取每像素最小值（WTA）→ 对像素求平均。值越小说明行对得越齐。
//
// 与 GPU 版**有意不同**的两处（都为了在 CPU 上跑得动，且只影响打分精度、不影响输出精度）：
//   1) 重采样抽查用 1 次双线性采样，不做 4 次超采样的平均（GPU 那份是给"输出"用的）。
//   2) 像素**按 step 抽样**（默认 2，即 1/4 的像素），且不聚合 5×5 窗口。
//      判据本身是统计量：640×360 抽 1/4 还有 5.7 万样本，足够区分 ±0.5 px 的行偏移。

#pragma once

#include <cstddef>
#include <cstdint>

#include "stereo/geometry.hpp"
#include "stereo/rectify.hpp"

namespace stereo {

struct CalibrateParams {
    /// 视差搜索上限（与匹配器的 maxDisparity 一致）。
    int max_disp = 64;
    /// 打分时的像素抽样步长：1 = 全部像素，4 = 每 4 行/列取一个（默认）。
    ///
    /// 默认 4 是实测出来的：640×360 下 54 次打分约 0.4 s，和原来"54 次 GPU
    /// command buffer + waitUntilCompleted"同量级，但不占 GPU、也没有 54 次往返。
    /// 想要更鲁棒就调成 2（代价翻倍）。
    int pixel_step = 4;
    /// 初值：没有任何候选比它更好时原样返回。
    int flip0 = 0;
    float dy0 = 0.0f;
};

struct CalibrateResult {
    int flip = 0;
    float dy = 0.0f;
    /// 最好那个候选的平均 WTA 代价（越小越对齐）。
    float score = 0.0f;
    /// 实际打分的次数（2×25 + 4 = 54，调试用）。
    int evaluations = 0;
};

/// 打分回调：返回该候选的平均代价（越小越对齐）。失败返回 INFINITY。
///
/// iOS 用这个把打分放在 GPU 上（和改造前的实现完全一致），
/// 于是"搜索策略"在 C++、"昂贵的那一次比较"在平台侧。
typedef float (*CalibrateScoreFn)(void* user, int flip, float dy);

/// 打分用的临时缓冲。三块都要给，尺寸按 `work_width × work_height` 算：
///   rect_l / rect_r : w*h*4 字节（重采样后的 BGRA8）
///   luma_l / luma_r : w*h 个 float
///   census_l / census_r : w*h 个 uint64
struct CalibrateScratch {
    uint8_t* rect_l = nullptr;
    uint8_t* rect_r = nullptr;
    float* luma_l = nullptr;
    float* luma_r = nullptr;
    uint64_t* census_l = nullptr;
    uint64_t* census_r = nullptr;
};

/// 只做搜索：2 个 flip × dy∈[-12,12] 共 50 次，再 ±0.25/±0.5 细化 4 次，取代价最小者。
///
/// 打分由调用方提供 —— 这样 iOS 可以把打分留在 GPU，其它平台用下面那个 CPU 全流水线版。
/// 返回 false：score 为空、max_disp < 1、或 54 次打分全部失败。
bool calibrate_search(CalibrateScoreFn score, void* user, const CalibrateParams& params,
                      CalibrateResult* out);

/// 自动标定（CPU 全流水线版：重采样 + 灰度 + Census + 逐视差代价都在这里）。
///
/// 给没有 GPU 的平台用。**注意成本**：640×360 上 54 次打分约 4~6 s（单核），
/// 比 GPU 打分慢一个数量级，所以 iOS 走的是 `calibrate_search` + GPU 打分。
///
/// `base` 是第 1 步读到的内参/外参（`flip`/`dy` 字段只作为初值，会被搜索结果覆盖）；
/// `wide`/`ultra` 必须是**原始（未矫正）**的 BGRA 帧，尺寸 = `base.src_width × src_height`。
///
/// 返回 false：参数非法、scratch 没给、或一次打分都没成功。
bool calibrate_rectification(const RectifyInput& base, const ImageView& wide,
                             const ImageView& ultra, const CalibrateParams& params,
                             CalibrateScratch* scratch, CalibrateResult* out);

}  // namespace stereo
