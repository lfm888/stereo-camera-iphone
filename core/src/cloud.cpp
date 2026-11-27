// stereo/cloud.cpp — 导出用的像素运算
//
// 与改造前 Swift 的对应关系：
//   Capture.ply()        → point_cloud_ply()
//   StereoEngine.depthData 的过滤环 → depth_filter()
//   Capture.export 的 depth_mm 打包 → depth_to_u16_mm()
//   StereoFrame.depthCopy() → copy_depth()
//
// 格式化用 std::to_chars 而不是 snprintf：不malloc、不碰 locale，
// 而且烧 14 万行时比 snprintf 快两倍以上（这正是原来卡顿的地方）。

#include "stereo/cloud.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace stereo {
namespace {

/// 每个顶点行最多多少字节：三个 fixed(4) 浮点 + 三个字节值 + 分隔符。
/// 3×(符号+5 整数+点+4 小数+空格) + 3×(3+空格) + 换行 ≈ 56，取 64 留余量。
constexpr size_t kVertexLineMax = 64;
constexpr size_t kHeaderMax = 256;

/// 一行顶点。返回写出的字节数（不含 '\0'），格式与 Swift 的
/// String(format: "%.4f %.4f %.4f %d %d %d\n", ...) 一致。
size_t format_vertex(char* buf, size_t cap, float x, float y, float z,
                     uint8_t r, uint8_t g, uint8_t b) {
    char* p = buf;
    char* end = buf + cap;
    const float values[3] = {x, y, z};
    for (int i = 0; i < 3; ++i) {
        const std::to_chars_result res =
            std::to_chars(p, end, values[i], std::chars_format::fixed, 4);
        if (res.ec != std::errc()) return 0;
        p = res.ptr;
        if (i < 2) *p++ = ' ';
    }
    const uint8_t rgb[3] = {r, g, b};
    for (int i = 0; i < 3; ++i) {
        *p++ = ' ';
        const std::to_chars_result res = std::to_chars(p, end, static_cast<unsigned>(rgb[i]));
        if (res.ec != std::errc()) return 0;
        p = res.ptr;
    }
    *p++ = '\n';
    return static_cast<size_t>(p - buf);
}

/// 头部长度依赖顶点数，所以只能算出来再格式化一次。
size_t format_header(char* buf, size_t cap, size_t vertices) {
    const int n = std::snprintf(buf, cap,
                                "ply\nformat ascii 1.0\nelement vertex %zu\n"
                                "property float x\nproperty float y\nproperty float z\n"
                                "property uchar red\nproperty uchar green\nproperty uchar blue\n"
                                "end_header\n",
                                vertices);
    return (n > 0 && static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : 0;
}

}  // namespace

size_t point_cloud_ply_capacity(int width, int height) {
    if (width <= 0 || height <= 0) return 0;
    return kHeaderMax + static_cast<size_t>(width) * static_cast<size_t>(height) * kVertexLineMax;
}

bool point_cloud_ply(const float* depth, int depth_stride, int width, int height,
                     const Intrinsics& k,
                     const uint8_t* rgba, int rgba_stride, int rgba_width, int rgba_height,
                     char* out, size_t capacity, size_t* out_bytes) {
    if (depth == nullptr || out_bytes == nullptr) return false;
    if (width <= 0 || height <= 0) return false;
    if (depth_stride <= 0) depth_stride = width;
    if (!(k.fx > 0.0f) || !(k.fy > 0.0f)) return false;
    if (rgba == nullptr || rgba_width <= 0 || rgba_height <= 0) return false;
    if (rgba_stride <= 0) rgba_stride = rgba_width * 4;

    // 第一遍：数顶点（头部要写顶点数，只能先知道）。
    size_t vertices = 0;
    for (int y = 0; y < height; ++y) {
        const float* row = depth + static_cast<size_t>(y) * static_cast<size_t>(depth_stride);
        for (int x = 0; x < width; ++x) {
            if (row[x] > 0.0f) ++vertices;
        }
    }

    char header[kHeaderMax];
    const size_t header_len = format_header(header, sizeof header, vertices);
    if (header_len == 0) return false;

    // out == nullptr：只累加长度（用一个栈上的小缓冲逐行格式化），得到精确尺寸。
    const bool measure_only = (out == nullptr);
    char scratch[kVertexLineMax];
    size_t total = header_len;
    if (!measure_only) {
        if (capacity < header_len) {
            *out_bytes = header_len + vertices * kVertexLineMax;
            return false;
        }
        std::memcpy(out, header, header_len);
    }

    for (int y = 0; y < height; ++y) {
        const float* row = depth + static_cast<size_t>(y) * static_cast<size_t>(depth_stride);
        // 颜色按比例取样（与 Swift 的 min(ch-1, y*ch/h) 一致）
        int sy = y * rgba_height / height;
        if (sy > rgba_height - 1) sy = rgba_height - 1;
        const uint8_t* color_row = rgba + static_cast<size_t>(sy) * static_cast<size_t>(rgba_stride);
        for (int x = 0; x < width; ++x) {
            const float z = row[x];
            if (!(z > 0.0f)) continue;
            const float X = (static_cast<float>(x) + 0.5f - k.cx) * z / k.fx;
            const float Y = (static_cast<float>(y) + 0.5f - k.cy) * z / k.fy;
            int sx = x * rgba_width / width;
            if (sx > rgba_width - 1) sx = rgba_width - 1;
            const uint8_t* p = color_row + static_cast<size_t>(sx) * 4;   // BGRA
            const size_t len = format_vertex(scratch, sizeof scratch, X, Y, z, p[2], p[1], p[0]);
            if (len == 0) return false;
            if (!measure_only) {
                if (total + len > capacity) {
                    *out_bytes = header_len + vertices * kVertexLineMax;
                    return false;
                }
                std::memcpy(out + total, scratch, len);
            }
            total += len;
        }
    }

    *out_bytes = total;
    return true;
}

bool depth_filter(const float* src, int src_stride, float* dst, int dst_stride,
                  int width, int height, float min_z, float max_z) {
    if (src == nullptr || dst == nullptr) return false;
    if (width <= 0 || height <= 0) return false;
    if (src_stride <= 0) src_stride = width;
    if (dst_stride <= 0) dst_stride = width;
    for (int y = 0; y < height; ++y) {
        const float* in = src + static_cast<size_t>(y) * static_cast<size_t>(src_stride);
        float* out = dst + static_cast<size_t>(y) * static_cast<size_t>(dst_stride);
        for (int x = 0; x < width; ++x) {
            const float z = in[x];
            out[x] = (std::isfinite(z) && z >= min_z && z <= max_z) ? z : 0.0f;
        }
    }
    return true;
}

bool depth_to_u16_mm(const float* depth, int stride, int width, int height, uint16_t* out) {
    if (depth == nullptr || out == nullptr) return false;
    if (width <= 0 || height <= 0) return false;
    if (stride <= 0) stride = width;
    for (int y = 0; y < height; ++y) {
        const float* row = depth + static_cast<size_t>(y) * static_cast<size_t>(stride);
        uint16_t* o = out + static_cast<size_t>(y) * static_cast<size_t>(width);
        for (int x = 0; x < width; ++x) {
            float v = row[x] * 1000.0f;
            if (!std::isfinite(v) || !(v > 0.0f)) {
                o[x] = 0;
                continue;
            }
            if (v > 65535.0f) v = 65535.0f;
            // Swift 的 rounded() 默认是"四舍五入远离零"，与 lround 一致
            o[x] = static_cast<uint16_t>(std::lround(v));
        }
    }
    return true;
}

bool copy_depth(const float* src, int src_stride, int width, int height, float* dst) {
    if (src == nullptr || dst == nullptr) return false;
    if (width <= 0 || height <= 0) return false;
    if (src_stride <= 0) src_stride = width;
    for (int y = 0; y < height; ++y) {
        std::memcpy(dst + static_cast<size_t>(y) * static_cast<size_t>(width),
                    src + static_cast<size_t>(y) * static_cast<size_t>(src_stride),
                    static_cast<size_t>(width) * sizeof(float));
    }
    return true;
}

}  // namespace stereo
