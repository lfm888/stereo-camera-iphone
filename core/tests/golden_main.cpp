// golden_main.cpp — C++ 标量实现 vs tools/gen_golden.py 生成的 .npy 金标准
//
// 用法：
//     golden_test [golden_dir]        # 默认 tests/golden
//
// 分两级：
//   A) 逐 kernel 对拍：用**存档的 luma** 当输入，把 Census/SGM/WTA/深度的
//      整数中间量做逐位比较（这一级不允许任何差异）。
//   B) 整链对拍：从 left_u8/right_u8 出发，自己算 luma，再跑一遍完整管线，
//      与存档的 disp/depth 比（检验 rgb_to_luma 的实现是否与 numpy 一致）。

#include "stereo/sgm.hpp"
#include "stereo/stereo_c_api.h"
#include "npy.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int g_fail = 0;

void report(const char* name, bool ok, const std::string& detail = {}) {
    std::printf("  %-22s %s%s%s\n", name, ok ? "OK  " : "FAIL",
                detail.empty() ? "" : "  ", detail.c_str());
    if (!ok) ++g_fail;
}

/// 逐字节比较（整数与"要求逐位一致"的浮点都走这里）。
template <typename T>
bool same_bytes(const std::vector<T>& a, const std::vector<T>& b, size_t* first_bad,
                size_t* ndiff) {
    *first_bad = 0;
    *ndiff = 0;
    if (a.size() != b.size()) {
        *ndiff = a.size();
        return false;
    }
    if (std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0) return true;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::memcmp(&a[i], &b[i], sizeof(T)) != 0) {
            if (*ndiff == 0) *first_bad = i;
            ++*ndiff;
        }
    }
    return false;
}

template <typename T>
std::vector<T> as_vec(const npy::Array& a) {
    const T* p = a.data<T>();
    return std::vector<T>(p, p + a.count());
}

/// 把 .npy 里的数据按目标类型取出来（dtype 必须匹配，否则抛）。
template <typename T>
std::vector<T> load_vec(const npy::Array& a, const char* expect) {
    if (a.dtype != expect) {
        throw std::runtime_error(std::string("dtype is '") + a.dtype + "', expected '" + expect + "'");
    }
    return as_vec<T>(a);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "tests/golden";
    auto load = [&](const char* name) { return npy::load(dir + "/" + name + ".npy"); };

    try {
        const auto a_left = load("left_u8");
        const auto a_right = load("right_u8");
        const auto a_luma = load("luma_f32");
        const auto a_cl = load("censusL_u64");
        const auto a_cr = load("censusR_u64");
        const auto a_cost = load("cost_u8");
        const auto a_sum = load("sum_u16");
        const auto a_dispR = load("dispR_f32");
        const auto a_disp = load("disp_f32");
        const auto a_depth = load("depth_f32");

        if (a_cost.shape.size() != 3) throw std::runtime_error("cost_u8 must be (H,W,D)");
        const int H = static_cast<int>(a_cost.shape[0]);
        const int W = static_cast<int>(a_cost.shape[1]);
        const int D = static_cast<int>(a_cost.shape[2]);

        stereo::SgmParams p;
        p.W = W;
        p.H = H;
        p.D = D;
        p.dir = 1;
        p.P1 = 8;
        p.P2 = 96;
        p.uniqueness = 0.1f;
        p.lrCheck = 1;

        // fB / minZ / maxZ 与 sgm_ref.pipeline()、gen_golden.py 的 DEFAULTS 一致
        const float fB = 0.66f, minZ = 0.12f, maxZ = 3.0f;

        const size_t n_pix = static_cast<size_t>(W) * H;
        const size_t n_cost = n_pix * D;

        std::printf("golden: %s  (%dx%d, D=%d)\n", dir.c_str(), W, H, D);

        // ---------------- A) 逐 kernel（每个 kernel 吃金标准输入）----------------
        const auto luma = load_vec<float>(a_luma, "f4");
        const auto g_cl = load_vec<uint64_t>(a_cl, "u8");
        const auto g_cr = load_vec<uint64_t>(a_cr, "u8");
        const auto g_cost = load_vec<uint8_t>(a_cost, "u1");
        const auto g_sum = load_vec<uint16_t>(a_sum, "u2");
        const auto g_dispR = load_vec<float>(a_dispR, "f4");
        const auto g_disp = load_vec<float>(a_disp, "f4");
        const auto g_depth = load_vec<float>(a_depth, "f4");

        size_t bad = 0, nd = 0;
        std::vector<uint64_t> cl(n_pix);
        stereo::census(luma.data(), W, H, cl.data());
        report("census (L)", same_bytes(cl, g_cl, &bad, &nd),
               nd ? "diff=" + std::to_string(nd) + " first=" + std::to_string(bad) : "");

        std::vector<uint8_t> cost(n_cost);
        stereo::cost_volume(g_cl.data(), g_cr.data(), p, cost.data());
        report("cost_volume", same_bytes(cost, g_cost, &bad, &nd),
               nd ? "diff=" + std::to_string(nd) + " first=" + std::to_string(bad) : "");

        std::vector<uint16_t> sum(n_cost);
        stereo::sgm_all_paths(g_cost.data(), sum.data(), luma.data(), p);
        report("sgm_all_paths x4", same_bytes(sum, g_sum, &bad, &nd),
               nd ? "diff=" + std::to_string(nd) + " first=" + std::to_string(bad) : "");

        std::vector<float> dispR(n_pix);
        stereo::wta_right(g_sum.data(), p, dispR.data());
        report("wta_right", same_bytes(dispR, g_dispR, &bad, &nd),
               nd ? "diff=" + std::to_string(nd) + " first=" + std::to_string(bad) : "");

        std::vector<float> disp(n_pix);
        stereo::wta_left(g_sum.data(), g_dispR.data(), p, disp.data());
        report("wta_left", same_bytes(disp, g_disp, &bad, &nd),
               nd ? "diff=" + std::to_string(nd) + " first=" + std::to_string(bad) : "");

        std::vector<float> depth(n_pix);
        stereo::to_depth(g_disp.data(), W, H, fB, minZ, maxZ, depth.data());
        report("to_depth", same_bytes(depth, g_depth, &bad, &nd),
               nd ? "diff=" + std::to_string(nd) + " first=" + std::to_string(bad) : "");

        // ---------------- B) 整链（从 uint8 输入开始）----------------
        const auto left_u8 = load_vec<uint8_t>(a_left, "u1");
        const auto right_u8 = load_vec<uint8_t>(a_right, "u1");
        std::vector<float> luma_c(n_pix), luma_rc(n_pix);
        stereo::rgb_to_luma(left_u8.data(), W, H, W * 3, luma_c.data());
        stereo::rgb_to_luma(right_u8.data(), W, H, W * 3, luma_rc.data());

        // rgb_to_luma 与 numpy 的 (img @ LUMA) 只要求数值等价，不要求逐位
        float max_dl = 0.0f;
        size_t nbit = 0;
        for (size_t i = 0; i < n_pix; ++i) {
            max_dl = std::max(max_dl, std::fabs(luma_c[i] - luma[i]));
            if (std::memcmp(&luma_c[i], &luma[i], sizeof(float)) != 0) ++nbit;
        }
        char buf[128];
        std::snprintf(buf, sizeof(buf), "max|dl|=%.3g  bitwise-diff=%zu/%zu", max_dl, nbit, n_pix);
        report("rgb_to_luma", max_dl <= 1e-7f, buf);

        std::vector<uint64_t> cl2(n_pix), cr2(n_pix);
        stereo::census(luma_c.data(), W, H, cl2.data());
        stereo::census(luma_rc.data(), W, H, cr2.data());
        report("census R (computed)", same_bytes(cr2, g_cr, &bad, &nd),
               nd ? "diff=" + std::to_string(nd) + " first=" + std::to_string(bad) : "");

        std::vector<uint8_t> cost2(n_cost);
        stereo::cost_volume(cl2.data(), cr2.data(), p, cost2.data());
        report("cost (computed)", same_bytes(cost2, g_cost, &bad, &nd),
               nd ? "diff=" + std::to_string(nd) + " first=" + std::to_string(bad) : "");

        std::vector<uint16_t> sum2(n_cost);
        stereo::sgm_all_paths(cost2.data(), sum2.data(), luma_c.data(), p);
        report("sgm (computed)", same_bytes(sum2, g_sum, &bad, &nd),
               nd ? "diff=" + std::to_string(nd) + " first=" + std::to_string(bad) : "");

        std::vector<float> dispR2(n_pix), disp2(n_pix), depth2(n_pix);
        stereo::wta_right(sum2.data(), p, dispR2.data());
        stereo::wta_left(sum2.data(), dispR2.data(), p, disp2.data());
        stereo::to_depth(disp2.data(), W, H, fB, minZ, maxZ, depth2.data());
        report("disp (computed)", same_bytes(disp2, g_disp, &bad, &nd),
               nd ? "diff=" + std::to_string(nd) + " first=" + std::to_string(bad) : "");
        report("depth (computed)", same_bytes(depth2, g_depth, &bad, &nd),
               nd ? "diff=" + std::to_string(nd) + " first=" + std::to_string(bad) : "");

        size_t nvalid = 0;
        for (float z : disp) nvalid += (z > 0.0f) ? 1 : 0;
        std::printf("\n有效视差 %.1f%%  (%zu/%zu)\n", 100.0 * double(nvalid) / double(n_pix), nvalid, n_pix);

        // ---------------- C) 纯 C 接口（三层架构的边界）----------------
        {
            stereo_config cfg;
            stereo_config_default(&cfg);
            cfg.max_disp = D;
            cfg.p1 = static_cast<int>(p.P1);
            cfg.p2 = static_cast<int>(p.P2);
            cfg.uniqueness = p.uniqueness;
            cfg.lr_check = p.lrCheck;

            stereo_context *ctx = stereo_create();
            std::vector<float> disp_c(n_pix, -1.0f), depth_c(n_pix, -1.0f);
            const stereo_image li{left_u8.data(), W, H, W * 3, STEREO_FORMAT_RGB8};
            const stereo_image ri{right_u8.data(), W, H, W * 3, STEREO_FORMAT_RGB8};

            int rc = -1;
            if (ctx != nullptr) {
                rc = stereo_configure(ctx, &cfg);
                if (rc == 0) rc = stereo_match(ctx, &li, &ri, 1, disp_c.data(), 0);
                if (rc == 0) {
                    rc = stereo_disparity_to_depth(ctx, disp_c.data(), 0, W, H, fB, minZ, maxZ,
                                                   depth_c.data(), 0);
                }
            }
            report("c_api stereo_match", rc == 0 && same_bytes(disp_c, g_disp, &bad, &nd),
                   rc != 0 ? std::string("rc=") + std::to_string(rc) + " " + stereo_last_error(ctx)
                           : "");
            report("c_api to_depth", rc == 0 && same_bytes(depth_c, g_depth, &bad, &nd), "");
            stereo_destroy(ctx);
        }

        // ---------------- D) 标量基准（不含 IO）----------------
        using clock = std::chrono::steady_clock;
        const int iters = 5;
        auto bench = [&](const char* name, auto&& fn) {
            // 预热一次，避免第一轮的缺页影响
            fn();
            const auto t0 = clock::now();
            for (int i = 0; i < iters; ++i) fn();
            const auto t1 = clock::now();
            const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / iters;
            std::printf("  %-14s %8.2f ms\n", name, ms);
            return ms;
        };

        std::printf("\n标量基准（%dx%d, D=%d, 单线程, 不含 IO）:\n", W, H, D);
        const double t_census = bench("census x2", [&] {
            stereo::census(luma_c.data(), W, H, cl2.data());
            stereo::census(luma_rc.data(), W, H, cr2.data());
        });
        const double t_cost = bench("costVolume", [&] {
            stereo::cost_volume(cl2.data(), cr2.data(), p, cost2.data());
        });
        const double t_sgm = bench("sgmPath x4", [&] {
            stereo::sgm_all_paths(cost2.data(), sum2.data(), luma_c.data(), p);
        });
        const double t_wta = bench("wta R+L", [&] {
            stereo::wta_right(sum2.data(), p, dispR2.data());
            stereo::wta_left(sum2.data(), dispR2.data(), p, disp2.data());
        });
        const double t_depth = bench("toDepth", [&] {
            stereo::to_depth(disp2.data(), W, H, fB, minZ, maxZ, depth2.data());
        });
        const double total = t_census + t_cost + t_sgm + t_wta + t_depth;
        std::printf("  %-14s %8.2f ms   → %.1f fps（单帧上限）\n", "合计", total, 1000.0 / total);
    } catch (const std::exception& e) {
        std::printf("ERROR: %s\n", e.what());
        return 2;
    }

    std::printf(g_fail ? "\n有 %d 项不一致\n" : "\n全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
