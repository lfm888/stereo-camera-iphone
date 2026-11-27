// color.cpp — 像素格式 → 亮度 / BGRA8
//
// 代码逐字搬自改造前的 `src/stereo_c_api.cpp`（to_luma / convert_to_bgra8）：
// 系数、除法顺序、通道下标都没动，因为 golden 对拍依赖逐位一致。

#include "stereo/color.hpp"

#include <cstring>

namespace stereo {

bool to_luma(const ImageView& img, float* luma) {
    const int w = img.width, h = img.height;
    if (img.data == nullptr || luma == nullptr || w <= 0 || h <= 0 || img.stride <= 0) return false;
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = img.data + static_cast<size_t>(y) * static_cast<size_t>(img.stride);
        float* out = luma + static_cast<size_t>(y) * static_cast<size_t>(w);
        switch (img.format) {
            case kFormatGray8:
                for (int x = 0; x < w; ++x) out[x] = static_cast<float>(row[x]) / 255.0f;
                break;
            case kFormatRGB8:
                for (int x = 0; x < w; ++x) {
                    const uint8_t* p = row + x * 3;
                    out[x] = static_cast<float>(p[0]) / 255.0f * 0.299f +
                             static_cast<float>(p[1]) / 255.0f * 0.587f +
                             static_cast<float>(p[2]) / 255.0f * 0.114f;
                }
                break;
            case kFormatBGR8:
                for (int x = 0; x < w; ++x) {
                    const uint8_t* p = row + x * 3;
                    out[x] = static_cast<float>(p[2]) / 255.0f * 0.299f +
                             static_cast<float>(p[1]) / 255.0f * 0.587f +
                             static_cast<float>(p[0]) / 255.0f * 0.114f;
                }
                break;
            case kFormatRGBA8:
                for (int x = 0; x < w; ++x) {
                    const uint8_t* p = row + x * 4;
                    out[x] = static_cast<float>(p[0]) / 255.0f * 0.299f +
                             static_cast<float>(p[1]) / 255.0f * 0.587f +
                             static_cast<float>(p[2]) / 255.0f * 0.114f;
                }
                break;
            case kFormatBGRA8:
                for (int x = 0; x < w; ++x) {
                    const uint8_t* p = row + x * 4;
                    out[x] = static_cast<float>(p[2]) / 255.0f * 0.299f +
                             static_cast<float>(p[1]) / 255.0f * 0.587f +
                             static_cast<float>(p[0]) / 255.0f * 0.114f;
                }
                break;
            default:
                return false;
        }
    }
    return true;
}

bool to_bgra8(const ImageView& src, uint8_t* dst) {
    const int w = src.width, h = src.height;
    if (src.data == nullptr || dst == nullptr || w <= 0 || h <= 0 || src.stride <= 0) return false;
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = src.data + static_cast<size_t>(y) * static_cast<size_t>(src.stride);
        uint8_t* out = dst + static_cast<size_t>(y) * static_cast<size_t>(w) * 4;
        switch (src.format) {
            case kFormatBGRA8:
                std::memcpy(out, row, static_cast<size_t>(w) * 4);
                break;
            case kFormatRGBA8:
                for (int x = 0; x < w; ++x) {
                    out[x * 4 + 0] = row[x * 4 + 2];
                    out[x * 4 + 1] = row[x * 4 + 1];
                    out[x * 4 + 2] = row[x * 4 + 0];
                    out[x * 4 + 3] = row[x * 4 + 3];
                }
                break;
            case kFormatRGB8:
                for (int x = 0; x < w; ++x) {
                    out[x * 4 + 0] = row[x * 3 + 2];
                    out[x * 4 + 1] = row[x * 3 + 1];
                    out[x * 4 + 2] = row[x * 3 + 0];
                    out[x * 4 + 3] = 255;
                }
                break;
            case kFormatBGR8:
                for (int x = 0; x < w; ++x) {
                    out[x * 4 + 0] = row[x * 3 + 0];
                    out[x * 4 + 1] = row[x * 3 + 1];
                    out[x * 4 + 2] = row[x * 3 + 2];
                    out[x * 4 + 3] = 255;
                }
                break;
            case kFormatGray8:
                for (int x = 0; x < w; ++x) {
                    out[x * 4 + 0] = row[x];
                    out[x * 4 + 1] = row[x];
                    out[x * 4 + 2] = row[x];
                    out[x * 4 + 3] = 255;
                }
                break;
            default:
                return false;
        }
    }
    return true;
}

}  // namespace stereo
