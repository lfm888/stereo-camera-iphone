// stereo/calibrate.cpp — 自动标定搜索
//
// 逐步对应改造前的 Swift 实现（StereoEngine.calibrate）：
//   2 个 flip × 25 个 dy（-12..12 整数）+ 4 次 ±0.25/±0.5 细化，
//   每次打分 = 两路重采样 → 灰度 → Census → 逐视差汉明距离取最小 → 全图平均。
//
// 三处与原版不同，都是为 CPU 成本做的取舍（见头文件）：
//   * 重采样 1 次采样（不是 4 次超采样）
//   * 像素按 step 抽样
//   * 不做 5×5 窗口聚合

#include "stereo/calibrate.hpp"

#include "stereo/sgm.hpp"

#include <cmath>
#include <limits>

namespace stereo {
namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();

/// 双线性取样（clamp_to_zero），单次采样。像素坐标语义与 rectify.hpp 一致：
/// 纹素中心在 +0.5，越界贡献 0。
inline void sample_linear(const uint8_t* src, int w, int h, int stride, float px, float py,
                          float acc[4]) {
    const float tx = px - 0.5f;
    const float ty = py - 0.5f;
    const int x0 = static_cast<int>(std::floor(tx));
    const int y0 = static_cast<int>(std::floor(ty));
    const float fx = tx - static_cast<float>(x0);
    const float fy = ty - static_cast<float>(y0);
    for (int j = 0; j < 2; ++j) {
        const float wy = j ? fy : (1.0f - fy);
        if (wy == 0.0f) continue;
        const int yy = y0 + j;
        if (yy < 0 || yy >= h) continue;
        for (int i = 0; i < 2; ++i) {
            const float wgt = (i ? fx : (1.0f - fx)) * wy;
            if (wgt == 0.0f) continue;
            const int xx = x0 + i;
            if (xx < 0 || xx >= w) continue;
            const uint8_t* p = src + static_cast<size_t>(yy) * static_cast<size_t>(stride) + xx * 4;
            for (int c = 0; c < 4; ++c) acc[c] += static_cast<float>(p[c]) * wgt;
        }
    }
}

/// 单应重采样到 dst（BGRA8，1 次采样）。打分专用。
bool rectify_fast(const ImageView& src, const Mat3& m, uint8_t* dst, int w, int h) {
    if (src.data == nullptr || dst == nullptr || w <= 0 || h <= 0) return false;
    const int stride = w * 4;
    for (int y = 0; y < h; ++y) {
        uint8_t* out = dst + static_cast<size_t>(y) * static_cast<size_t>(stride);
        for (int x = 0; x < w; ++x) {
            const float px = static_cast<float>(x) + 0.5f;
            const float py = static_cast<float>(y) + 0.5f;
            const float qz = m.m[6] * px + m.m[7] * py + m.m[8];
            float acc[4] = {0, 0, 0, 0};
            if (std::fabs(qz) > 1e-12f) {
                const float sx = (m.m[0] * px + m.m[1] * py + m.m[2]) / qz;
                const float sy = (m.m[3] * px + m.m[4] * py + m.m[5]) / qz;
                if (std::isfinite(sx) && std::isfinite(sy)) {
                    sample_linear(src.data, src.width, src.height, src.stride, sx, sy, acc);
                }
            }
            uint8_t* p = out + x * 4;
            for (int c = 0; c < 3; ++c) {
                const float v = acc[c];
                p[c] = v <= 0.0f ? 0 : (v >= 255.0f ? 255 : static_cast<uint8_t>(std::lrintf(v)));
            }
            p[3] = 255;
        }
    }
    return true;
}

/// BGRA8 → 灰度。系数与 sgm.cpp 的 rgb_to_luma 一致（这里输入是 BGRA，顺序反过来取）。
void luma_from_bgra(const uint8_t* src, int w, int h, float* luma) {
    const int stride = w * 4;
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = src + static_cast<size_t>(y) * static_cast<size_t>(stride);
        float* dst = luma + static_cast<size_t>(y) * static_cast<size_t>(w);
        for (int x = 0; x < w; ++x) {
            const uint8_t* p = row + x * 4;
            const float b = static_cast<float>(p[0]) / 255.0f;
            const float g = static_cast<float>(p[1]) / 255.0f;
            const float r = static_cast<float>(p[2]) / 255.0f;
            dst[x] = r * 0.299f + g * 0.587f + b * 0.114f;
        }
    }
}

/// 一个候选 (flip, dy) 的平均 WTA 代价。
float score_candidate(const RectifyInput& base, int flip, float dy, const ImageView& wide,
                      const ImageView& ultra, const CalibrateParams& p,
                      const CalibrateScratch& s) {
    RectifyInput in = base;
    in.flip = flip ? 1 : 0;
    in.dy = dy;

    RectifyOutput g;
    if (!rectify_geometry(in, &g)) return kInf;

    const int W = in.work_width;
    const int H = in.work_height;
    if (W <= 0 || H <= 0) return kInf;

    if (!rectify_fast(wide, g.to_wide_image, s.rect_l, W, H)) return kInf;
    if (!rectify_fast(ultra, g.to_ultra_image, s.rect_r, W, H)) return kInf;

    luma_from_bgra(s.rect_l, W, H, s.luma_l);
    luma_from_bgra(s.rect_r, W, H, s.luma_r);

    census(s.luma_l, W, H, s.census_l);
    census(s.luma_r, W, H, s.census_r);

    const int D = p.max_disp;
    const int dir = g.dir;
    int step = p.pixel_step;
    if (step < 1) step = 1;

    double sum = 0.0;
    long long count = 0;
    for (int y = 0; y < H; y += step) {
        const size_t row = static_cast<size_t>(y) * static_cast<size_t>(W);
        for (int x = 0; x < W; x += step) {
            int best = kCostInvalid;
            for (int d = 0; d < D; ++d) {
                const int xr = x - dir * d;
                if (xr < 0 || xr >= W) continue;
                const int c = popcount64(s.census_l[row + x] ^ s.census_r[row + xr]);
                if (c < best) best = c;
            }
            if (best < kCostInvalid) {
                sum += best;
                ++count;
            }
        }
    }
    if (count == 0) return kInf;
    return static_cast<float>(sum / static_cast<double>(count));
}

/// CPU 全流水线打分用的上下文。
struct CpuScoreContext {
    const RectifyInput* base;
    const ImageView* wide;
    const ImageView* ultra;
    const CalibrateParams* params;
    const CalibrateScratch* scratch;
};

float cpu_score(void* user, int flip, float dy) {
    const CpuScoreContext* c = static_cast<const CpuScoreContext*>(user);
    return score_candidate(*c->base, flip, dy, *c->wide, *c->ultra, *c->params, *c->scratch);
}

}  // namespace

bool calibrate_search(CalibrateScoreFn score, void* user, const CalibrateParams& params,
                      CalibrateResult* out) {
    if (score == nullptr || out == nullptr) return false;
    if (params.max_disp < 1) return false;

    CalibrateResult best;
    best.flip = params.flip0 ? 1 : 0;
    best.dy = params.dy0;
    best.score = kInf;
    best.evaluations = 0;

    // 1) 粗搜：两个 flip × dy ∈ [-12, 12] 整数（与改造前的 Swift 版一致）
    for (int flip = 0; flip <= 1; ++flip) {
        for (int i = -12; i <= 12; ++i) {
            const float v = score(user, flip, static_cast<float>(i));
            ++best.evaluations;
            if (v < best.score) {
                best.flip = flip;
                best.dy = static_cast<float>(i);
                best.score = v;
            }
        }
    }

    // 2) 细化：在最优点附近试 ±0.5 / ±0.25（顺序与 Swift 版一致）
    const float deltas[4] = {-0.5f, -0.25f, 0.25f, 0.5f};
    for (int k = 0; k < 4; ++k) {
        const float v = score(user, best.flip, best.dy + deltas[k]);
        ++best.evaluations;
        if (v < best.score) {
            best.dy += deltas[k];
            best.score = v;
        }
    }

    if (!std::isfinite(best.score)) return false;
    *out = best;
    return true;
}

bool calibrate_rectification(const RectifyInput& base, const ImageView& wide,
                             const ImageView& ultra, const CalibrateParams& params,
                             CalibrateScratch* scratch, CalibrateResult* out) {
    if (scratch == nullptr || out == nullptr) return false;
    if (scratch->rect_l == nullptr || scratch->rect_r == nullptr || scratch->luma_l == nullptr ||
        scratch->luma_r == nullptr || scratch->census_l == nullptr || scratch->census_r == nullptr) {
        return false;
    }
    if (params.max_disp < 1 || base.work_width <= 0 || base.work_height <= 0) return false;
    if (wide.data == nullptr || ultra.data == nullptr) return false;

    CpuScoreContext ctx;
    ctx.base = &base;
    ctx.wide = &wide;
    ctx.ultra = &ultra;
    ctx.params = &params;
    ctx.scratch = scratch;
    return calibrate_search(cpu_score, &ctx, params, out);
}

}  // namespace stereo
