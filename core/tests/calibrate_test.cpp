// calibrate_test.cpp — 自动标定的自检
//
// 这类"搜索"代码最难验的是"它到底有没有在对的维度上找"。所以这里不查内部实现，
// 而是**注入已知答案再让它找回来**：
//
//   1) 造一张假的"矫正后"图 A（块状噪声，便于 Census 分辨）
//   2) 右图 = A 平移 (d0, dy_true) 后得到的原始帧 → 也就是"两路之间有 dy_true 的行错位"
//   3) 单应取单位阵（两路同内参、无旋转、基线 65 mm 沿 x），于是"矫正"就是恒等
//   4) 于是正确的答案就是 dy = dy_true（且不翻转），搜不到就是 bug
//
// 还能顺便验：score 在正确答案附近确实是最小、evaluations 是 55、非法参数会拒绝。

#include "stereo/calibrate.hpp"
#include "stereo/stereo_c_api.h"

#include <cmath>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <vector>

namespace {

int g_fail = 0;

void report(const char* name, bool ok, const char* detail = "") {
    std::printf("  %-44s %s%s%s\n", name, ok ? "OK  " : "FAIL", detail[0] ? "  " : "", detail);
    if (!ok) ++g_fail;
}

constexpr int W = 160;   // 工作分辨率（也等于原始帧尺寸，方便让单应退化成恒等）
constexpr int H = 120;

/// 逐像素伪随机纹理（2×2 一块）。
///
/// 用块状常量纹理会骗过判据：那种图上"换一个视差"几乎总能找到同样的 Census，
/// 于是 per-pixel 最小值对**行**偏移不敏感。要验 dy，纹理必须在横向上唯一。
uint8_t pattern(int x, int y) {
    unsigned h = static_cast<unsigned>(x / 2) * 73856093u ^ static_cast<unsigned>(y / 2) * 19349663u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return static_cast<uint8_t>(h & 0xFF);
}

/// 原始帧（BGRA8）：内容 = pattern(x - sx, y - sy)。
struct Frame {
    std::vector<uint8_t> px;
    int stride = W * 4;
    Frame(int sx, int sy) : px(static_cast<size_t>(W) * H * 4, 0) {
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                const int sxp = x - sx;
                const int syp = y - sy;
                const uint8_t v =
                    (sxp >= 0 && sxp < W && syp >= 0 && syp < H) ? pattern(sxp, syp) : 0;
                uint8_t* p = &px[(static_cast<size_t>(y) * W + x) * 4];
                p[0] = v;          // B
                p[1] = v;          // G
                p[2] = v;          // R
                p[3] = 255;
            }
        }
    }
};

stereo::ImageView view(const Frame& f) {
    return stereo::ImageView{f.px.data(), W, H, f.stride, stereo::kFormatBGRA8};
}

/// 两路同内参、无旋转、基线沿 x —— 于是矫正单应 = 恒等，便于注入已知答案。
stereo::RectifyInput base_input() {
    stereo::RectifyInput in;
    in.wide = stereo::Intrinsics{100.0f, 100.0f, 80.0f, 60.0f};
    in.ultra = in.wide;
    in.extrinsic.R = stereo::mat3_identity();
    in.extrinsic.t = stereo::Vec3{65.0f, 0.0f, 0.0f};
    in.src_width = W;
    in.work_width = W;
    in.work_height = H;
    in.flip = 0;
    in.dy = 0;
    return in;
}

}  // namespace

int main() {
    using namespace stereo;

    std::vector<uint8_t> rect_l(static_cast<size_t>(W) * H * 4);
    std::vector<uint8_t> rect_r(static_cast<size_t>(W) * H * 4);
    std::vector<float> luma_l(static_cast<size_t>(W) * H);
    std::vector<float> luma_r(static_cast<size_t>(W) * H);
    std::vector<uint64_t> census_l(static_cast<size_t>(W) * H);
    std::vector<uint64_t> census_r(static_cast<size_t>(W) * H);
    CalibrateScratch sc;
    sc.rect_l = rect_l.data();
    sc.rect_r = rect_r.data();
    sc.luma_l = luma_l.data();
    sc.luma_r = luma_r.data();
    sc.census_l = census_l.data();
    sc.census_r = census_r.data();

    CalibrateParams params;
    params.max_disp = 64;
    params.pixel_step = 1;   // 测试用全采样，结论更硬
    params.flip0 = 0;
    params.dy0 = 0;

    // 左图不动；右图内容下移 3 行，并**左**移 5 像素当视差。
    // 左移是因为 App 的约定是 dir = +1（同名点在左图右侧、右图左侧），
    // 反过来注入就会让搜索合理地选 flip = 1。
    const int dy_true = 3;
    const int disp = -5;
    Frame left(0, 0);
    Frame right(disp, dy_true);

    // ---------------- 1) 能找到注入的 dy ----------------
    {
        CalibrateResult res;
        const bool ok = calibrate_rectification(base_input(), view(left), view(right), params, &sc,
                                                &res);
        char buf[128];
        std::snprintf(buf, sizeof buf, "flip=%d dy=%.2f score=%.3f", res.flip,
                      static_cast<double>(res.dy), static_cast<double>(res.score));
        report("找到注入的 dy = +3", ok && res.flip == 0 && std::fabs(res.dy - 3.0f) < 0.01f, buf);
        report("打分次数是 54（2×25 + 4 细化）", ok && res.evaluations == 54);
    }

    // ---------------- 2) 另一个方向 ----------------
    {
        Frame right_up(disp, -2);
        CalibrateResult res;
        const bool ok = calibrate_rectification(base_input(), view(left), view(right_up), params,
                                               &sc, &res);
        char buf[96];
        std::snprintf(buf, sizeof buf, "dy=%.2f", static_cast<double>(res.dy));
        report("找到注入的 dy = -2", ok && res.flip == 0 && std::fabs(res.dy + 2.0f) < 0.01f, buf);
    }

    // ---------------- 3) 没有行错位时应该选 0 ----------------
    {
        Frame right_flat(disp, 0);
        CalibrateResult res;
        const bool ok = calibrate_rectification(base_input(), view(left), view(right_flat), params,
                                               &sc, &res);
        char buf[96];
        std::snprintf(buf, sizeof buf, "dy=%.2f", static_cast<double>(res.dy));
        report("无行错位时选 dy = 0", ok && res.flip == 0 && std::fabs(res.dy) < 0.01f, buf);
    }

    // ---------------- 4) 扫一遍注入的值，逐个都要能找回 ----------------
    // （不比较不同图之间的 score —— 那是不可比的；能对上注入值才是真正的性质）
    {
        const int injected[5] = {-7, -3, 0, 2, 5};
        int good = 0;
        for (int k = 0; k < 5; ++k) {
            Frame r(disp, injected[k]);
            CalibrateResult res;
            if (calibrate_rectification(base_input(), view(left), view(r), params, &sc, &res) &&
                res.flip == 0 && std::fabs(res.dy - static_cast<float>(injected[k])) < 0.01f) {
                ++good;
            }
        }
        char buf[64];
        std::snprintf(buf, sizeof buf, "%d/5 组", good);
        report("5 个不同的注入 dy 全部找回", good == 5, buf);
    }

    // ---------------- 5) 抽样步长不影响结论 ----------------
    {
        Frame right_true(disp, dy_true);
        CalibrateParams sampled = params;
        sampled.pixel_step = 4;
        CalibrateResult res;
        const bool ok = calibrate_rectification(base_input(), view(left), view(right_true), sampled,
                                               &sc, &res);
        char buf[96];
        std::snprintf(buf, sizeof buf, "dy=%.2f（step=4）", static_cast<double>(res.dy));
        report("抽样到 1/16 仍找到 dy = +3", ok && std::fabs(res.dy - 3.0f) < 0.01f, buf);
    }

    // ---------------- 6) 非法参数 ----------------
    {
        CalibrateResult res;
        CalibrateScratch empty;
        report("scratch 为空 -> 拒绝",
               !calibrate_rectification(base_input(), view(left), view(right), params, &empty, &res));
        CalibrateScratch broken = sc;
        broken.census_r = nullptr;
        report("census 缓冲缺失 -> 拒绝",
               !calibrate_rectification(base_input(), view(left), view(right), params, &broken, &res));
        CalibrateParams bad = params;
        bad.max_disp = 0;
        report("max_disp = 0 -> 拒绝",
               !calibrate_rectification(base_input(), view(left), view(right), bad, &sc, &res));
        RectifyInput no_base = base_input();
        no_base.extrinsic.t = Vec3{0, 0, 0};
        report("外参为 0（无法解矫正）-> 拒绝",
               !calibrate_rectification(no_base, view(left), view(right), params, &sc, &res));
    }

    // ---------------- 7) C 接口与 C++ 一致 ----------------
    {
        stereo_context* ctx = stereo_create();
        report("stereo_create", ctx != nullptr);

        stereo_rectify_input ci{};
        ci.wide.fx = 100;
        ci.wide.fy = 100;
        ci.wide.cx = 80;
        ci.wide.cy = 60;
        ci.ultra = ci.wide;
        ci.R[0] = ci.R[4] = ci.R[8] = 1.0f;
        ci.t[0] = 65.0f;
        ci.src_width = W;
        ci.work_width = W;
        ci.work_height = H;

        stereo_image lv{left.px.data(), W, H, left.stride, STEREO_FORMAT_BGRA8};
        stereo_image rv{right.px.data(), W, H, right.stride, STEREO_FORMAT_BGRA8};

        stereo_calibrate_params cp{};
        cp.max_disp = 64;
        cp.pixel_step = 1;
        stereo_calibrate_result cr{};
        const bool ok = stereo_calibrate(ctx, &ci, &lv, &rv, &cp, &cr) == 0;

        CalibrateResult cpp;
        const bool ok2 = calibrate_rectification(base_input(), view(left), view(right), params, &sc,
                                                &cpp);
        char buf[128];
        std::snprintf(buf, sizeof buf, "C: dy=%.2f  C++: dy=%.2f", static_cast<double>(cr.dy),
                      static_cast<double>(cpp.dy));
        report("C 接口与 C++ 结果一致",
               ok && ok2 && cr.flip == cpp.flip && std::memcmp(&cr.dy, &cpp.dy, 4) == 0, buf);

        report("C 接口拒绝 NULL",
               stereo_calibrate(ctx, nullptr, &lv, &rv, &cp, &cr) != 0);
        stereo_destroy(ctx);
    }

    // ---------------- 7b) 平台打分回调（iOS 的 GPU 路径就是这个形状）----------------
    {
        // 一个假的"GPU 打分"：只看 dy 离 2 有多远，验证搜索循环把 54 个候选都问了一遍、
        // 并且确实返回了最小者。
        struct Fake { int calls; } fake{0};
        auto score = [](void *user, int flip, float dy) -> float {
            Fake *f = static_cast<Fake *>(user);
            ++f->calls;
            return std::fabs(dy - 2.0f) + (flip ? 5.0f : 0.0f);
        };
        stereo_calibrate_params cp{};
        cp.max_disp = 64;
        cp.pixel_step = 4;
        stereo_calibrate_result cr{};
        const bool ok = stereo_calibrate_search(score, &fake, &cp, &cr) == 0;
        char buf[96];
        std::snprintf(buf, sizeof buf, "flip=%d dy=%.2f calls=%d", cr.flip,
                      static_cast<double>(cr.dy), fake.calls);
        report("平台打分回调：返回最小者且问了 54 次",
               ok && cr.flip == 0 && std::fabs(cr.dy - 2.0f) < 0.01f && fake.calls == 54, buf);
        report("平台打分回调拒绝空指针", stereo_calibrate_search(nullptr, &fake, &cp, &cr) != 0);
    }

    // ---------------- 8) 640×360 实测耗时（信息行 + 回归兜底）----------------
    {
        const int BW = 640, BH = 360;
        std::vector<uint8_t> bl(static_cast<size_t>(BW) * BH * 4), br(bl.size());
        for (int y = 0; y < BH; ++y) {
            for (int x = 0; x < BW; ++x) {
                const uint8_t v = pattern(x, y);
                uint8_t* a = &bl[(static_cast<size_t>(y) * BW + x) * 4];
                a[0] = a[1] = a[2] = v;
                a[3] = 255;
                uint8_t* b = &br[(static_cast<size_t>(y) * BW + x) * 4];
                const int sx = x + 5, sy = y - 2;
                const uint8_t w2 = (sx < BW && sy >= 0) ? pattern(sx, sy) : 0;
                b[0] = b[1] = b[2] = w2;
                b[3] = 255;
            }
        }
        std::vector<uint8_t> rl(static_cast<size_t>(BW) * BH * 4), rr(rl.size());
        std::vector<float> ll(static_cast<size_t>(BW) * BH), lr(ll.size());
        std::vector<uint64_t> cl(static_cast<size_t>(BW) * BH), cr(cl.size());
        CalibrateScratch big{rl.data(), rr.data(), ll.data(), lr.data(), cl.data(), cr.data()};

        RectifyInput bi = base_input();
        bi.wide = Intrinsics{400, 400, 320, 180};
        bi.ultra = bi.wide;
        bi.src_width = BW;
        bi.work_width = BW;
        bi.work_height = BH;

        ImageView lv{bl.data(), BW, BH, BW * 4, kFormatBGRA8};
        ImageView rv{br.data(), BW, BH, BW * 4, kFormatBGRA8};
        // CPU 全流水线版的成本是**设计上**的：64 万像素 × 54 次重采样，单核就是几秒。
        // 所以 iOS 走的是 stereo_calibrate_search + GPU 打分；这里只把数字打出来。
        CalibrateParams sp = params;
        sp.pixel_step = CalibrateParams{}.pixel_step;
        CalibrateResult res;
        const clock_t t0 = clock();
        const bool ok = calibrate_rectification(bi, lv, rv, sp, &big, &res);
        const double ms = 1000.0 * static_cast<double>(clock() - t0) / CLOCKS_PER_SEC;
        std::printf("  %-24s step=%-2d %6.0f ms  dy=%.1f  %s\n", "640×360 CPU 版（参考值）",
                    sp.pixel_step, ms, static_cast<double>(res.dy), ok ? "OK" : "FAIL");
        if (!ok) ++g_fail;
    }

    std::printf(g_fail ? "\n有 %d 项不一致\n" : "\n全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
