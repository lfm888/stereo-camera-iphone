// npy.hpp — 极简 .npy 读取（只为测试服务，不进入核心库）
//
// 支持 numpy.save() 写出的 v1/v2/v3 格式：小端、C 顺序、非 pickle。
// 能解析的 descr：'|u1' / '<u2' / '<u8' / '<f4'（gen_golden.py 只会产出这几种）。
// 不理解 fortran_order，遇到直接报错——测试向量必须是 C 顺序。

#pragma once

#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace npy {

struct Array {
    std::string dtype;            ///< 归一化后的 dtype，如 "u1" "u2" "u4" "f4"
    std::vector<size_t> shape;
    std::vector<uint8_t> bytes;   ///< 原始数据（长度 = 元素数 * 元素字节数）

    size_t count() const {
        size_t n = 1;
        for (size_t d : shape) n *= d;
        return n;
    }

    template <typename T>
    const T* data() const {
        return reinterpret_cast<const T*>(bytes.data());
    }
};

inline Array load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);

    char magic[6] = {0};
    f.read(magic, 6);
    if (std::memcmp(magic, "\x93NUMPY", 6) != 0) throw std::runtime_error("not an .npy: " + path);

    unsigned char ver[2] = {0, 0};
    f.read(reinterpret_cast<char*>(ver), 2);

    size_t hlen = 0;
    if (ver[0] == 1) {
        unsigned char len[2];
        f.read(reinterpret_cast<char*>(len), 2);
        hlen = static_cast<size_t>(len[0]) | (static_cast<size_t>(len[1]) << 8);
    } else {
        unsigned char len[4];
        f.read(reinterpret_cast<char*>(len), 4);
        for (int i = 0; i < 4; ++i) hlen |= static_cast<size_t>(len[i]) << (8 * i);
    }

    std::string header(hlen, '\0');
    f.read(header.data(), static_cast<std::streamsize>(hlen));

    if (header.find("'fortran_order': True") != std::string::npos ||
        header.find("\"fortran_order\": True") != std::string::npos) {
        throw std::runtime_error("fortran_order arrays are not supported: " + path);
    }

    // descr —— 注意 key 本身带引号（'descr': '|u1'），要先跳过 key 自己的收尾引号
    Array a;
    size_t p = header.find("descr");
    if (p == std::string::npos) throw std::runtime_error("no descr in " + path);
    p = header.find(':', p);            // key 后面的冒号
    if (p == std::string::npos) throw std::runtime_error("bad descr in " + path);
    p = header.find('\'', p);           // 值前面的引号
    if (p == std::string::npos) throw std::runtime_error("bad descr in " + path);
    const size_t q = header.find('\'', p + 1);
    std::string descr = header.substr(p + 1, q - p - 1);
    // '<f4' / '|u1' / '<u8' → "f4" / "u1" / "u8"
    std::string norm;
    for (char ch : descr) {
        if (ch == '<' || ch == '>' || ch == '|' || ch == '=') continue;
        norm.push_back(ch);
    }
    if (descr[0] == '>') throw std::runtime_error("big-endian array: " + path);
    a.dtype = norm;

    // shape
    p = header.find("shape");
    if (p == std::string::npos) throw std::runtime_error("no shape in " + path);
    const size_t l = header.find('(', p);
    const size_t r = header.find(')', l);
    if (l == std::string::npos || r == std::string::npos)
        throw std::runtime_error("bad shape in " + path);
    const std::string body = header.substr(l + 1, r - l - 1);
    size_t start = 0;
    while (start < body.size()) {
        while (start < body.size() && (body[start] == ' ' || body[start] == ',')) ++start;
        if (start >= body.size()) break;
        size_t end = start;
        while (end < body.size() && body[end] >= '0' && body[end] <= '9') ++end;
        if (end == start) break;
        a.shape.push_back(static_cast<size_t>(std::stoull(body.substr(start, end - start))));
        start = end;
    }

    size_t elem = 0;
    if (norm == "u1" || norm == "i1") elem = 1;
    else if (norm == "u2" || norm == "i2" || norm == "f2") elem = 2;
    else if (norm == "u4" || norm == "i4" || norm == "f4") elem = 4;
    else if (norm == "u8" || norm == "i8" || norm == "f8") elem = 8;
    else throw std::runtime_error("unsupported dtype '" + norm + "' in " + path);

    a.bytes.resize(a.count() * elem);
    f.read(reinterpret_cast<char*>(a.bytes.data()), static_cast<std::streamsize>(a.bytes.size()));
    if (static_cast<size_t>(f.gcount()) != a.bytes.size()) throw std::runtime_error("truncated " + path);
    return a;
}

}  // namespace npy
