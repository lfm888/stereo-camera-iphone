// stereo/rectify.cpp — 矫正重采样 + 深度回转（Shaders.metal 的 CPU 孪生）
//
// 与 Metal kernel 逐条对齐。两处刻意的实现选择：
//   * 双线性在**字节域**做（0..255 的 float 累加），而不是先低频化成 0..1 再乘回去；
//     数学上等价，但少一次除法，且避免 1/255 的表示误差在量化边界上翻位。
//   * 越界一律贡献 0（clamp_to_zero），不是钳到边缘像素 —— 这一点很重要：
//     钳到边缘会在图像边界拉出假的颜色，Census 在边界就会算错。

#include "stereo/rectify.hpp"

#include <cmath>

namespace stereo {
namespace {

/// 双线性取样，clamp_to_zero。px/py 是像素坐标（纹素中心在 +0.5）。
/// 累加进 acc[0..ch)，权重合计为 1（越界部分不贡献）。
inline void sample_linear(const uint8_t* data, int w, int h, int stride, int ch,
                          float px, float py, float acc[4]) {
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
        if (yy < 0 || yy >= h) continue;   // clamp_to_zero
        for (int i = 0; i < 2; ++i) {
            const float wgt = (i ? fx : (1.0f - fx)) * wy;
            if (wgt == 0.0f) continue;
            const int xx = x0 + i;
            if (xx < 0 || xx >= w) continue;   // clamp_to_zero
            const uint8_t* p = data + static_cast<size_t>(yy) * static_cast<size_t>(stride) +
                               static_cast<size_t>(xx) * static_cast<size_t>(ch);
            for (int c = 0; c < ch; ++c) acc[c] += static_cast<float>(p[c]) * wgt;
        }
    }
}

/// 齐次除法 + 单应变换，返回 false 表示这一点落在无穷远（按越界处理）。
inline bool project(const Mat3& m, float x, float y, float* ox, float* oy) {
    const float qz = m.m[6] * x + m.m[7] * y + m.m[8];
    if (!(std::fabs(qz) > 1e-12f)) return false;
    const float qx = m.m[0] * x + m.m[1] * y + m.m[2];
    const float qy = m.m[3] * x + m.m[4] * y + m.m[5];
    const float sx = qx / qz;
    const float sy = qy / qz;
    if (!std::isfinite(sx) || !std::isfinite(sy)) return false;
    *ox = sx;
    *oy = sy;
    return true;
}

inline uint8_t quantize(float v) {
    if (!(v > 0.0f)) return 0;
    if (v >= 255.0f) return 255;
    // 与 unorm8 的写回一致：就近取整（.5 走银行家舍入）
    return static_cast<uint8_t>(std::lrintf(v));
}

}  // namespace

int format_channels(int format) {
    switch (format) {
        case kFormatRGB8:
        case kFormatBGR8:
            return 3;
        case kFormatGray8:
            return 1;
        case kFormatBGRA8:
        case kFormatRGBA8:
            return 4;
        default:
            return 0;
    }
}

bool rectify_image(const ImageView& src, const Mat3& m, const MutableImageView& dst) {
    const int sc = format_channels(src.format);
    const int dc = format_channels(dst.format);
    if (sc == 0 || dc == 0) return false;        // 格式不认识
    if (src.format != dst.format) return false;  // 不隐式换通道顺序，见头文件
    if (src.data == nullptr || dst.data == nullptr) return false;
    if (src.width <= 0 || src.height <= 0 || dst.width <= 0 || dst.height <= 0) return false;
    if (src.stride < src.width * sc || dst.stride < dst.width * dc) return false;

    for (int y = 0; y < dst.height; ++y) {
        uint8_t* out = dst.data + static_cast<size_t>(y) * static_cast<size_t>(dst.stride);
        for (int x = 0; x < dst.width; ++x) {
            float acc[4] = {0, 0, 0, 0};
            for (int i = 0; i < 4; ++i) {
                // 与 kernel 的旋转网格顺序一致：(0.25,0.25) (0.75,0.25) (0.25,0.75) (0.75,0.75)
                const float ox = (i & 1) ? 0.75f : 0.25f;
                const float oy = (i >> 1) ? 0.75f : 0.25f;
                float sx = 0, sy = 0;
                if (!project(m, static_cast<float>(x) + ox, static_cast<float>(y) + oy, &sx, &sy)) continue;
                sample_linear(src.data, src.width, src.height, src.stride, sc, sx, sy, acc);
            }
            uint8_t* p = out + static_cast<size_t>(x) * static_cast<size_t>(dc);
            for (int c = 0; c < dc; ++c) p[c] = quantize(acc[c] * 0.25f);
        }
    }
    return true;
}

bool unrectify_depth(const float* src, int src_width, int src_height, int src_stride_px,
                     const Mat3& m, float* dst, int dst_width, int dst_height,
                     int dst_stride_px) {
    if (src == nullptr || dst == nullptr) return false;
    if (src_width <= 0 || src_height <= 0 || dst_width <= 0 || dst_height <= 0) return false;
    if (src_stride_px <= 0) src_stride_px = src_width;
    if (dst_stride_px <= 0) dst_stride_px = dst_width;

    for (int y = 0; y < dst_height; ++y) {
        float* out = dst + static_cast<size_t>(y) * static_cast<size_t>(dst_stride_px);
        for (int x = 0; x < dst_width; ++x) {
            const float px = static_cast<float>(x) + 0.5f;
            const float py = static_cast<float>(y) + 0.5f;
            const float qz = m.m[6] * px + m.m[7] * py + m.m[8];
            float z = 0.0f;
            if (qz > 1e-6f) {
                const float sx = (m.m[0] * px + m.m[1] * py + m.m[2]) / qz;
                const float sy = (m.m[3] * px + m.m[4] * py + m.m[5]) / qz;
                if (std::isfinite(sx) && std::isfinite(sy)) {
                    const int xs = static_cast<int>(std::floor(sx));
                    const int ys = static_cast<int>(std::floor(sy));
                    if (xs >= 0 && ys >= 0 && xs < src_width && ys < src_height) {
                        z = src[static_cast<size_t>(ys) * static_cast<size_t>(src_stride_px) + xs];
                    }
                }
            }
            out[x] = z;
        }
    }
    return true;
}

}  // namespace stereo
