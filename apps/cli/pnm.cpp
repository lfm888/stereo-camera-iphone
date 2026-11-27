// apps/cli/pnm.cpp — PNM / PFM 的最小实现

#include "pnm.h"

#include <cstdio>
#include <cstring>

namespace cli {
namespace {

/// 跳过空白与 # 注释（PNM 头允许注释行）。
bool skip_space_and_comments(FILE* f) {
    int c = std::fgetc(f);
    for (;;) {
        if (c == '#') {
            while (c != '\n' && c != EOF) c = std::fgetc(f);
        } else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            c = std::fgetc(f);
        } else {
            break;
        }
    }
    if (c == EOF) return false;
    std::ungetc(c, f);
    return true;
}

bool read_int(FILE* f, int& out) {
    if (!skip_space_and_comments(f)) return false;
    if (std::fscanf(f, "%d", &out) != 1) return false;
    return true;
}

}  // namespace

bool read_pnm(const std::string& path, Image& out, std::string& err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        err = "cannot open " + path;
        return false;
    }
    char magic[3] = {0, 0, 0};
    if (std::fscanf(f, "%2s", magic) != 1 ||
        (std::strcmp(magic, "P5") != 0 && std::strcmp(magic, "P6") != 0)) {
        std::fclose(f);
        err = "only binary PGM (P5) / PPM (P6) are supported: " + path;
        return false;
    }
    int w = 0, h = 0, maxval = 0;
    if (!read_int(f, w) || !read_int(f, h) || !read_int(f, maxval)) {
        std::fclose(f);
        err = "bad PNM header: " + path;
        return false;
    }
    if (w <= 0 || h <= 0 || maxval <= 0 || maxval > 255) {
        std::fclose(f);
        err = "unsupported PNM size/maxval (8-bit only): " + path;
        return false;
    }
    // 头之后**恰好**一个空白字符，然后是二进制数据。
    int c = std::fgetc(f);
    if (c == '\r') {
        c = std::fgetc(f);
        if (c != '\n') std::ungetc(c, f);
    } else if (c != ' ' && c != '\t' && c != '\n') {
        std::fclose(f);
        err = "missing whitespace after PNM header: " + path;
        return false;
    }

    const int channels = magic[1] == '5' ? 1 : 3;
    const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) *
                         static_cast<size_t>(channels);
    out.pixels.assign(bytes, 0);
    const size_t got = std::fread(out.pixels.data(), 1, bytes, f);
    std::fclose(f);
    if (got != bytes) {
        err = "truncated PNM data: " + path;
        return false;
    }
    out.width = w;
    out.height = h;
    out.channels = channels;
    return true;
}

bool write_pgm(const std::string& path, const uint8_t* data, int width, int height,
               std::string& err) {
    if (data == nullptr || width <= 0 || height <= 0) {
        err = "bad PGM arguments";
        return false;
    }
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        err = "cannot write " + path;
        return false;
    }
    std::fprintf(f, "P5\n%d %d\n255\n", width, height);
    const size_t n = static_cast<size_t>(width) * static_cast<size_t>(height);
    const bool ok = std::fwrite(data, 1, n, f) == n;
    std::fclose(f);
    if (!ok) err = "short write: " + path;
    return ok;
}

bool write_pfm(const std::string& path, const float* data, int width, int height,
               std::string& err) {
    if (data == nullptr || width <= 0 || height <= 0) {
        err = "bad PFM arguments";
        return false;
    }
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        err = "cannot write " + path;
        return false;
    }
    // -1.0 = little-endian（x86 / arm64 都是小端；大端平台不在本项目目标内）
    std::fprintf(f, "Pf\n%d %d\n-1.0\n", width, height);
    bool ok = true;
    for (int y = height - 1; y >= 0 && ok; --y) {
        ok = std::fwrite(data + static_cast<size_t>(y) * static_cast<size_t>(width),
                         sizeof(float), static_cast<size_t>(width), f) ==
             static_cast<size_t>(width);
    }
    std::fclose(f);
    if (!ok) err = "short write: " + path;
    return ok;
}

}  // namespace cli
