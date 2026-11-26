// stereo/geometry.cpp — 极线矫正的几何求解（第 2 步）
//
// 逐行对应改造前的 Swift 实现（StereoVision/StereoEngine.swift 的 geometry()），
// 语义、运算顺序、特殊标记都保持不变；只是把 simd 换成手写 3x3，把 Apple 依赖去掉。
// 差异只有一处：3x3 求逆用伴随矩阵（simd 走 Accelerate 的 LU），所以结果落在
// ~1e-7 相对误差里 —— 这是可以接受的，因为 MW/MU/MUp 只用来做像素重采样。

#include "stereo/geometry.hpp"

#include <cmath>

namespace stereo {
namespace {

/// 3x3 行列式。
inline float det3(const float* m) {
    return m[0] * (m[4] * m[8] - m[5] * m[7]) -
           m[1] * (m[3] * m[8] - m[5] * m[6]) +
           m[2] * (m[3] * m[7] - m[4] * m[6]);
}

inline Vec3 normalized(const Vec3& v) {
    const float n = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (!(n > 0.0f)) return Vec3{0, 0, 0};
    return Vec3{v.x / n, v.y / n, v.z / n};
}

inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

/// 行主序：[row0 | row1 | row2]。
inline Mat3 mat3_of_rows(const Vec3& r0, const Vec3& r1, const Vec3& r2) {
    Mat3 out{};
    out.m[0] = r0.x; out.m[1] = r0.y; out.m[2] = r0.z;
    out.m[3] = r1.x; out.m[4] = r1.y; out.m[5] = r1.z;
    out.m[6] = r2.x; out.m[7] = r2.y; out.m[8] = r2.z;
    return out;
}

/// T = [[1,0,0],[0,1,dy],[0,0,1]]：作用在齐次像素上就是 y 方向平移 dy。
inline Mat3 mat3_shear_y(float dy) {
    Mat3 out = mat3_identity();
    out.m[5] = dy;
    return out;
}

/// 3x4 = [K | c]，行主序。
inline void projection_of(const Mat3& k, float cx4, float cy4, float cz4, float p[12]) {
    p[0] = k.m[0]; p[1] = k.m[1]; p[2] = k.m[2]; p[3] = cx4;
    p[4] = k.m[3]; p[5] = k.m[4]; p[6] = k.m[5]; p[7] = cy4;
    p[8] = k.m[6]; p[9] = k.m[7]; p[10] = k.m[8]; p[11] = cz4;
}

/// Q：视差（**非负**）→ 矫正后 3D，见头文件的约定。
inline void q_of(float fx, float fy, float cx, float cy, float baseline_m, float q[16]) {
    for (int i = 0; i < 16; ++i) q[i] = 0.0f;
    q[0] = 1.0f / fx;
    q[3] = -cx / fx;
    q[5] = 1.0f / fy;
    q[7] = -cy / fy;
    q[11] = 1.0f;                       // Z' = 1，配合 W' = d / (fx*b) 得到 Z = fx*b/d
    q[14] = 1.0f / (fx * baseline_m);   // W' = d / (fx*b)
}

}  // namespace

// ---------------------------------------------------------------------------
// 3x3 小工具
// ---------------------------------------------------------------------------

Mat3 mat3_identity() {
    Mat3 out{};
    out.m[0] = out.m[4] = out.m[8] = 1.0f;
    return out;
}

Mat3 mat3_transpose(const Mat3& a) {
    Mat3 out{};
    out.m[0] = a.m[0]; out.m[1] = a.m[3]; out.m[2] = a.m[6];
    out.m[3] = a.m[1]; out.m[4] = a.m[4]; out.m[5] = a.m[7];
    out.m[6] = a.m[2]; out.m[7] = a.m[5]; out.m[8] = a.m[8];
    return out;
}

Mat3 mat3_mul(const Mat3& a, const Mat3& b) {
    Mat3 out{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            out.m[r * 3 + c] = a.m[r * 3 + 0] * b.m[0 * 3 + c] +
                               a.m[r * 3 + 1] * b.m[1 * 3 + c] +
                               a.m[r * 3 + 2] * b.m[2 * 3 + c];
        }
    }
    return out;
}

Vec3 mat3_mul_vec(const Mat3& a, const Vec3& v) {
    return Vec3{a.m[0] * v.x + a.m[1] * v.y + a.m[2] * v.z,
                a.m[3] * v.x + a.m[4] * v.y + a.m[5] * v.z,
                a.m[6] * v.x + a.m[7] * v.y + a.m[8] * v.z};
}

bool mat3_inverse(const Mat3& a, Mat3* out) {
    const float d = det3(a.m);
    if (!(std::fabs(d) > 1e-20f)) return false;
    const float inv = 1.0f / d;
    Mat3 r{};
    r.m[0] = (a.m[4] * a.m[8] - a.m[5] * a.m[7]) * inv;
    r.m[1] = (a.m[2] * a.m[7] - a.m[1] * a.m[8]) * inv;
    r.m[2] = (a.m[1] * a.m[5] - a.m[2] * a.m[4]) * inv;
    r.m[3] = (a.m[5] * a.m[6] - a.m[3] * a.m[8]) * inv;
    r.m[4] = (a.m[0] * a.m[8] - a.m[2] * a.m[6]) * inv;
    r.m[5] = (a.m[2] * a.m[3] - a.m[0] * a.m[5]) * inv;
    r.m[6] = (a.m[3] * a.m[7] - a.m[4] * a.m[6]) * inv;
    r.m[7] = (a.m[1] * a.m[6] - a.m[0] * a.m[7]) * inv;
    r.m[8] = (a.m[0] * a.m[4] - a.m[1] * a.m[3]) * inv;
    *out = r;
    return true;
}

Mat3 mat3_from_intrinsics(float fx, float fy, float cx, float cy) {
    // 标准针孔矩阵 P = [[fx,0,cx],[0,fy,cy],[0,0,1]]。
    // 注意别把它写成 [[fx,0,0],[0,fy,0],[cx,cy,1]] —— 两者互为转置，
    // P 直接乘 3D 点，而转置只对“齐次像素”有意义，乘混了会静默算错。
    Mat3 out{};
    out.m[0] = fx;
    out.m[2] = cx;
    out.m[4] = fy;
    out.m[5] = cy;
    out.m[8] = 1.0f;
    return out;
}

// ---------------------------------------------------------------------------
// 极线矫正
// ---------------------------------------------------------------------------

bool rectify_geometry(const RectifyInput& in, RectifyOutput* out) {
    if (out == nullptr) return false;
    if (in.src_width <= 0 || in.work_width <= 0 || in.work_height <= 0) return false;
    if (!(in.wide.fx > 0.0f) || !(in.wide.fy > 0.0f)) return false;
    if (!(in.ultra.fx > 0.0f) || !(in.ultra.fy > 0.0f)) return false;

    // ---- 外参；flip 时取逆（X_ultra = R^T * X_wide - R^T * t）----
    Mat3 R = in.extrinsic.R;
    Vec3 t = in.extrinsic.t;
    if (in.flip) {
        R = mat3_transpose(R);
        const Vec3 rt = mat3_mul_vec(R, t);
        t = Vec3{-rt.x, -rt.y, -rt.z};
    }

    const float baseline_mm = std::sqrt(t.x * t.x + t.y * t.y + t.z * t.z);
    if (!(baseline_mm > 0.0f)) return false;

    const float dir_f = t.x >= 0.0f ? 1.0f : -1.0f;
    const int dir = dir_f > 0.0f ? 1 : -1;

    // ---- 矫正旋转 Rr：把基线转到 +x，两路共用（Rl == Rr）----
    const Vec3 e1 = normalized(t);                       // 先归一，再乘 dir
    const Vec3 e1d{e1.x * dir_f, e1.y * dir_f, e1.z * dir_f};
    const Vec3 e2 = normalized(Vec3{-e1d.y, e1d.x, 0.0f});
    const Vec3 e3 = cross(e1d, e2);
    const Mat3 Rr = mat3_of_rows(e1d, e2, e3);

    // ---- 矫正后的共同内参：宽相机 K 缩放到工作分辨率 ----
    const float s = static_cast<float>(in.work_width) / static_cast<float>(in.src_width);
    Intrinsics K{};
    K.fx = in.wide.fx * s;
    K.fy = in.wide.fy * s;
    K.cx = in.wide.cx * s;
    K.cy = in.wide.cy * s;

    const Mat3 Kr = mat3_from_intrinsics(K.fx, K.fy, K.cx, K.cy);
    const Mat3 KW = mat3_from_intrinsics(in.wide.fx, in.wide.fy, in.wide.cx, in.wide.cy);
    const Mat3 KU = mat3_from_intrinsics(in.ultra.fx, in.ultra.fy, in.ultra.cx, in.ultra.cy);

    Mat3 Kr_inv{};
    if (!mat3_inverse(Kr, &Kr_inv)) return false;

    const Mat3 RrT = mat3_transpose(Rr);
    const Mat3 back = mat3_mul(RrT, Kr_inv);   // 矫正后像素 → 宽相机方向

    RectifyOutput g{};
    g.to_wide_image = mat3_mul(KW, back);
    // MU = KU * R^T * back * T（T 是 dy 的行偏移）
    const Mat3 RT = mat3_transpose(R);
    g.to_ultra_image = mat3_mul(mat3_mul(mat3_mul(KU, RT), back), mat3_shear_y(in.dy));
    g.to_rect_image = mat3_mul(mat3_mul(Kr, Rr), Kr_inv);

    g.K = K;
    g.dir = dir;
    g.baseline_mm = baseline_mm;
    g.baseline_deg = static_cast<float>(std::atan2(static_cast<double>(t.y),
                                                   static_cast<double>(t.x)) *
                                        180.0 / 3.14159265358979323846);

    const float baseline_m = baseline_mm / 1000.0f;
    g.focal_baseline = K.fx * baseline_m;

    g.Rl = Rr;
    g.Rr = Rr;
    // Pl = K [I | 0]；Pr = K [I | (-dir*b, 0, 0)]（Rr*t 精确等于 (dir*b, 0, 0)）
    projection_of(Kr, 0.0f, 0.0f, 0.0f, g.P_left);
    projection_of(Kr, -static_cast<float>(dir) * baseline_m, 0.0f, 0.0f, g.P_right);
    q_of(K.fx, K.fy, K.cx, K.cy, baseline_m, g.Q);

    *out = g;
    return true;
}

}  // namespace stereo
