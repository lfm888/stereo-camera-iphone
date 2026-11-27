// stereo/analyze.cpp — 深度连通域（原 StereoVision/Analyzer.swift 的 C++ 版）
//
// 逐条对齐改造前的 Swift 实现。三处容易写错、且写错了 OverlayView 就会跟导出
// 的 meta.json 对不上的地方，都用注释标出来了：
//   * 网格有效门槛是 `c * 2 >= step * step`（半数以上像素有效），不是 > 0
//   * 连通容差用**当前**格子的深度算：tol = 0.02 + 0.04·z
//   * 面块的 id 是按 cell 数重排后的序号，labels 里 0 表示"未保留"

#include "stereo/analyze.hpp"

#include "stereo/sample.hpp"

#include <algorithm>
#include <cmath>

namespace stereo {
namespace {

/// 一个连通域在扫描过程中的累加器。大小固定 32 字节，
/// analyze_comps_bytes() 按 32 字节/格算，改字段要同步改 static_assert。
struct Comp {
    int id;
    int count;
    float sum_z;
    int sum_x;
    int x0, y0, x1, y1;
};
static_assert(sizeof(Comp) == 32, "analyze_comps_bytes() 按 32 字节算，改字段要同步");

/// 降采样倍率：640 宽 → 4（160 列）。与 Swift 的 max(1, width/160) 一致。
inline int grid_step(int width) {
    const int s = width / 160;
    return s < 1 ? 1 : s;
}

}  // namespace

void analyze_grid_size(int width, int height, int* grid_width, int* grid_height) {
    if (width <= 0 || height <= 0) {
        if (grid_width) *grid_width = 0;
        if (grid_height) *grid_height = 0;
        return;
    }
    const int step = grid_step(width);
    if (grid_width) *grid_width = width / step;
    if (grid_height) *grid_height = height / step;
}

size_t analyze_comps_bytes(int grid_width, int grid_height) {
    if (grid_width <= 0 || grid_height <= 0) return 0;
    return static_cast<size_t>(grid_width) * static_cast<size_t>(grid_height) * sizeof(Comp);
}

bool analyze_depth(const float* depth, int stride, int width, int height,
                   const AnalyzeParams& params,
                   const float center_rect[4], float center_q,
                   BlobRecord* blobs, int blob_capacity,
                   uint8_t* labels,
                   AnalyzeScratch* scratch,
                   AnalyzeResult* out) {
    if (depth == nullptr || out == nullptr || scratch == nullptr) return false;
    if (width <= 0 || height <= 0) return false;
    if (stride <= 0) stride = width;
    if (blobs == nullptr || blob_capacity <= 0 || params.max_blobs <= 0) return false;
    if (!(params.fx > 0.0f) || !(params.fy > 0.0f)) return false;

    const int step = grid_step(width);
    const int gw = width / step;
    const int gh = height / step;
    if (gw <= 0 || gh <= 0) return false;
    const size_t n = static_cast<size_t>(gw) * static_cast<size_t>(gh);

    if (scratch->grid == nullptr || scratch->labels == nullptr || scratch->stack == nullptr ||
        scratch->remap == nullptr) {
        return false;
    }
    if (scratch->comps == nullptr || scratch->comps_bytes < analyze_comps_bytes(gw, gh)) return false;
    float* grid = scratch->grid;
    int* lab = scratch->labels;
    int* stack = scratch->stack;
    int* remap = scratch->remap;
    Comp* comps = static_cast<Comp*>(scratch->comps);

    // ---- 1) 降采样成网格：块内有效像素的平均值，有效像素不足一半就整格作废 ----
    const int block = step * step;
    for (int gy = 0; gy < gh; ++gy) {
        for (int gx = 0; gx < gw; ++gx) {
            float sum = 0.0f;
            int count = 0;
            for (int yy = 0; yy < step; ++yy) {
                const float* row =
                    depth + static_cast<size_t>(gy * step + yy) * static_cast<size_t>(stride);
                for (int xx = 0; xx < step; ++xx) {
                    const float v = row[gx * step + xx];
                    if (v > 0.0f) {
                        sum += v;
                        ++count;
                    }
                }
            }
            // 门槛是"半数以上有效"；count == 0 时 0 >= block 为假，所以不会除零。
            grid[static_cast<size_t>(gy) * gw + gx] =
                (count * 2 >= block) ? sum / static_cast<float>(count) : 0.0f;
        }
    }

    // ---- 2) 4 邻域深度连通域（显式栈，不递归）----
    std::fill(lab, lab + n, 0);
    int comp_count = 0;
    for (size_t i = 0; i < n; ++i) {
        if (!(grid[i] > 0.0f) || lab[i] != 0) continue;
        const int cid = comp_count + 1;
        int sp = 0;
        lab[i] = cid;
        stack[sp++] = static_cast<int>(i);
        Comp c{};
        c.id = cid;
        c.x0 = 1 << 30;
        c.y0 = 1 << 30;
        while (sp > 0) {
            const int p = stack[--sp];
            const float z = grid[p];
            const int px = p % gw;
            const int py = p / gw;
            c.count += 1;
            c.sum_z += z;
            c.sum_x += px;
            if (px < c.x0) c.x0 = px;
            if (px > c.x1) c.x1 = px;
            if (py < c.y0) c.y0 = py;
            if (py > c.y1) c.y1 = py;
            const float tol = 0.02f + 0.04f * z;
            // 邻居顺序与 Swift 一致：左、右、上、下
            const int nb[4] = {p - 1, p + 1, p - gw, p + gw};
            const bool ok[4] = {px > 0, px < gw - 1, py > 0, py < gh - 1};
            for (int k = 0; k < 4; ++k) {
                if (!ok[k]) continue;
                const int q = nb[k];
                if (lab[q] != 0 || !(grid[q] > 0.0f)) continue;
                if (std::fabs(grid[q] - z) < tol) {
                    lab[q] = cid;
                    stack[sp++] = q;
                }
            }
        }
        comps[comp_count++] = c;
    }

    // ---- 3) 过滤 + 排序（对 comps 原地排，不用额外索引数组）----
    // 全序：cell 数降序，相等时按发现顺序（id 升序），保证结果可复现。
    std::sort(comps, comps + comp_count, [](const Comp& a, const Comp& b) {
        if (a.count != b.count) return a.count > b.count;
        return a.id < b.id;
    });

    int emit = 0;
    while (emit < comp_count && comps[emit].count >= params.min_cells) ++emit;
    if (emit > params.max_blobs) emit = params.max_blobs;
    if (emit > blob_capacity) return false;   // 调用方给的面块数组不够大

    // 旧 cid → 新 id（0 = 不保留）。remap 需要 comp_count+1 项，容量由调用方保证 >= n+1。
    std::fill(remap, remap + comp_count + 1, 0);
    for (int k = 0; k < emit; ++k) {
        const Comp& c = comps[k];
        const int new_id = k + 1;
        remap[c.id] = new_id;

        const float z = c.sum_z / static_cast<float>(c.count);
        // 网格中心 → 像素 → 相机坐标（与 Swift 的 (sumX/count + 0.5) * step 一致）
        const float u =
            (static_cast<float>(c.sum_x) / static_cast<float>(c.count) + 0.5f) * static_cast<float>(step);
        const float wpx = static_cast<float>(c.x1 - c.x0 + 1) * static_cast<float>(step);
        BlobRecord& b = blobs[k];
        b.identifier = new_id;
        b.cells = c.count;
        b.distance = z;
        b.x = (u - params.cx) * z / params.fx;
        b.width = wpx * z / params.fx;
        b.rect[0] = static_cast<float>(c.x0) / static_cast<float>(gw);
        b.rect[1] = static_cast<float>(c.y0) / static_cast<float>(gh);
        b.rect[2] = static_cast<float>(c.x1 - c.x0 + 1) / static_cast<float>(gw);
        b.rect[3] = static_cast<float>(c.y1 - c.y0 + 1) / static_cast<float>(gh);
    }

    if (labels != nullptr) {
        for (size_t i = 0; i < n; ++i) labels[i] = static_cast<uint8_t>(remap[lab[i]]);
    }

    // ---- 4) 准星读数：中心小块的中位数 ----
    out->blob_count = emit;
    out->grid_width = gw;
    out->grid_height = gh;
    out->center = 0.0f;
    out->has_center = false;
    if (center_rect != nullptr && scratch->samples != nullptr) {
        float v = 0.0f;
        if (sample_quantile(depth, stride, width, height, center_rect[0], center_rect[1],
                            center_rect[2], center_rect[3], center_q, scratch->samples,
                            kSampleScratchFloats, &v)) {
            out->center = v;
            out->has_center = true;
        }
    }
    return true;
}

}  // namespace stereo
