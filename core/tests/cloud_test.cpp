// cloud_test.cpp — 导出用像素运算的自检（点云 PLY / 深度裁剪 / 打包 / 拷贝）
//
// 这几块的风险不在"算错"，在"格式写错一点点"：PLY 头部的顶点数写错、小数位数不对、
// 颜色分量顺序反了，都会让文件在某些工具里打不开或颜色怪异，而 App 里看不出来。
// 所以这里逐条比字符串，而不是只看长度。

#include "stereo/cloud.hpp"
#include "stereo/stereo_c_api.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_fail = 0;

void report(const char* name, bool ok, const char* detail = "") {
    std::printf("  %-40s %s%s%s\n", name, ok ? "OK  " : "FAIL", detail[0] ? "  " : "", detail);
    if (!ok) ++g_fail;
}

/// 按行切分，便于逐行比较。
std::vector<std::string> lines(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        const size_t nl = s.find('\n', start);
        if (nl == std::string::npos) break;
        out.push_back(s.substr(start, nl - start));
        start = nl + 1;
    }
    return out;
}

}  // namespace

int main() {
    using namespace stereo;

    // ---------------- 1) 点云 PLY ----------------
    {
        const int w = 4, h = 3;
        // 深度：只有一个像素无效（0），其余 1.0 m
        std::vector<float> depth(static_cast<size_t>(w) * h, 1.0f);
        depth[5] = 0.0f;
        // 彩色：BGRA，宽度与深度相同，方便核对取样
        std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4, 0);
        for (int i = 0; i < w * h; ++i) {
            rgba[static_cast<size_t>(i) * 4 + 0] = static_cast<uint8_t>(i);       // B
            rgba[static_cast<size_t>(i) * 4 + 1] = static_cast<uint8_t>(i + 10);  // G
            rgba[static_cast<size_t>(i) * 4 + 2] = static_cast<uint8_t>(i + 20);  // R
            rgba[static_cast<size_t>(i) * 4 + 3] = 255;
        }
        Intrinsics k;
        k.fx = 10.0f;
        k.fy = 10.0f;
        k.cx = 2.0f;
        k.cy = 1.5f;

        // 第一遍：问尺寸
        size_t need = 0;
        const bool ok1 = point_cloud_ply(depth.data(), w, w, h, k, rgba.data(), w * 4, w, h,
                                         nullptr, 0, &need);
        report("点云尺寸可预问", ok1 && need > 0);
        report("预问尺寸 <= 最坏情况",
               need <= point_cloud_ply_capacity(w, h), "最坏情况是上限");

        std::vector<char> buf(need);
        size_t written = 0;
        const bool ok2 = point_cloud_ply(depth.data(), w, w, h, k, rgba.data(), w * 4, w, h,
                                         buf.data(), buf.size(), &written);
        report("点云写入成功且长度一致", ok2 && written == need);

        const std::string ply(buf.data(), written);
        const auto ls = lines(ply);
        report("头部行序正确",
               ls.size() > 4 && ls[0] == "ply" && ls[1] == "format ascii 1.0" &&
                   ls[2] == "element vertex 11",
               ls.size() > 2 ? ls[2].c_str() : "");
        report("头部属性声明齐全",
               ply.find("property float x\nproperty float y\nproperty float z\n"
                        "property uchar red\nproperty uchar green\nproperty uchar blue\n"
                        "end_header\n") != std::string::npos);

        // 顶点数 = 12 - 1（跳过那个 0）
        report("顶点数 = 11", ls.size() == 10 + 11, "9 行头 + 11 行顶点");

        // 第一个顶点：像素 (0,0)，z=1，cx=2 cy=1.5 fx=fy=10
        //   X = (0+0.5-2)*1/10 = -0.15
        //   Y = (0+0.5-1.5)*1/10 = -0.10
        //   颜色 = i=0 → R=20 G=10 B=0（BGR 顺序取自 p[2],p[1],p[0]）
        // 头部 9 行（0..8 是声明，第 9 行是 end_header），顶点从第 10 行开始
        const std::string first = ls[10];
        report("首个顶点坐标/颜色格式正确", first == "-0.1500 -0.1000 1.0000 20 10 0", first.c_str());

        // 跳过无效像素后，第 6 个顶点应该来自像素 index 6（i=5 被跳过）
        //   x=6%4=2, y=6/4=1 → X=(2+0.5-2)/10=0.05, Y=(1+0.5-1.5)/10=0.00
        const std::string sixth = ls[10 + 5];
        report("无效像素被跳过", sixth == "0.0500 0.0000 1.0000 26 16 6", sixth.c_str());

        // 缓冲不足要拒绝，且不越界
        report("缓冲不足 -> 拒绝",
               !point_cloud_ply(depth.data(), w, w, h, k, rgba.data(), w * 4, w, h, buf.data(),
                                need - 1, &written));
        report("缺彩色图 -> 拒绝",
               !point_cloud_ply(depth.data(), w, w, h, k, nullptr, 0, w, h, buf.data(), need,
                                &written));
        // 全无效 → 只有头，顶点数 0
        std::vector<float> none(static_cast<size_t>(w) * h, 0.0f);
        size_t empty_len = 0;
        const bool ok3 = point_cloud_ply(none.data(), w, w, h, k, rgba.data(), w * 4, w, h,
                                        nullptr, 0, &empty_len);
        std::vector<char> ebuf(empty_len + 1, 0);
        size_t empty_written = 0;
        const bool ok4 = point_cloud_ply(none.data(), w, w, h, k, rgba.data(), w * 4, w, h,
                                         ebuf.data(), ebuf.size(), &empty_written);
        report("全无效深度 -> 0 顶点的合法 PLY",
               ok3 && ok4 && empty_written == empty_len &&
                   std::string(ebuf.data()).find("element vertex 0") != std::string::npos);
    }

    // ---------------- 2) 彩色图与深度尺寸不同（真实用法：全分辨率彩色 + 640×360 深度）------
    {
        const int w = 2, h = 2, cw = 8, ch = 8;
        std::vector<float> depth(static_cast<size_t>(w) * h, 2.0f);
        std::vector<uint8_t> rgba(static_cast<size_t>(cw) * ch * 4, 0);
        // 让每个像素的颜色 = 它的列号，便于验证取样那是列的哪一张
        for (int y = 0; y < ch; ++y) {
            for (int x = 0; x < cw; ++x) {
                uint8_t* p = &rgba[(static_cast<size_t>(y) * cw + x) * 4];
                p[0] = static_cast<uint8_t>(x);   // B
                p[2] = 0;
            }
        }
        Intrinsics k;
        k.fx = 100;
        k.fy = 100;
        k.cx = 0;
        k.cy = 0;
        size_t need = 0;
        point_cloud_ply(depth.data(), w, w, h, k, rgba.data(), cw * 4, cw, ch, nullptr, 0, &need);
        std::vector<char> buf(need);
        size_t written = 0;
        const bool ok = point_cloud_ply(depth.data(), w, w, h, k, rgba.data(), cw * 4, cw, ch,
                                        buf.data(), buf.size(), &written);
        const auto ls = lines(std::string(buf.data(), written));
        // 深度 x=1 → 彩色 x = 1*8/2 = 4 → 该像素 BGRA 的 B=4；PLY 里写的是 R G B = 0 0 4
        bool sampled = false;
        for (const auto& l : ls) {
            if (l.size() > 6 && l.compare(l.size() - 5, 5, "0 0 4") == 0) sampled = true;
        }
        report("彩色图按比例取样", ok && sampled, "2 列深度取到 8 列彩色的 x=0 / x=4");
    }

    // ---------------- 3) 深度裁剪 ----------------
    {
        const int w = 3, h = 2, pad = 2;
        std::vector<float> src(static_cast<size_t>(w) * h, 0.0f);
        src[0] = 0.5f;   // 范围内
        src[1] = 0.05f;  // 太小
        src[2] = std::nanf("");  // NaN
        src[3] = 5.0f;   // 太大
        src[4] = 1.0f;   // 范围内
        src[5] = -1.0f;  // 负
        std::vector<float> dst(static_cast<size_t>((w + pad) * h), -9.0f);
        const bool ok = depth_filter(src.data(), w, dst.data(), w + pad, w, h, 0.1f, 3.0f);
        const float want[6] = {0.5f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
        int bad = 0;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                if (std::memcmp(&dst[static_cast<size_t>(y) * (w + pad) + x],
                                &want[y * w + x], 4) != 0) {
                    ++bad;
                }
            }
            for (int x = w; x < w + pad; ++x) {
                if (dst[static_cast<size_t>(y) * (w + pad) + x] != -9.0f) ++bad;
            }
        }
        report("深度裁剪：范围 + NaN + 跨度填充都正确", ok && bad == 0);
    }

    // ---------------- 4) 16 位毫米打包 ----------------
    {
        const int w = 4, h = 1;
        const float in[4] = {0.0f, 0.12345f, 0.5f, std::nanf("")};
        uint16_t out[4] = {7, 7, 7, 7};
        const bool ok = depth_to_u16_mm(in, w, w, h, out);
        report("mm 打包：0 / 四舍五入 / NaN",
               ok && out[0] == 0 && out[1] == 123 && out[2] == 500 && out[3] == 0,
               "0,123,500,0");
        const float big[1] = {100.0f};
        uint16_t bo[1] = {0};
        depth_to_u16_mm(big, 1, 1, 1, bo);
        report("mm 打包：超出范围钳到 65535", bo[0] == 65535);
    }

    // ---------------- 5) 跨度拷贝 ----------------
    {
        const int w = 3, h = 2, pad = 2;
        std::vector<float> src(static_cast<size_t>((w + pad) * h), -1.0f);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) src[static_cast<size_t>(y) * (w + pad) + x] = y * 10.0f + x;
        }
        std::vector<float> dst(static_cast<size_t>(w) * h, 0.0f);
        const bool ok = copy_depth(src.data(), w + pad, w, h, dst.data());
        const float want[6] = {0, 1, 2, 10, 11, 12};
        report("跨度拷贝去掉行间填充",
               ok && std::memcmp(dst.data(), want, sizeof want) == 0);
    }

    // ---------------- 6) C 接口与 C++ 一致 ----------------
    {
        const int w = 4, h = 3;
        std::vector<float> depth(static_cast<size_t>(w) * h, 1.5f);
        depth[2] = 0.0f;
        std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4, 200);
        Intrinsics k;
        k.fx = 50;
        k.fy = 50;
        k.cx = 2;
        k.cy = 1;

        size_t need_c = 0;
        const bool ok_c1 = stereo_point_cloud(depth.data(), w, w, h, k.fx, k.fy, k.cx, k.cy,
                                             rgba.data(), w * 4, w, h, nullptr, 0, &need_c) == 0;
        std::vector<char> a(need_c), b(need_c);
        size_t wa = 0, wb = 0;
        const bool ok_a = point_cloud_ply(depth.data(), w, w, h, k, rgba.data(), w * 4, w, h,
                                          a.data(), a.size(), &wa);
        const bool ok_b = stereo_point_cloud(depth.data(), w, w, h, k.fx, k.fy, k.cx, k.cy,
                                            rgba.data(), w * 4, w, h, b.data(), b.size(),
                                            &wb) == 0;
        report("C 接口点云与 C++ 逐字节一致", ok_c1 && ok_a && ok_b && wa == wb &&
                                                  std::memcmp(a.data(), b.data(), wa) == 0);
        report("C 接口拒绝 NULL",
               stereo_point_cloud(nullptr, w, w, h, 50, 50, 2, 1, rgba.data(), w * 4, w, h,
                                  a.data(), a.size(), &wa) != 0);

        std::vector<float> fd(static_cast<size_t>(w) * h, 0.0f);
        report("C 接口 depth_filter",
               stereo_depth_filter(depth.data(), w, fd.data(), w, w, h, 1.1f, 2.0f) == 0 &&
                   fd[0] == 1.5f && fd[2] == 0.0f);
        std::vector<uint16_t> mm(static_cast<size_t>(w) * h, 0);
        report("C 接口 depth_to_u16_mm",
               stereo_depth_to_u16_mm(depth.data(), w, w, h, mm.data()) == 0 && mm[0] == 1500);
        std::vector<float> cd(static_cast<size_t>(w) * h, 0.0f);
        report("C 接口 copy_depth",
               stereo_copy_depth(depth.data(), w, w, h, cd.data()) == 0 && cd[0] == 1.5f);
    }

    std::printf(g_fail ? "\n有 %d 项不一致\n" : "\n全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
