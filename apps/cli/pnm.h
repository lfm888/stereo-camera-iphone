// apps/cli/pnm.hpp — PNM / PFM 读写（零第三方依赖）
//
// 只做 CLI 需要的最小子集：
//   读：P5（8 位灰度）、P6（8 位 RGB）
//   写：P5（标签图）、PFM Pf（float32 深度/视差，行序自下而上，与 PFM 标准一致）
//
// 不支持 16 位样值、多图、ASCII（P2/P3）——那三种在 CLI 里没有使用场景，
// 遇到就报错而不是猜。

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cli {

struct Image {
    int width = 0;
    int height = 0;
    int channels = 0;                  ///< 1 = P5，3 = P6
    std::vector<uint8_t> pixels;       ///< 行紧凑，stride = width * channels
    int stride() const { return width * channels; }
};

/// 读 P5/P6（二进制）。失败时 err 带原因。
bool read_pnm(const std::string& path, Image& out, std::string& err);

/// 写 8 位灰度 PGM（P5）。
bool write_pgm(const std::string& path, const uint8_t* data, int width, int height,
               std::string& err);

/// 写 float32 PFM（Pf）。标准 PFM 的行序是自下而上，这里按标准写。
bool write_pfm(const std::string& path, const float* data, int width, int height,
               std::string& err);

}  // namespace cli
