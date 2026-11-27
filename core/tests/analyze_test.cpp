// analyze_test.cpp — 深度连通域分析 / 分位数采样的自检
//
// 这两块是纯数组运算，但"看起来对"和"真的对"差得很远：一旦 labels 与 blobs 对不上，
// OverlayView 画的框和导出的 meta.json 就会互相矛盾。所以这里用一套**独立写的朴素参考**
// （暴力排序求分位、暴力重建每个连通域）来对拍，而不是只检查几个不变量。
//
// 用法：analyze_test

#include "stereo/analyze.hpp"
#include "stereo/sample.hpp"
#include "stereo/stereo_c_api.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int g_fail = 0;

void report(const char* name, bool ok, const char* detail = "") {
    std::printf("  %-38s %s%s%s\n", name, ok ? "OK  " : "FAIL", detail[0] ? "  " : "", detail);
    if (!ok) ++g_fail;
}

bool near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

/// 造一张合成深度图：几个深度不同的实心矩形，其余是 0（无效）。
/// 视差/深度的具体值不重要，重要的是"块内恒定、块间差异明显"。
struct Scene {
    int w = 0, h = 0;
    std::vector<float> depth;
    struct Region { int x0, y0, x1, y1; float z; };
    std::vector<Region> regions;

    void add(int ax0, int ay0, int ax1, int ay1, float z) {
        for (int y = ay0; y < ay1; ++y) {
            for (int x = ax0; x < ax1; ++x) depth[static_cast<size_t>(y) * w + x] = z;
        }
        regions.push_back(Region{ax0, ay0, ax1, ay1, z});
    }
};

Scene make_scene(int w, int h) {
    Scene s;
    s.w = w;
    s.h = h;
    s.depth.assign(static_cast<size_t>(w) * h, 0.0f);
    // 三个块：一个大的在左（近）、一个中的在右（中）、一个小的在下（远）
    s.add(w / 10, h / 10, w / 2, h * 6 / 10, 0.50f);
    s.add(w * 55 / 100, h * 15 / 100, w * 85 / 100, h * 5 / 10, 1.20f);
    s.add(w * 20 / 100, h * 70 / 100, w * 35 / 100, h * 9 / 10, 2.50f);
    return s;
}

/// 朴素参考：把整个矩形内的有效像素抄出来，完整排序，取第 k 个。
bool ref_quantile(const std::vector<float>& depth, int w, int h, float x0, float y0, float x1,
                  float y1, float q, float* out) {
    int px0 = std::max(0, static_cast<int>(x0 * w)), px1 = std::min(w, static_cast<int>(x1 * w));
    int py0 = std::max(0, static_cast<int>(y0 * h)), py1 = std::min(h, static_cast<int>(y1 * h));
    if (px1 <= px0 || py1 <= py0) return false;
    const int area = (px1 - px0) * (py1 - py0);
    int step = static_cast<int>(std::sqrt(static_cast<double>(area) / 400.0));
    if (step < 1) step = 1;
    std::vector<float> v;
    for (int y = py0; y < py1; y += step) {
        for (int x = px0; x < px1; x += step) {
            const float d = depth[static_cast<size_t>(y) * w + x];
            if (d > 0.0f) v.push_back(d);
        }
    }
    if (v.size() < 6) return false;
    std::sort(v.begin(), v.end());
    *out = v[static_cast<size_t>(static_cast<int>(static_cast<float>(v.size() - 1) * q))];
    return true;
}

/// 朴素参考的网格降采样（用来独立核对 blob 的 cells / distance 是否自洽）。
struct Grid {
    int gw = 0, gh = 0, step = 1;
    std::vector<float> g;
};

Grid make_grid(const std::vector<float>& depth, int w, int h) {
    Grid out;
    out.step = std::max(1, w / 160);
    out.gw = w / out.step;
    out.gh = h / out.step;
    out.g.assign(static_cast<size_t>(out.gw) * out.gh, 0.0f);
    const int block = out.step * out.step;
    for (int gy = 0; gy < out.gh; ++gy) {
        for (int gx = 0; gx < out.gw; ++gx) {
            float sum = 0;
            int c = 0;
            for (int yy = 0; yy < out.step; ++yy) {
                for (int xx = 0; xx < out.step; ++xx) {
                    const float v = depth[static_cast<size_t>(gy * out.step + yy) * w + gx * out.step + xx];
                    if (v > 0.0f) { sum += v; ++c; }
                }
            }
            out.g[static_cast<size_t>(gy) * out.gw + gx] = (c * 2 >= block) ? sum / c : 0.0f;
        }
    }
    return out;
}

}  // namespace

int main() {
    using namespace stereo;

    // ---------------- 1) 分位数采样 ----------------
    {
        const int w = 320, h = 240;
        Scene s = make_scene(w, h);
        std::vector<float> scratch(kSampleScratchFloats);
        float got = 0, want = 0;

        // 全部有效区域的整体分布：0.5 应落在中间那个块的深度附近
        const bool ok = sample_quantile(s.depth.data(), w, w, h, 0.0f, 0.0f, 1.0f, 1.0f, 0.5f,
                                        scratch.data(), static_cast<int>(scratch.size()), &got);
        const bool ref = ref_quantile(s.depth, w, h, 0.0f, 0.0f, 1.0f, 1.0f, 0.5f, &want);
        report("整体中位数与暴力排序一致", ok && ref && got == want);

        // 单个块内恒定 → 任意分位数都应该等于该块的深度
        const float rx0 = static_cast<float>(w / 10) / w, rx1 = static_cast<float>(w / 2) / w;
        const float ry0 = static_cast<float>(h / 10) / h, ry1 = static_cast<float>(h * 6 / 10) / h;
        for (float q : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
            float v = 0;
            const bool okq = sample_quantile(s.depth.data(), w, w, h, rx0, ry0, rx1, ry1, q,
                                             scratch.data(), static_cast<int>(scratch.size()), &v);
            char buf[64];
            std::snprintf(buf, sizeof buf, "q=%.2f -> %.2f", static_cast<double>(q),
                          static_cast<double>(v));
            report("块内分位数恒定", okq && near(v, 0.50f, 1e-6f), buf);
        }

        // 各分位数与暴力排序逐点一致
        int mismatch = 0;
        for (int i = 0; i <= 20; ++i) {
            const float q = static_cast<float>(i) / 20.0f;
            float a = 0, b = 0;
            if (!sample_quantile(s.depth.data(), w, w, h, 0.0f, 0.0f, 1.0f, 1.0f, q,
                                 scratch.data(), static_cast<int>(scratch.size()), &a)) {
                ++mismatch;
                continue;
            }
            if (!ref_quantile(s.depth, w, h, 0.0f, 0.0f, 1.0f, 1.0f, q, &b) || a != b) ++mismatch;
        }
        report("21 个分位数全部与暴力排序逐位一致", mismatch == 0);

        // 无效输入
        float v = 0;
        report("全 0 区域 -> 失败", !sample_quantile(s.depth.data(), w, w, h, 0.0f, 0.0f, 0.01f, 0.01f,
                                                    0.5f, scratch.data(), 2048, &v));
        report("scratch 太小 -> 失败", !sample_quantile(s.depth.data(), w, w, h, 0.0f, 0.0f, 1.0f,
                                                       1.0f, 0.5f, scratch.data(), 8, &v));
        report("矩形反向 -> 失败", !sample_quantile(s.depth.data(), w, w, h, 0.6f, 0.0f, 0.4f, 1.0f,
                                                   0.5f, scratch.data(), 2048, &v));
    }

    // ---------------- 2) 连通域分析 ----------------
    {
        const int w = 640, h = 360;   // 真实工作分辨率：step=4 → 160×90
        Scene s = make_scene(w, h);
        const Grid grid = make_grid(s.depth, w, h);

        int gw = 0, gh = 0;
        analyze_grid_size(w, h, &gw, &gh);
        char buf[128];
        std::snprintf(buf, sizeof buf, "%d×%d（期望 %d×%d）", gw, gh, grid.gw, grid.gh);
        report("网格尺寸符合 W/160 规则", gw == grid.gw && gh == grid.gh && gw == 160 && gh == 90, buf);

        const size_t n = static_cast<size_t>(gw) * static_cast<size_t>(gh);
        std::vector<float> g(n), samples(kSampleScratchFloats);
        std::vector<int> labels(n), stack(n), remap(n + 1);
        std::vector<unsigned> comps_bytes(analyze_comps_bytes(gw, gh) / sizeof(unsigned) + 1);
        std::vector<BlobRecord> blobs(24);
        std::vector<uint8_t> lab_out(n);

        AnalyzeScratch sc;
        sc.grid = g.data();
        sc.labels = labels.data();
        sc.stack = stack.data();
        sc.remap = remap.data();
        sc.comps = comps_bytes.data();
        sc.comps_bytes = comps_bytes.size() * sizeof(unsigned);
        sc.samples = samples.data();

        AnalyzeParams ap;
        ap.fx = 700.0f;
        ap.fy = 700.0f;
        ap.cx = 320.0f;
        ap.cy = 180.0f;
        ap.max_blobs = 24;
        ap.min_cells = 40;

        const float center[4] = {0.475f, 0.455f, 0.525f, 0.545f};
        AnalyzeResult res;
        const bool ok = analyze_depth(s.depth.data(), w, w, h, ap, center, 0.5f, blobs.data(), 24,
                                      lab_out.data(), &sc, &res);
        report("analyze_depth 成功", ok && res.blob_count == 3);
        std::snprintf(buf, sizeof buf, "%d 个（期望 3）", res.blob_count);
        report("找到 3 个面块", ok && res.blob_count == 3, buf);

        // 块内恒定 → labels 里每个保留块的格子数必须等于 blob.cells，且深度等于块深度
        int label_mismatch = 0;
        for (int k = 0; k < res.blob_count; ++k) {
            int cells = 0;
            double sum = 0;
            for (size_t i = 0; i < n; ++i) {
                if (lab_out[i] == static_cast<uint8_t>(blobs[k].identifier)) {
                    ++cells;
                    sum += grid.g[i];
                }
            }
            if (cells != blobs[k].cells) ++label_mismatch;
            // 容差 1e-4：库内部是按洪泛访问顺序做 float32 累加，这里按行序做 double 累加，
            // 两者最后的位不同（实测 1.2 m 那块差 1.7e-5）。差一个数量级就说明真的错了。
            if (!near(static_cast<float>(sum / cells), blobs[k].distance, 1e-4f)) ++label_mismatch;
        }
        report("labels 与 blobs 的格子数/深度自洽", label_mismatch == 0);

        // 顺序：cell 数降序；id 从 1 连续
        bool ordered = true;
        for (int k = 0; k < res.blob_count; ++k) {
            if (blobs[k].identifier != k + 1) ordered = false;
            if (k > 0 && blobs[k].cells > blobs[k - 1].cells) ordered = false;
        }
        report("面块按大小降序、id 连续", ordered);

        // 包围盒必须落在 [0,1] 内且非空
        bool boxed = true;
        for (int k = 0; k < res.blob_count; ++k) {
            const float* r = blobs[k].rect;
            if (!(r[0] >= 0 && r[1] >= 0 && r[2] > 0 && r[3] > 0 && r[0] + r[2] <= 1.001f &&
                  r[1] + r[3] <= 1.001f)) {
                boxed = false;
            }
        }
        report("归一化包围盒合法", boxed);

        // 最大块应该就是左边那个近的（0.50 m），且物理宽度 = wpx*z/fx
        if (res.blob_count == 3) {
            const BlobRecord& big = blobs[0];
            const float expect_width =
                static_cast<float>(w / 2 - w / 10) * big.distance / ap.fx;  // 像素宽 × z / fx
            std::snprintf(buf, sizeof buf, "z=%.3f w=%.4f（期望 ~%.4f）", static_cast<double>(big.distance),
                          static_cast<double>(big.width), static_cast<double>(expect_width));
            report("最大块的深度与物理宽度正确",
                   near(big.distance, 0.50f, 1e-5f) && near(big.width, expect_width, 0.01f), buf);
        }

        // 中心读数：准星那块（0.475..0.525 × 0.455..0.545）落在左边近块的范围内 → 0.50
        float want_center = 0;
        const bool want_ok = ref_quantile(s.depth, w, h, center[0], center[1], center[2], center[3],
                                          0.5f, &want_center);
        std::snprintf(buf, sizeof buf, "%.3f（期望 %.3f）", static_cast<double>(res.center),
                      static_cast<double>(want_center));
        report("准星中位数与暴力排序一致",
               res.has_center && want_ok && res.center == want_center, buf);

        // 中心全 0 → has_center = false
        {
            Scene empty;
            empty.w = w;
            empty.h = h;
            empty.depth.assign(static_cast<size_t>(w) * h, 0.0f);
            AnalyzeResult r2;
            const bool ok2 = analyze_depth(empty.depth.data(), w, w, h, ap, center, 0.5f,
                                           blobs.data(), 24, lab_out.data(), &sc, &r2);
            report("全空深度图：0 个面块且 has_center=false",
                   ok2 && r2.blob_count == 0 && !r2.has_center);
        }

        // min_cells 过滤：把小块的阈值提到比最大块还大 → 一个都不留，labels 全 0
        {
            AnalyzeParams strict = ap;
            strict.min_cells = 1 << 20;
            AnalyzeResult r3;
            std::vector<uint8_t> lab3(n, 7);
            const bool ok3 = analyze_depth(s.depth.data(), w, w, h, strict, center, 0.5f,
                                           blobs.data(), 24, lab3.data(), &sc, &r3);
            int nonzero = 0;
            for (uint8_t v : lab3) nonzero += (v != 0);
            report("min_cells 过滤后 labels 全 0", ok3 && r3.blob_count == 0 && nonzero == 0);
        }

        // 缓冲校验：comps 不够大要拒绝，不能越界写
        {
            AnalyzeScratch small = sc;
            small.comps_bytes = 16;
            AnalyzeResult r4;
            report("comps 缓冲不足 -> 拒绝",
                   !analyze_depth(s.depth.data(), w, w, h, ap, center, 0.5f, blobs.data(), 24,
                                  lab_out.data(), &small, &r4));
            AnalyzeScratch no_remap = sc;
            no_remap.remap = nullptr;
            report("remap 为 NULL -> 拒绝",
                   !analyze_depth(s.depth.data(), w, w, h, ap, center, 0.5f, blobs.data(), 24,
                                  lab_out.data(), &no_remap, &r4));
            report("blobs 容量不足 -> 拒绝",
                   !analyze_depth(s.depth.data(), w, w, h, ap, center, 0.5f, blobs.data(), 1,
                                  lab_out.data(), &sc, &r4));
        }

        // 容差语义：两块深度差小于 0.02+0.04z 时应该连成一块
        {
            Scene merge;
            merge.w = w;
            merge.h = h;
            merge.depth.assign(static_cast<size_t>(w) * h, 0.0f);
            merge.add(100, 100, 300, 250, 1.000f);
            merge.add(100, 250, 300, 300, 1.015f);   // 差 0.015 < 0.02+0.04*1.0
            AnalyzeResult r5;
            const bool ok5 = analyze_depth(merge.depth.data(), w, w, h, ap, nullptr, 0.5f,
                                           blobs.data(), 24, lab_out.data(), &sc, &r5);
            report("深度接近的相邻块合并成一个", ok5 && r5.blob_count == 1);
        }
        {
            Scene split;
            split.w = w;
            split.h = h;
            split.depth.assign(static_cast<size_t>(w) * h, 0.0f);
            // 中间留一条无效带：否则边界的网格块会把两边平均成一个中间值，
            // 顺着容差链把两块连通（那是算法的正确行为，不是 bug）
            split.add(100, 100, 300, 240, 1.000f);
            split.add(100, 270, 300, 340, 1.100f);
            AnalyzeResult r6;
            const bool ok6 = analyze_depth(split.depth.data(), w, w, h, ap, nullptr, 0.5f,
                                           blobs.data(), 24, lab_out.data(), &sc, &r6);
            report("深度差得远的相邻块保持分开", ok6 && r6.blob_count == 2);
        }

        // 8 连通不该合并：斜对角接触的两块在 4 邻域下必须分开
        {
            Scene diag;
            diag.w = w;
            diag.h = h;
            diag.depth.assign(static_cast<size_t>(w) * h, 0.0f);
            // 只有对角相接 → 4 邻域分开。每块都要够 40 格（step=4 → 每块 20×20 px = 25 格）
            diag.add(40, 40, 120, 120, 0.5f);
            diag.add(120, 120, 200, 200, 2.0f);
            AnalyzeResult r7;
            const bool ok7 = analyze_depth(diag.depth.data(), w, w, h, ap, nullptr, 0.5f,
                                           blobs.data(), 24, lab_out.data(), &sc, &r7);
            report("对角相接按 4 邻域算两块", ok7 && r7.blob_count == 2);
        }
    }

    // ---------------- 3) C 接口一致性 ----------------
    {
        const int w = 320, h = 240;
        Scene s = make_scene(w, h);
        stereo_context* ctx = stereo_create();
        report("stereo_create", ctx != nullptr);

        int cgw = 0, cgh = 0;
        const bool grid_ok = stereo_analyze_grid(w, h, &cgw, &cgh) == 0 && cgw == 160 && cgh == 120;
        report("stereo_analyze_grid（320 宽 → 160 列）", grid_ok);

        // 分位数：非 0 返回 = 有效样本不足
        float v = 0;
        const bool sample_ok = stereo_sample_depth(ctx, s.depth.data(), w, w, h, 0.1f, 0.1f, 0.4f,
                                                   0.5f, 0.5f, &v) == 0;
        report("stereo_sample_depth 成功", sample_ok && near(v, 0.50f, 1e-6f));
        report("stereo_sample_depth 对空区域返回 1",
               stereo_sample_depth(ctx, s.depth.data(), w, w, h, 0.0f, 0.0f, 0.01f, 0.01f, 0.5f,
                                   &v) == 1);
        report("stereo_sample_depth 对反向矩形返回 -1",
               stereo_sample_depth(ctx, s.depth.data(), w, w, h, 0.5f, 0.0f, 0.4f, 1.0f, 0.5f,
                                   &v) == -1);

        // 分析：C 接口 vs C++ 接口逐字段一致
        stereo_analyze_params p;
        p.fx = 700;
        p.fy = 700;
        p.cx = 160;
        p.cy = 120;
        p.max_blobs = 24;
        p.min_cells = 40;
        static const float center[4] = {0.475f, 0.455f, 0.525f, 0.545f};
        stereo_blob cblobs[24];
        std::vector<uint8_t> clabels(static_cast<size_t>(cgw) * cgh);
        stereo_analysis ca;
        std::memset(&ca, 0, sizeof ca);
        const bool a_ok = stereo_analyze(ctx, s.depth.data(), w, w, h, &p, center, 0.5f, cblobs, 24,
                                         clabels.data(), &ca) == 0;
        report("stereo_analyze 成功", a_ok && ca.blob_count == 3);

        // 自己再跑一遍 C++ 版本对比
        int gw = 0, gh = 0;
        analyze_grid_size(w, h, &gw, &gh);
        const size_t n = static_cast<size_t>(gw) * gh;
        std::vector<float> g2(n);
        std::vector<int> l2(n), s2(n), r2v(n + 1);
        std::vector<unsigned> cb(analyze_comps_bytes(gw, gh) / 4 + 1);
        std::vector<BlobRecord> b2(24);
        std::vector<uint8_t> lab2(n);
        std::vector<float> samples3(kSampleScratchFloats);
        AnalyzeScratch sc;
        sc.grid = g2.data();
        sc.labels = l2.data();
        sc.stack = s2.data();
        sc.remap = r2v.data();
        sc.comps = cb.data();
        sc.comps_bytes = cb.size() * 4;
        sc.samples = samples3.data();
        AnalyzeParams ap;
        ap.fx = p.fx;
        ap.fy = p.fy;
        ap.cx = p.cx;
        ap.cy = p.cy;
        ap.max_blobs = p.max_blobs;
        ap.min_cells = p.min_cells;
        AnalyzeResult r;
        const bool b_ok = analyze_depth(s.depth.data(), w, w, h, ap, center, 0.5f, b2.data(), 24,
                                        lab2.data(), &sc, &r);
        int diff = 0;
        for (int k = 0; k < r.blob_count && b_ok; ++k) {
            diff += (b2[k].identifier != cblobs[k].identifier);
            diff += (b2[k].cells != cblobs[k].cells);
            diff += std::memcmp(&b2[k].distance, &cblobs[k].distance, 4) != 0;
            diff += std::memcmp(&b2[k].x, &cblobs[k].x, 4) != 0;
            diff += std::memcmp(&b2[k].width, &cblobs[k].width, 4) != 0;
            for (int j = 0; j < 4; ++j) {
                diff += std::memcmp(&b2[k].rect[j], &cblobs[k].rect[j], 4) != 0;
            }
        }
        diff += (r.grid_width != ca.grid_width) || (r.grid_height != ca.grid_height);
        diff += std::memcmp(&r.center, &ca.center, 4) != 0;
        diff += std::memcmp(lab2.data(), clabels.data(), n) != 0;
        char buf[96];
        std::snprintf(buf, sizeof buf, "%d 处不同", diff);
        report("C 接口与 C++ 逐字段/逐字节一致", b_ok && diff == 0, buf);

        stereo_destroy(ctx);
    }

    std::printf(g_fail ? "\n有 %d 项不一致\n" : "\n全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
