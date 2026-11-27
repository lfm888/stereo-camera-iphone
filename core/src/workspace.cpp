// workspace.cpp — 跨帧缓冲的 ensure / scratch（语义逐条对齐改造前的 stereo_context::ensure*）
//
// 唯一的要求：尺寸不变时**不重新分配**，这样第一帧之后稳态零分配。
// 内存不足一律返回 false（不抛异常）。

#include "stereo/workspace.hpp"

#include "stereo/sample.hpp"   // kSampleScratchFloats

#include <new>

namespace stereo {

bool MatchBuffers::ensure(int w, int h, int d) {
    if (W == w && H == h && D == d) return true;
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    const size_t nc = n * static_cast<size_t>(d);
    try {
        luma_l.assign(n, 0.0f);
        luma_r.assign(n, 0.0f);
        census_l.assign(n, 0);
        census_r.assign(n, 0);
        cost.assign(nc, 0);
        sum.assign(nc, 0);
        disp_r.assign(n, 0.0f);
        disp_l.assign(n, 0.0f);
    } catch (const std::bad_alloc&) {
        return false;
    }
    W = w;
    H = h;
    D = d;
    return true;
}

MatchScratch MatchBuffers::scratch() {
    MatchScratch sc;
    sc.luma_l = luma_l.data();
    sc.luma_r = luma_r.data();
    sc.census_l = census_l.data();
    sc.census_r = census_r.data();
    sc.cost = cost.data();
    sc.sum = sum.data();
    sc.disp_r = disp_r.data();
    sc.disp_l = disp_l.data();
    return sc;
}

bool AnalyzeBuffers::ensure(int grid_w, int grid_h) {
    if (gw == grid_w && gh == grid_h && !grid.empty()) return true;
    const size_t n = static_cast<size_t>(grid_w) * static_cast<size_t>(grid_h);
    try {
        grid.assign(n, 0.0f);
        labels.assign(n, 0);
        stack.assign(n, 0);
        remap.assign(n + 1, 0);
        comps.assign(analyze_comps_bytes(grid_w, grid_h) / sizeof(unsigned) + 1, 0u);
        samples.assign(static_cast<size_t>(kSampleScratchFloats), 0.0f);
    } catch (const std::bad_alloc&) {
        gw = gh = 0;
        return false;
    }
    gw = grid_w;
    gh = grid_h;
    return true;
}

AnalyzeScratch AnalyzeBuffers::scratch() {
    AnalyzeScratch sc;
    sc.grid = grid.data();
    sc.labels = labels.data();
    sc.stack = stack.data();
    sc.remap = remap.data();
    sc.comps = comps.data();
    sc.comps_bytes = comps.size() * sizeof(unsigned);
    sc.samples = samples.data();
    return sc;
}

bool CalibrateBuffers::ensure(int w, int h) {
    if (W == w && H == h) return true;
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    try {
        rect_l.assign(n * 4, 0);
        rect_r.assign(n * 4, 0);
        luma_l.assign(n, 0.0f);
        luma_r.assign(n, 0.0f);
        census_l.assign(n, 0);
        census_r.assign(n, 0);
    } catch (const std::bad_alloc&) {
        W = H = 0;
        return false;
    }
    W = w;
    H = h;
    return true;
}

CalibrateScratch CalibrateBuffers::scratch() {
    CalibrateScratch sc;
    sc.rect_l = rect_l.data();
    sc.rect_r = rect_r.data();
    sc.luma_l = luma_l.data();
    sc.luma_r = luma_r.data();
    sc.census_l = census_l.data();
    sc.census_r = census_r.data();
    return sc;
}

bool FrameBuffers::ensure(int w, int h) {
    if (W == w && H == h && !rect_l.empty()) return true;
    const size_t rect_bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    try {
        rect_l.assign(rect_bytes, 0);
        rect_r.assign(rect_bytes, 0);
        disp.assign(n, 0.0f);
        depth_rect.assign(n, 0.0f);
    } catch (const std::bad_alloc&) {
        W = H = 0;
        return false;
    }
    W = w;
    H = h;
    return true;
}

bool FrameBuffers::ensure_input(int w, int h) {
    if (src_w == w && src_h == h && !in_l.empty()) return true;
    const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
    try {
        in_l.assign(bytes, 0);
        in_r.assign(bytes, 0);
    } catch (const std::bad_alloc&) {
        src_w = src_h = 0;
        return false;
    }
    src_w = w;
    src_h = h;
    return true;
}

bool FrameBuffers::ensure_blobs(int n) {
    if (n <= 0) return true;
    if (static_cast<int>(blobs.size()) >= n) return true;
    try {
        blobs.resize(static_cast<size_t>(n));
    } catch (const std::bad_alloc&) {
        return false;
    }
    return true;
}

ImageView view_of(const stereo_image& img) {
    ImageView v;
    v.data = img.data;
    v.width = img.width;
    v.height = img.height;
    v.stride = img.stride;
    v.format = img.format;
    return v;
}

MutableImageView mutable_view_of(stereo_image& img) {
    MutableImageView v;
    v.data = const_cast<uint8_t*>(img.data);
    v.width = img.width;
    v.height = img.height;
    v.stride = img.stride;
    v.format = img.format;
    return v;
}

}  // namespace stereo
