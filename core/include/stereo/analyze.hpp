// stereo/analyze.hpp — 深度图的连通域分析（"3D segmentation" 面块）
//
// 这是原来 StereoVision/Analyzer.swift 里那 100 行算法的 C++ 版：
//   1) 把深度图按 step 降采样成网格（默认 step = max(1, W/160) → 640×360 得 160×90）
//   2) 4 邻域深度连通域（容差 0.02 + 0.04·z，随深度线性放松）
//   3) 保留 cell 数 >= min_cells 的前 max_blobs 个，输出距离 / 横向位置 / 宽度 / 归一化包围盒
//   4) 顺便算"准星读数"：中心小块深度的中位数
//
// 语义逐条对应改造前的 Swift 实现（包括取整方向、容差公式、排序方式），
// 因为 OverlayView / Renderer / 导出的 meta.json 都依赖这些数字。
//
// 缓冲区一律由调用方提供（见 AnalyzeScratch），函数内不分配内存。

#pragma once

#include <cstddef>
#include <cstdint>

namespace stereo {

/// 一个面块。字段与 Swift 侧的 `Blob` 一一对应（rect 用归一化坐标，不再用 CGRect）。
struct BlobRecord {
    int identifier = 0;    ///< 1..blob_count，按 cell 数从大到小
    int cells = 0;         ///< 网格格子数
    float distance = 0;    ///< 平均深度，米
    float x = 0;           ///< 面块中心在相机坐标系里的 X，米
    float width = 0;       ///< 面块宽度（按深度缩放后的物理宽度），米
    float rect[4] = {0, 0, 0, 0};  ///< 归一化包围盒 x, y, w, h
};

struct AnalyzeParams {
    /// 深度图内参（像素单位）。用来把网格坐标反投影成米。
    float fx = 1, fy = 1, cx = 0, cy = 0;
    int max_blobs = 24;
    int min_cells = 40;
};

struct AnalyzeResult {
    int blob_count = 0;
    int grid_width = 0;
    int grid_height = 0;
    float center = 0;
    bool has_center = false;   ///< false = 准星那块有效深度不足（UI 显示 "—"）
};

/// 网格尺寸。降采样倍率是 step = max(1, width/160)，所以 640 宽 → 160 列。
void analyze_grid_size(int width, int height, int* grid_width, int* grid_height);

/// comps 缓冲需要多少字节（内部结构尺寸对外不暴露，只能问这个函数）。
size_t analyze_comps_bytes(int grid_width, int grid_height);

/// 临时缓冲。四个都要给，尺寸见上；`analyze_depth` 会自己校验并返回 false。
struct AnalyzeScratch {
    float* grid = nullptr;     ///< grid_width * grid_height
    int* labels = nullptr;     ///< grid_width * grid_height
    int* stack = nullptr;      ///< grid_width * grid_height（洪泛用的显式栈）
    int* remap = nullptr;      ///< grid_width * grid_height + 1（旧 id → 新 id）
    void* comps = nullptr;     ///< analyze_comps_bytes(...)
    size_t comps_bytes = 0;
    /// 准星分位数采样用（>= kSampleScratchFloats）。
    /// 单独一块而不是复用 grid：grid 留给调用方看（调试/后续处理都需要）。
    float* samples = nullptr;
};

/// 分析一帧深度图。
///
/// - `center_rect` 是归一化矩形的 {x0, y0, x1, y1}（准星那块），`center_q` 是分位数（0.5 = 中位数）。
/// - `blobs` 最多写 blob_capacity 个；`labels` 写 grid_width*grid_height 个字节，
///   0 = 未保留（含"太小被过滤"的块），1..blob_count = 保留下来的面块，可为 nullptr。
/// - 返回 false：参数非法 / 缓冲不足 / 尺寸非法。
bool analyze_depth(const float* depth, int stride, int width, int height,
                   const AnalyzeParams& params,
                   const float center_rect[4], float center_q,
                   BlobRecord* blobs, int blob_capacity,
                   uint8_t* labels,
                   AnalyzeScratch* scratch,
                   AnalyzeResult* out);

}  // namespace stereo
