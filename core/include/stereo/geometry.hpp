// stereo/geometry.hpp — 双目四步里的第 2 步：极线矫正的**几何部分**
//
// 分工（与 test.md 的步骤编号对应）：
//   第 1 步 双目标定      拿内参/外参（iOS 上是 AVFoundation，留在 Swift：Calibration.swift）
//   第 2 步 极线矫正      **本文件**：由内参+外参求矫正映射；重采样在 rectify.hpp
//   第 3 步 SGM            sgm.hpp（已存在）
//   第 4 步 深度恢复      sgm.hpp 的 to_depth + rectify.hpp 的 unrectify_depth
//
// ---------------------------------------------------------------------------
// 为什么这里没有 Rl / Rr / Pl / Pr / Q 的"求解"过程
// ---------------------------------------------------------------------------
// Bouguet / OpenCV 的 stereoRectify 要解 Rl、Rr 两个旋转，是因为它要**同时**
// 满足两件事：①极线水平；②两幅图的内参统一成 K_new 且畸变补干净。它用的是
// "把光轴转到基线上"的折中解。
//
// 本管线的前提不一样：iPhone 的 constituent 相机输出**已经去畸变**（
// `AVCaptureDevice` 不做像素级畸变校正，但 Dual-Wide 的每个 constituent 输出
// 都是逐台相机自校正过的正方形像素图，且两路同尺寸），而且我们不需要把光轴
// 对到基线上——只需要**极线与图像 x 轴平行**。所以：
//
//   * 畸变项 = 0                      → 不需要 rl/rr 的切向修正
//   * 两个矫正后相机共用同一个内参 K   → 取宽相机的 K 缩放到工作分辨率
//   * 唯一需要的旋转 = 一个 Rr：把基线转到 x 轴（roll），两路共用
//
// 于是 Rl = Rr = Rr，Kl = Kr = K，矫正后的投影矩阵就是标准的
//     Pl = K [I | 0]        Pr = K [I | (-dir * b, 0, 0)]
// 因为 Rr 的三行是 (e1, e2, e3)、e1 ∝ 基线，所以 Rr * t = (dir*b, 0, 0) 是**精确**
// 的（不是近似）：e2 ⟂ t、e3 ⟂ t 由构造保证。Q 也就能写成闭式（见下面 q_of()）。
//
// 这组 Pl/Pr/Q 与 Bouguet 的解在"极线水平 + 同一内参"这个意义上**等价**，
// 只是本管线额外允许 dy（亚像素行偏移）与 flip（基线方向交换）两个自由度，
// 那是 iPhone 双摄标定的系统性误差补偿，标准 Bouguet 里没有。
//
// 设计约束与 sgm.hpp 一致：零 Apple 依赖、不动态分配、不抛异常、float32。

#pragma once

namespace stereo {

/// 3x3 行主序矩阵：`m[row * 3 + col]`，作用在**列向量**上（p' = M * p）。
struct Mat3 {
    float m[9];
};

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

Mat3 mat3_identity();
Mat3 mat3_transpose(const Mat3& a);
/// a * b（先 b 后 a）。
Mat3 mat3_mul(const Mat3& a, const Mat3& b);
Vec3 mat3_mul_vec(const Mat3& a, const Vec3& v);
/// 失败（奇异）返回 false，out 不变。
bool mat3_inverse(const Mat3& a, Mat3* out);
/// 针孔内参 → 3x3：row0 = (fx, 0, 0)，row1 = (0, fy, 0)，row2 = (cx, cy, 1)。
Mat3 mat3_from_intrinsics(float fx, float fy, float cx, float cy);

// ---------------------------------------------------------------------------
// 第 2 步的输入：第 1 步（Swift）从系统读到的数，原样搬进来，不做推导
// ---------------------------------------------------------------------------

/// 一台相机的内参。单位是**原始图像像素**。
struct Intrinsics {
    float fx = 0, fy = 0, cx = 0, cy = 0;
};

/// 两台相机之间的外参：`X_wide = R * X_ultra + t`。
/// t 的单位与基线一致（AVFoundation 给**毫米**）。
struct Extrinsics {
    Mat3 R;
    Vec3 t;
};

struct RectifyInput {
    Intrinsics wide{};   ///< 宽相机内参（原始图像像素）
    Intrinsics ultra{};  ///< 超广角内参（原始图像像素）
    Extrinsics extrinsic{};

    int src_width = 0;   ///< 原始图宽，用来把内参缩放到工作分辨率
    int work_width = 640;
    int work_height = 360;

    int flip = 0;        ///< 交换基线方向（外参取逆）
    float dy = 0.0f;     ///< 矫正后行偏移（亚像素，单位是工作分辨率像素）
};

// ---------------------------------------------------------------------------
// 第 2 步的输出
// ---------------------------------------------------------------------------

/// 矫正映射。三个矩阵都是"齐次像素坐标"之间的单应：
/// `p_dst ∝ M * p_src`，用之前要除以第三分量。
struct RectifyOutput {
    Mat3 to_wide_image{};   ///< 矫正后左图像素 → **宽相机原始**像素（MW）
    Mat3 to_ultra_image{};  ///< 矫正后左图像素 → **超广角原始**像素（MU）
    Mat3 to_rect_image{};   ///< 宽相机像素 → 矫正后图像素（MUp，深度回转用）

    /// 矫正后的共同内参（= 宽相机 K 按 src_width / work_width 缩放）。
    Intrinsics K{};
    /// +1：匹配在左图右侧；-1：在左侧（由基线方向决定）。
    int dir = 1;

    /// fB = K.fx * baseline_m，配合 to_depth 用（输出单位 = 米）。
    float focal_baseline = 0.0f;
    float baseline_mm = 0.0f;
    /// 基线相对宽相机图像 x 轴的角度（度）。iPhone 13-15 的对角双摄约 ±45°。
    float baseline_deg = 0.0f;

    /// 两个矫正旋转（本管线里恒等，见文件头）。
    Mat3 Rl{};
    Mat3 Rr{};
    /// 3x4 投影矩阵，行主序，与 OpenCV 的 P1/P2 同义。
    float P_left[12]{};
    float P_right[12]{};
    /// 4x4 视差→3D 矩阵。把 (x_l, y, d, 1) 映射成 (X, Y, Z, 1)（**齐次**，要除以 w）。
    /// d 是 `to_depth` 用的**非负**视差，所以 Q 与 dir 无关：
    ///     Z = fx * b / d,  X = (x_l - cx) * b / d,  Y = (y - cy) * b / d
    /// 坐标是**矫正后**相机坐标系（x 右、y 下、z 前），单位米。
    float Q[16]{};
};

/// 解极线矫正。
///
/// 失败条件：src_width <= 0、work_width <= 0、外参为零向量、K 奇异。失败时
/// *out 不变并返回 false。
bool rectify_geometry(const RectifyInput& in, RectifyOutput* out);

}  // namespace stereo
