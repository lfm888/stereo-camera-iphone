// sgm.cpp — Census / SGM / WTA / 深度的标量参考实现
//
// 逐条对应 Shaders.metal 的 kernel（见每个函数上方的注释）。刻意保持"笨"：
// 先正确、再快。向量化与多线程在通过 tools/gen_golden.py 的逐位对拍之后再加。

#include "stereo/sgm.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace stereo {
namespace {

/// SGM 路径累加器两端的哨兵（视差越界）：0x3FFFFFFF，与 kernel 一致。
constexpr unsigned kSentinel = 0x3FFFFFFFu;

/// 路径累加的上限（ushort 饱和）。
constexpr unsigned kSumMax = 65535u;

/// 亮度权重，与 Shaders.metal 的 kLuma 一致。
constexpr float kLumaR = 0.299f;
constexpr float kLumaG = 0.587f;
constexpr float kLumaB = 0.114f;

/// census 的中心阈值偏置，抑制平坦区的噪声抖动。
constexpr float kCensusBias = 0.004f;

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace

void rgb_to_luma(const uint8_t* rgb, int w, int h, int stride_bytes, float* luma) {
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = rgb + static_cast<size_t>(y) * stride_bytes;
        float* dst = luma + static_cast<size_t>(y) * w;
        for (int x = 0; x < w; ++x) {
            const uint8_t* p = row + x * 3;
            // 注意：真实相机 buffer 是 BGRA，喂进来之前要把通道顺序理成 R,G,B
            // （Metal 的 bgra8Unorm 采样器会自动重排，C++ 不会）。
            // 用除法而不是乘 1/255f：numpy 是 arr/255.0，两者舍入不同（差 1 ULP）。
            const float r = static_cast<float>(p[0]) / 255.0f;
            const float g = static_cast<float>(p[1]) / 255.0f;
            const float b = static_cast<float>(p[2]) / 255.0f;
            dst[x] = r * kLumaR + g * kLumaG + b * kLumaB;
        }
    }
}

void census(const float* luma, int w, int h, uint64_t* out) {
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float c = luma[static_cast<size_t>(y) * w + x] - kCensusBias;
            uint64_t bits = 0;
            int k = 0;
            for (int dy = -3; dy <= 3; ++dy) {
                const int ys = clampi(y + dy, 0, h - 1);
                const float* row = luma + static_cast<size_t>(ys) * w;
                for (int dx = -3; dx <= 3; ++dx) {
                    if (dx == 0 && dy == 0) continue;
                    const int xs = clampi(x + dx, 0, w - 1);
                    if (row[xs] < c) bits |= (uint64_t{1} << k);
                    ++k;
                }
            }
            out[static_cast<size_t>(y) * w + x] = bits;  // k == 48
        }
    }
}

void cost_volume(const uint64_t* cl, const uint64_t* cr, const SgmParams& p, uint8_t* C) {
    const int W = p.W, H = p.H, D = p.D;
    std::memset(C, kCostInvalid, static_cast<size_t>(W) * H * D);
    for (int y = 0; y < H; ++y) {
        const size_t row = static_cast<size_t>(y) * W;
        for (int x = 0; x < W; ++x) {
            const uint64_t l = cl[row + x];
            uint8_t* out = C + (row + x) * D;
            for (int d = 0; d < D; ++d) {
                const int xr = x - p.dir * d;
                if (xr < 0 || xr >= W) continue;  // 越界保持 48（不是 break）
                out[d] = static_cast<uint8_t>(popcount64(l ^ cr[row + xr]));
            }
        }
    }
}

void sgm_path(const uint8_t* C, uint16_t* S, const float* luma,
              const SgmParams& p, int dx, int dy, bool first) {
    const int W = p.W, H = p.H, D = p.D;
    const bool horizontal = dx != 0;
    const int n_lines = horizontal ? H : W;
    const int steps = horizontal ? W : H;

    // prev[] 的两端（index 0 与 D+1）永远是哨兵，等价于"视差越界"。
    std::vector<unsigned> prev(static_cast<size_t>(D) + 2, kSentinel);
    std::vector<unsigned> cur(static_cast<size_t>(D), 0);

    for (int line = 0; line < n_lines; ++line) {
        int x = horizontal ? (dx > 0 ? 0 : W - 1) : line;
        int y = horizontal ? line : (dy > 0 ? 0 : H - 1);
        std::fill(prev.begin(), prev.end(), kSentinel);
        unsigned prev_min = 0;
        float Ip = 0.0f;

        for (int s = 0; s < steps; ++s) {
            const size_t pix = static_cast<size_t>(y) * W + x;
            const uint8_t* c_row = C + pix * D;
            uint16_t* s_row = S + pix * D;
            const float I = luma[pix];

            if (s == 0) {
                for (int d = 0; d < D; ++d) cur[d] = c_row[d];
            } else {
                const unsigned pm = prev_min;
                // P2' = max(P1+1, (unsigned)((float)P2 / (1 + 16*|I - Ip|)))
                // 先截断再取 max，顺序与 kernel/numpy 一致。
                const float denom = 1.0f + std::fabs(I - Ip) * 16.0f;
                unsigned p2eff = static_cast<unsigned>(static_cast<float>(p.P2) / denom);
                const unsigned p1min = p.P1 + 1u;
                if (p2eff < p1min) p2eff = p1min;
                const unsigned p2term = pm + p2eff;

                for (int d = 0; d < D; ++d) {
                    const unsigned lo = prev[d];      // L[d-1]
                    const unsigned mid = prev[d + 1]; // L[d]
                    const unsigned hi = prev[d + 2];  // L[d+1]
                    const unsigned inner = std::min(lo, hi) + p.P1;
                    unsigned m = std::min(mid, inner);
                    m = std::min(m, p2term);
                    cur[d] = c_row[d] + m - pm;
                }
            }

            for (int d = 0; d < D; ++d) {
                const unsigned L = cur[d];
                if (first) {
                    s_row[d] = static_cast<uint16_t>(L);
                } else {
                    const unsigned sum = static_cast<unsigned>(s_row[d]) + L;
                    s_row[d] = static_cast<uint16_t>(std::min(sum, kSumMax));
                }
                prev[d + 1] = L;  // barrier 之后整体写回，等价于先算完再赋值
            }

            unsigned mn = cur[0];
            for (int d = 1; d < D; ++d) mn = std::min(mn, cur[d]);
            prev_min = mn;
            Ip = I;

            x += dx;
            y += dy;
        }
    }
}

void sgm_all_paths(const uint8_t* C, uint16_t* S, const float* luma, const SgmParams& p) {
    static const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    // 先清零，保证第一条路径 first=true 时 SUM 的"覆盖写"与从零累加等价
    std::memset(S, 0, static_cast<size_t>(p.W) * p.H * p.D * sizeof(uint16_t));
    for (int i = 0; i < 4; ++i) {
        sgm_path(C, S, luma, p, dirs[i][0], dirs[i][1], i == 0);
    }
}

void wta_right(const uint16_t* S, const SgmParams& p, float* dispR) {
    const int W = p.W, H = p.H, D = p.D;
    for (int y = 0; y < H; ++y) {
        const size_t row = static_cast<size_t>(y) * W;
        for (int x = 0; x < W; ++x) {
            unsigned best = 0xFFFFFFFFu;
            int bd = 0;
            for (int d = 0; d < D; ++d) {
                const int xl = x + p.dir * d;
                if (xl < 0 || xl >= W) break;
                const unsigned c = S[(row + xl) * D + d];
                if (c < best) { best = c; bd = d; }
            }
            dispR[row + x] = static_cast<float>(bd);
        }
    }
}

void wta_left(const uint16_t* S, const float* dispR, const SgmParams& p, float* disp) {
    const int W = p.W, H = p.H, D = p.D;
    const float uniq = 1.0f + p.uniqueness;
    for (int y = 0; y < H; ++y) {
        const size_t row = static_cast<size_t>(y) * W;
        for (int x = 0; x < W; ++x) {
            const uint16_t* s = S + (row + x) * D;

            int count = 0;
            for (int d = 0; d < D; ++d) {
                const int xr = x - p.dir * d;
                if (xr < 0 || xr >= W) break;
                count = d + 1;
            }
            if (count < 3) { disp[row + x] = kInvalidDisp; continue; }

            int bd = 0;
            for (int d = 1; d < count; ++d) {
                if (s[d] < s[bd]) bd = d;
            }
            unsigned second = 0xFFFFFFFFu;
            for (int d = 0; d < count; ++d) {
                if ((d > bd ? d - bd : bd - d) > 1) second = std::min(second, static_cast<unsigned>(s[d]));
            }
            const float best = static_cast<float>(s[bd]);
            bool ok = bd > 0 && bd < count - 1 && best * uniq < static_cast<float>(second);
            if (ok && p.lrCheck) {
                const float dr = dispR[row + (x - p.dir * bd)];
                ok = std::fabs(dr - static_cast<float>(bd)) <= 1.0f;
            }

            float sub = 0.0f;
            if (ok) {
                const float c0 = static_cast<float>(s[bd - 1]);
                const float c2 = static_cast<float>(s[bd + 1]);
                const float den = c0 - 2.0f * best + c2;
                if (den > 0.0f) {
                    sub = 0.5f * (c0 - c2) / den;
                    sub = sub < -0.5f ? -0.5f : (sub > 0.5f ? 0.5f : sub);
                }
            }
            disp[row + x] = ok ? static_cast<float>(bd) + sub : kInvalidDisp;
        }
    }
}

void to_depth(const float* disp, int w, int h, float fB, float minZ, float maxZ, float* depth) {
    to_depth(disp, w, w, h, fB, minZ, maxZ, depth, w);
}

void to_depth(const float* disp, int disp_stride, int w, int h, float fB, float minZ, float maxZ,
              float* depth, int depth_stride) {
    if (disp_stride <= 0) disp_stride = w;
    if (depth_stride <= 0) depth_stride = w;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float v[9];
            int n = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                const int ys = clampi(y + dy, 0, h - 1);
                for (int dx = -1; dx <= 1; ++dx) {
                    const int xs = clampi(x + dx, 0, w - 1);
                    const float d = disp[static_cast<size_t>(ys) * static_cast<size_t>(disp_stride) + xs];
                    if (d > 0.0f) v[n++] = d;
                }
            }
            float z = kInvalidDepth;
            if (n >= 5) {
                // 插入排序：与 kernel 完全一致的升序，中值取 v[n/2]
                for (int i = 1; i < n; ++i) {
                    const float key = v[i];
                    int j = i - 1;
                    while (j >= 0 && v[j] > key) { v[j + 1] = v[j]; --j; }
                    v[j + 1] = key;
                }
                z = fB / v[n / 2];
                if (z < minZ || z > maxZ) z = kInvalidDepth;
            }
            depth[static_cast<size_t>(y) * static_cast<size_t>(depth_stride) + x] = z;
        }
    }
}

void match_luma(const float* luma_l, const float* luma_r, const SgmParams& p,
                const MatchScratch& sc, float* disp, int disp_stride) {
    census(luma_l, p.W, p.H, sc.census_l);
    census(luma_r, p.W, p.H, sc.census_r);
    cost_volume(sc.census_l, sc.census_r, p, sc.cost);
    sgm_all_paths(sc.cost, sc.sum, luma_l, p);

    // wta_right 的结果是 wta_left 的输入（左右一致性检查），两张图不能是同一块缓冲区。
    wta_right(sc.sum, p, sc.disp_r);
    wta_left(sc.sum, sc.disp_r, p, sc.disp_l);

    // 输出到调用方的缓冲区（可能带行跨度）。逐行 memcpy，不做逐像素运算。
    const int W = p.W, H = p.H;
    if (disp_stride <= 0) disp_stride = W;
    for (int y = 0; y < H; ++y) {
        std::memcpy(disp + static_cast<size_t>(y) * static_cast<size_t>(disp_stride),
                    sc.disp_l + static_cast<size_t>(y) * static_cast<size_t>(W),
                    static_cast<size_t>(W) * sizeof(float));
    }
}

}  // namespace stereo
