// geometry_test.cpp — 第 2 步（极线矫正）与第 4 步（深度回转）的自检
//
// 这里**不需要**外部金标准：矫正的正确性可以用"模型往返"来验，
// 也就是把「矫正后像素 → 3D → 原始相机像素」算两遍，一遍用 MW/MU 单应，
// 一遍用显式的针孔投影，两边必须逐点相等。这样在 Windows/Linux 上就能跑，
// 不需要 Mac、不需要 Metal。
//
// 用法：geometry_test
//
// 检查项：
//   1) 基线沿 x 的退化情形：MW/MU/MUp 都该是单位阵
//   2) 对角双摄（iPhone 13-15）：Rr*t 精确落在 (baseline,0,0)，基线角 ≈ 45°
//   3) MW / MU 与显式针孔投影逐点相等（含 dy 行偏移）
//   4) MUp 是"矫正后 ↔ 宽相机"的往返
//   5) Q 把 (x, y, 非负视差) 还原成矫正后 3D（米）
//   6) rectify_image 的方向约定（+2 像素平移必须真的往 +2 走）
//   7) unrectify_depth 在单位阵下逐位不变
//   8) C 接口与 C++ 接口结果一致；to_depth 的紧凑/跨版本一致

#include "stereo/geometry.hpp"
#include "stereo/rectify.hpp"
#include "stereo/sgm.hpp"
#include "stereo/stereo_c_api.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int g_fail = 0;

void report(const char* name, bool ok, const char* detail = "") {
    std::printf("  %-34s %s%s%s\n", name, ok ? "OK  " : "FAIL", detail[0] ? "  " : "", detail);
    if (!ok) ++g_fail;
}

bool near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

/// 相对容差比较（像素坐标量级 1e2~1e3）。
bool near_rel(float a, float b, float rel = 2e-3f) {
    const float s = std::fmax(1.0f, std::fmax(std::fabs(a), std::fabs(b)));
    return std::fabs(a - b) <= rel * s;
}

stereo::RectifyInput make_input(float deg, float baseline_mm, int work, int src) {
    stereo::RectifyInput in;
    in.wide = stereo::Intrinsics{1400.0f, 1400.0f, 640.0f, 360.0f};
    in.ultra = stereo::Intrinsics{900.0f, 900.0f, 640.0f, 360.0f};
    in.extrinsic.R = stereo::mat3_identity();
    const float r = deg * 3.14159265358979f / 180.0f;
    in.extrinsic.t = stereo::Vec3{baseline_mm * std::cos(r), baseline_mm * std::sin(r), 0.0f};
    in.src_width = src;
    in.work_width = work;
    in.work_height = (work * 9) / 16;
    in.flip = 0;
    in.dy = 0.0f;
    return in;
}

/// 齐次像素：返回 (x, y) 与第三分量。
struct Pt {
    float x, y, w;
};

Pt apply(const stereo::Mat3& m, float u, float v) {
    const stereo::Vec3 q = stereo::mat3_mul_vec(m, stereo::Vec3{u, v, 1.0f});
    return Pt{q.x, q.y, q.z};
}

/// 显式针孔投影：X（相机坐标，米/毫米同单位）→ 像素。
Pt project(const stereo::Intrinsics& k, const stereo::Vec3& X) {
    return Pt{k.fx * X.x + k.cx * X.z, k.fy * X.y + k.cy * X.z, X.z};
}

/// MW / MU / MUp 的完整校验：只看数学，不依赖金标准。
///
/// 对一组 3D 点（宽相机坐标系，单位不限）算两遍：
///   一遍用针孔投影：宽相机 → 左图原图；超广角（含外参）→ 右图原图
///   一遍用矫正映射：Rr*X 落到“矫正后”坐标系，再用 MW / MU 回到原图
/// 同时检查极线性质：同一个 3D 点在左右两幅矫正图里的 **y 必须相等**
/// （差一个已知的 dy 行偏移）。这是极线矫正的定义本身。
int check_model(const stereo::RectifyOutput& g, const stereo::RectifyInput& in, int count) {
    int bad = 0;
    const stereo::Mat3 RrT = stereo::mat3_transpose(g.Rr);
    const float b = g.baseline_mm / 1000.0f;
    for (int i = 0; i < count; ++i) {
        // 宽相机坐标系里的 3D 点（毫米）
        const stereo::Vec3 X{-80.0f + static_cast<float>(i % 17) * 10.0f,
                             -60.0f + static_cast<float>(i % 13) * 10.0f,
                             300.0f + static_cast<float>(i % 7) * 250.0f};

        const stereo::Vec3 Xu = stereo::mat3_mul_vec(
            stereo::mat3_transpose(in.extrinsic.R),
            stereo::Vec3{X.x - in.extrinsic.t.x, X.y - in.extrinsic.t.y, X.z - in.extrinsic.t.z});
        if (!(Xu.z > 50.0f)) continue;

        const Pt act_l = project(in.wide, X);
        const Pt act_u = project(in.ultra, Xu);
        // 宽相机像素在**工作分辨率**下的位置（MUp 用这个坐标系，见 geometry.hpp）
        const Pt wide_work = project(g.K, X);
        // 矫正后坐标：X_rect = Rr * X
        const Pt rec_l = project(g.K, stereo::mat3_mul_vec(g.Rr, X));
        const Pt rec_r = project(g.K, stereo::mat3_mul_vec(g.Rr, Xu));
        const float lx = rec_l.x / rec_l.w, ly = rec_l.y / rec_l.w;
        const float rx = rec_r.x / rec_r.w, ry = rec_r.y / rec_r.w;

        // (a) 极线：同一个 3D 点在左右两幅矫正图里必须同一行。
        if (!near_rel(ly, ry, 5e-3f)) ++bad;
        // (b) 左右视差 = fx * b / Z（乘 dir 是因为基线方向可取反）
        if (!near_rel(lx - rx, g.K.fx * b / (X.z / 1000.0f) * static_cast<float>(g.dir), 5e-3f)) {
            ++bad;
        }
        // (c) MW / MU / MUp
        const Pt w = apply(g.to_wide_image, lx, ly);
        if (w.w <= 0.0f || !near_rel(w.x / w.w, act_l.x / act_l.w, 5e-3f) ||
            !near_rel(w.y / w.w, act_l.y / act_l.w, 5e-3f)) {
            ++bad;
        }
        // MU 内部有 +dy 的行平移（calibrate() 找到的系统误差补偿量）；
        // 要把 dy 减掉才能对应“没有人为偏移”的理想模型。
        const Pt u = apply(g.to_ultra_image, rx, ry - in.dy);
        if (u.w <= 0.0f || !near_rel(u.x / u.w, act_u.x / act_u.w, 5e-3f) ||
            !near_rel(u.y / u.w, act_u.y / act_u.w, 5e-3f)) {
            ++bad;
        }
        const Pt m = apply(g.to_rect_image, wide_work.x / wide_work.w, wide_work.y / wide_work.w);
        if (m.w <= 0.0f || !near_rel(m.x / m.w, lx, 5e-3f) ||
            !near_rel(m.y / m.w, ly, 5e-3f)) {
            ++bad;
        }
    }
    return bad;
}

}  // namespace

int main() {
    using namespace stereo;

    // ---------------- 1) 基线沿 x 的退化情形 ----------------
    {
        const int W = 640;
        RectifyInput in = make_input(0.0f, 65.0f, W, W);  // 缩放比 1
        in.ultra = in.wide;                              // 两路同内参：三个矩阵都该是 I
        RectifyOutput g;
        const bool ok = rectify_geometry(in, &g);
        report("rectify_geometry 成功", ok);
        if (ok) {
            float max_dev = 0.0f;
            for (int i = 0; i < 9; ++i) {
                const float ident = (i % 4 == 0) ? 1.0f : 0.0f;
                max_dev = std::fmax(max_dev, std::fabs(g.to_wide_image.m[i] - ident));
                max_dev = std::fmax(max_dev, std::fabs(g.to_ultra_image.m[i] - ident));
                max_dev = std::fmax(max_dev, std::fabs(g.to_rect_image.m[i] - ident));
            }
            char buf[96];
            std::snprintf(buf, sizeof buf, "max|M-I| = %.2e", static_cast<double>(max_dev));
            report("基线沿 x → MW/MU/MUp = I", max_dev < 1e-4f, buf);
            report("dir = +1", g.dir == 1);
            report("baseline_mm ≈ 65", near(g.baseline_mm, 65.0f, 1e-3f));
            std::snprintf(buf, sizeof buf, "%.4f", static_cast<double>(g.focal_baseline));
            report("fB = K.fx * b(米)", near(g.focal_baseline, g.K.fx * 0.065f, 1e-3f), buf);
        }

        // 同一台相机、缩放比 0.5：K 减半，MW 仍是"校正到 K"的映射
        RectifyInput in2 = make_input(0.0f, 65.0f, 640, 1280);
        RectifyOutput g2;
        report("缩放比 0.5 时 K.fx = 700", rectify_geometry(in2, &g2) && near(g2.K.fx, 700.0f, 1e-3f));
    }

    // ---------------- 2) 对角双摄（iPhone 13-15） ----------------
    {
        RectifyInput in = make_input(45.0f, 65.0f, 640, 1280);
        in.dy = 0.25f;   // 顺便把行偏移带进来，测 MU 的 T
        RectifyOutput g;
        report("对角基线 rectify 成功", rectify_geometry(in, &g));
        report("baseline_deg ≈ 45", near(g.baseline_deg, 45.0f, 1e-2f));

        // Rr * t 必须精确落在 (baseline, 0, 0)：e2、e3 与 t 正交是构造保证。
        const Vec3 rt = mat3_mul_vec(g.Rr, in.extrinsic.t);
        char buf[128];
        std::snprintf(buf, sizeof buf, "(%.3f, %.2e, %.2e)", static_cast<double>(rt.x),
                      static_cast<double>(rt.y), static_cast<double>(rt.z));
        report("Rr*t = (b, 0, 0)", near(rt.x, g.baseline_mm, 1e-3f) && near(rt.y, 0.0f, 1e-3f) &&
                                       near(rt.z, 0.0f, 1e-3f),
               buf);

        // 主点：矫正后的主点必须映到原始图的主点
        const Pt w0 = apply(g.to_wide_image, g.K.cx, g.K.cy);
        report("主点 → 宽相机主点", near_rel(w0.x / w0.w, 640.0f) && near_rel(w0.y / w0.w, 360.0f));
        const Pt u0 = apply(g.to_ultra_image, g.K.cx, g.K.cy - in.dy);
        report("主点 → 超广角主点", near_rel(u0.x / u0.w, 640.0f) && near_rel(u0.y / u0.w, 360.0f));

        // ---------------- 3) 与全链模型逐点对拍（含极线性质）----------------
        const int bad_model = check_model(g, in, 120);
        std::snprintf(buf, sizeof buf, "%d 处不一致（含极线）", bad_model);
        report("极线 + MW/MU/MUp 与模型一致", bad_model == 0, buf);

        // ---------------- 4) MUp 往返 ----------------
        int bad_m = 0;
        for (int i = 0; i < 60; ++i) {
            const float u = 5.0f + static_cast<float>(i) * 9.0f;
            const float v = 5.0f + static_cast<float>(i % 31) * 8.0f;
            const float Z = 0.5f + static_cast<float>(i % 5) * 0.5f;
            const Vec3 Xr{(u - g.K.cx) * Z / g.K.fx, (v - g.K.cy) * Z / g.K.fy, Z};
            const Vec3 Xw = mat3_mul_vec(mat3_transpose(g.Rr), Xr);
            const Pt pw = project(g.K, Xw);   // 宽相机（工作分辨率 K）
            const Pt back = apply(g.to_rect_image, pw.x / pw.w, pw.y / pw.w);
            if (back.w <= 0.0f || !near_rel(back.x / back.w, u) || !near_rel(back.y / back.w, v)) {
                ++bad_m;
            }
        }
        std::snprintf(buf, sizeof buf, "%d/60 不一致", bad_m);
        report("MUp 把宽相机像素转回矫正像素", bad_m == 0, buf);

        // ---------------- 5) Q 还原 3D ----------------
        int bad_q = 0;
        const float b = g.baseline_mm / 1000.0f;
        for (int i = 0; i < 60; ++i) {
            const float X = -0.4f + static_cast<float>(i % 13) * 0.08f;
            const float Y = -0.2f + static_cast<float>(i % 7) * 0.06f;
            const float Z = 0.3f + static_cast<float>(i % 11) * 0.25f;
            const float xl = g.K.fx * X / Z + g.K.cx;
            const float y = g.K.fy * Y / Z + g.K.cy;
            const float d = g.K.fx * b / Z;
            // Q * (xl, y, d, 1)
            const float q[4] = {g.Q[0] * xl + g.Q[3], g.Q[5] * y + g.Q[7],
                                g.Q[11], g.Q[14] * d};
            if (!near_rel(q[0] / q[3], X, 5e-3f) || !near_rel(q[1] / q[3], Y, 5e-3f) ||
                !near_rel(q[2] / q[3], Z, 5e-3f)) {
                ++bad_q;
            }
        }
        std::snprintf(buf, sizeof buf, "%d/60 不一致", bad_q);
        report("Q 还原矫正后 3D（米）", bad_q == 0, buf);

        // Pl/Pr：基线在 x 上，x 方向的平移量必须是 dir*b（b 以米为单位）
        report("P_left 右列全 0", near(g.P_left[3], 0.0f, 0.0f) && near(g.P_left[7], 0.0f, 0.0f) &&
                                       near(g.P_left[11], 0.0f, 0.0f));
        report("P_right 右列 = (-dir*b, 0, 0)",
               near(g.P_right[3], -static_cast<float>(g.dir) * b, 1e-6f) &&
                   near(g.P_right[7], 0.0f, 0.0f) && near(g.P_right[11], 0.0f, 0.0f));
    }

    // ---------------- flip：外参取逆 + flip 必须回到原点 ----------------
    {
        RectifyInput a = make_input(45.0f, 65.0f, 640, 1280);
        RectifyOutput ga;
        const bool ok_a = rectify_geometry(a, &ga);

        // 把外参换成反方向（X_ultra = R^T*X_wide - R^T*t），再 flip：几何必须与原来一致
        RectifyInput b = a;
        b.extrinsic.R = mat3_transpose(a.extrinsic.R);
        const Vec3 rt = mat3_mul_vec(b.extrinsic.R, a.extrinsic.t);
        b.extrinsic.t = Vec3{-rt.x, -rt.y, -rt.z};
        b.flip = 1;
        RectifyOutput gb;
        const bool ok_b = rectify_geometry(b, &gb);
        report("反向外参 + flip 成功", ok_a && ok_b);
        float max_dev = 0.0f;
        for (int i = 0; i < 9; ++i) {
            max_dev = std::fmax(max_dev, std::fabs(ga.to_wide_image.m[i] - gb.to_wide_image.m[i]));
            max_dev = std::fmax(max_dev, std::fabs(ga.to_ultra_image.m[i] - gb.to_ultra_image.m[i]));
        }
        char buf[96];
        std::snprintf(buf, sizeof buf, "max|ΔM| = %.2e", static_cast<double>(max_dev));
        report("反向外参 + flip 复原几何", max_dev < 1e-5f, buf);
        report("反向外参 + flip 基线与角度不变",
               near(gb.baseline_mm, ga.baseline_mm, 1e-3f) &&
                   near(gb.baseline_deg, ga.baseline_deg, 1e-2f) && gb.dir == ga.dir);
    }

    // ---------------- 6) rectify_image 的方向约定 ----------------
    {
        const int W = 24, H = 8;
        std::vector<uint8_t> src(static_cast<size_t>(W) * H * 4);
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                uint8_t* p = &src[(static_cast<size_t>(y) * W + x) * 4];
                p[0] = static_cast<uint8_t>(x * 8);   // B
                p[1] = static_cast<uint8_t>(y * 20);  // G
                p[2] = static_cast<uint8_t>(200 - x * 4);  // R
                p[3] = 255;
            }
        }
        ImageView s{src.data(), W, H, W * 4, kFormatBGRA8};

        std::vector<uint8_t> a(static_cast<size_t>(W) * H * 4, 0);
        std::vector<uint8_t> b(static_cast<size_t>(W) * H * 4, 0);
        MutableImageView da{a.data(), W, H, W * 4, kFormatBGRA8};
        MutableImageView db{b.data(), W, H, W * 4, kFormatBGRA8};

        Mat3 ident = mat3_identity();
        Mat3 shift = mat3_identity();
        shift.m[2] = 2.0f;   // x 方向 +2 像素
        const bool ok = rectify_image(s, ident, da) && rectify_image(s, shift, db);

        // 平移 2 像素：dst(x) = src(x+2)，所以 b(x, y) 必须逐位等于 a(x+2, y)
        int diff = 0;
        for (int y = 1; y < H - 1; ++y) {
            for (int x = 1; x < W - 3; ++x) {
                const uint8_t* pa = &a[(static_cast<size_t>(y) * W + x + 2) * 4];
                const uint8_t* pb = &b[(static_cast<size_t>(y) * W + x) * 4];
                if (std::memcmp(pa, pb, 3) != 0) ++diff;
            }
        }
        char buf[96];
        std::snprintf(buf, sizeof buf, "%d 个像素不同", diff);
        report("+2px 平移方向正确", ok && diff == 0, buf);

        // 通道不串：B 随 x 增大而增大，R 随 x 增大而减小
        const uint8_t* left = &a[(static_cast<size_t>(4) * W + 3) * 4];
        const uint8_t* right = &a[(static_cast<size_t>(4) * W + 20) * 4];
        report("通道顺序不变（B/R 不串）", left[0] < right[0] && left[2] > right[2],
               "BGRA 顺序保持");

        // 源全 0：输出必须全 0（越界按 clamp_to_zero）
        std::vector<uint8_t> zero(static_cast<size_t>(W) * H * 4, 0);
        ImageView zs{zero.data(), W, H, W * 4, kFormatBGRA8};
        std::vector<uint8_t> out(static_cast<size_t>(W) * H * 4, 7);
        MutableImageView do_{out.data(), W, H, W * 4, kFormatBGRA8};
        Mat3 far = mat3_identity();
        far.m[2] = 1e5f;   // 完全跑出源图
        const bool ok2 = rectify_image(zs, far, do_);
        int nonzero = 0;
        for (uint8_t v : out) nonzero += (v != 0);
        report("越界补 0", ok2 && nonzero == 0);

        // 灰度图（1 通道）也能走；边缘会因为 clamp_to_zero 变暗，只看内部
        std::vector<uint8_t> g1(static_cast<size_t>(W) * H, 128), g2(static_cast<size_t>(W) * H, 0);
        ImageView gs{g1.data(), W, H, W, kFormatGray8};
        MutableImageView gd{g2.data(), W, H, W, kFormatGray8};
        const bool ok_gray = rectify_image(gs, ident, gd);
        int badc = 0;
        for (int y = 1; y < H - 1; ++y) {
            for (int x = 1; x < W - 1; ++x) badc += (g2[static_cast<size_t>(y) * W + x] != 128);
        }
        report("灰度 1 通道可用", ok_gray && badc == 0);

        // 格式不同必须拒绝（否则会静默把 B 与 R 写反）
        MutableImageView wrong{g2.data(), W, H, W, kFormatRGB8};
        report("源/目标格式不同被拒", !rectify_image(gs, ident, wrong));
        report("边缘按 clamp_to_zero 变暗", g2[0] < 128 && g2[W - 1] < 128);
    }

    // ---------------- 7) unrectify_depth 单位阵 ----------------
    {
        const int W = 16, H = 6;
        std::vector<float> src(static_cast<size_t>(W) * H);
        for (int i = 0; i < W * H; ++i) src[i] = static_cast<float>(i) * 0.25f;
        std::vector<float> dst(static_cast<size_t>(W) * H, -1.0f);
        const Mat3 ident = mat3_identity();
        const bool ok = unrectify_depth(src.data(), W, H, W, ident, dst.data(), W, H, W);
        report("单位阵下逐位不变", ok && std::memcmp(src.data(), dst.data(), src.size() * 4) == 0);
    }

    // ---------------- 8) C 接口与 C++ 接口一致 ----------------
    {
        RectifyInput in = make_input(45.0f, 65.0f, 640, 1280);
        in.dy = 0.25f;

        stereo_rectify_input ci;
        std::memset(&ci, 0, sizeof ci);
        ci.wide.fx = in.wide.fx;
        ci.wide.fy = in.wide.fy;
        ci.wide.cx = in.wide.cx;
        ci.wide.cy = in.wide.cy;
        ci.ultra.fx = in.ultra.fx;
        ci.ultra.fy = in.ultra.fy;
        ci.ultra.cx = in.ultra.cx;
        ci.ultra.cy = in.ultra.cy;
        for (int i = 0; i < 9; ++i) ci.R[i] = in.extrinsic.R.m[i];
        ci.t[0] = in.extrinsic.t.x;
        ci.t[1] = in.extrinsic.t.y;
        ci.t[2] = in.extrinsic.t.z;
        ci.src_width = in.src_width;
        ci.work_width = in.work_width;
        ci.work_height = in.work_height;
        ci.flip = in.flip;
        ci.dy = in.dy;

        stereo_rectify_output co;
        std::memset(&co, 0, sizeof co);
        RectifyOutput g;
        const bool ok = stereo_rectify_geometry(&ci, &co) == 0 && rectify_geometry(in, &g);
        int diff = 0;
        for (int i = 0; i < 9 && ok; ++i) {
            diff += std::memcmp(&co.to_wide_image[i], &g.to_wide_image.m[i], 4) != 0;
            diff += std::memcmp(&co.to_ultra_image[i], &g.to_ultra_image.m[i], 4) != 0;
            diff += std::memcmp(&co.to_rect_image[i], &g.to_rect_image.m[i], 4) != 0;
        }
        diff += std::memcmp(&co.focal_baseline, &g.focal_baseline, 4) != 0;
        diff += std::memcmp(&co.baseline_deg, &g.baseline_deg, 4) != 0;
        diff += (co.dir != g.dir);
        report("C 接口与 C++ 逐字节一致", ok && diff == 0);

        // 参数非法要拒绝，不能算出垃圾
        stereo_rectify_input bad = ci;
        bad.src_width = 0;
        report("src_width = 0 被拒", stereo_rectify_geometry(&bad, &co) != 0);
        bad = ci;
        bad.t[0] = bad.t[1] = bad.t[2] = 0.0f;
        report("外参为 0 被拒", stereo_rectify_geometry(&bad, &co) != 0);

        // stereo_rectify_image / stereo_unrectify_depth 的 C 封装必须与 C++ 一致
        const int W = 32, H = 18;
        std::vector<uint8_t> src(static_cast<size_t>(W) * H * 4);
        for (size_t i = 0; i < src.size(); ++i) src[i] = static_cast<uint8_t>((i * 37) & 0xFF);
        std::vector<uint8_t> viaCpp(static_cast<size_t>(W) * H * 4, 0);
        std::vector<uint8_t> viaC(static_cast<size_t>(W) * H * 4, 0);

        const Mat3 rect_to_wide = mat3_from_intrinsics(700.0f, 700.0f, 320.0f, 180.0f);
        const Mat3 mw = mat3_mul(rect_to_wide, mat3_transpose(g.Rr));
        ImageView is{src.data(), W, H, W * 4, kFormatBGRA8};
        MutableImageView o1{viaCpp.data(), W, H, W * 4, kFormatBGRA8};
        const bool ok1 = rectify_image(is, mw, o1);

        stereo_image ci_src{};
        ci_src.data = src.data();
        ci_src.width = W;
        ci_src.height = H;
        ci_src.stride_bytes = W * 4;
        ci_src.format = STEREO_FORMAT_BGRA8;
        stereo_image ci_dst = ci_src;
        ci_dst.data = viaC.data();
        float mw9[9];
        for (int i = 0; i < 9; ++i) mw9[i] = mw.m[i];
        const bool ok2 = stereo_rectify_image(&ci_src, mw9, &ci_dst) == 0;
        report("stereo_rectify_image 与 C++ 逐字节一致",
               ok1 && ok2 && std::memcmp(viaCpp.data(), viaC.data(), viaCpp.size()) == 0);

        std::vector<float> d1(static_cast<size_t>(W) * H, 0.0f);
        std::vector<float> d2(static_cast<size_t>(W) * H, 0.0f);
        for (int i = 0; i < W * H; ++i) d1[i] = static_cast<float>(i % 7) * 0.5f;
        float mup9[9];
        for (int i = 0; i < 9; ++i) mup9[i] = g.to_rect_image.m[i];
        const bool ok3 = unrectify_depth(d1.data(), W, H, W, g.to_rect_image, d2.data(), W, H, W);
        std::vector<float> d3(static_cast<size_t>(W) * H, 0.0f);
        const bool ok4 = stereo_unrectify_depth(d1.data(), W, W, H, mup9, d3.data(), W, W, H) == 0;
        report("stereo_unrectify_depth 与 C++ 逐字节一致",
               ok3 && ok4 && std::memcmp(d2.data(), d3.data(), d2.size() * 4) == 0);
    }

    // ---------------- 9) to_depth 紧凑 / 跨版本一致 ----------------
    {
        const int W = 40, H = 12, pad = 5;
        std::vector<float> disp(static_cast<size_t>(W) * H);
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                disp[static_cast<size_t>(y) * W + x] =
                    (x % 7 == 0 || y % 5 == 0) ? 0.0f : 8.0f + static_cast<float>(x % 13);
            }
        }
        std::vector<float> a(static_cast<size_t>(W) * H, 0);
        std::vector<float> b(static_cast<size_t>((W + pad) * H), -9.0f);
        to_depth(disp.data(), W, H, 65.0f, 0.12f, 3.0f, a.data());
        to_depth(disp.data(), W, W, H, 65.0f, 0.12f, 3.0f, b.data(), W + pad);

        int diff = 0;
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                if (std::memcmp(&a[static_cast<size_t>(y) * W + x],
                                &b[static_cast<size_t>(y) * (W + pad) + x], 4) != 0) {
                    ++diff;
                }
            }
            for (int x = W; x < W + pad; ++x) {
                diff += (b[static_cast<size_t>(y) * (W + pad) + x] != -9.0f);
            }
        }
        report("to_depth 紧凑/跨版本一致", diff == 0);

        // C 接口的 depth 也要和 C++ 一致
        stereo_context* ctx = stereo_create();
        std::vector<float> c(static_cast<size_t>(W) * H, 0);
        const bool ok = stereo_disparity_to_depth(disp.data(), W, W, H, 65.0f, 0.12f, 3.0f,
                                                  c.data(), W) == 0;
        report("stereo_disparity_to_depth 一致",
               ok && std::memcmp(a.data(), c.data(), a.size() * 4) == 0);
        stereo_destroy(ctx);
    }

    std::printf(g_fail ? "\n有 %d 项不一致\n" : "\n全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
