// apps/cli/main.cpp — 第二个前端：读一对**原始图** + 标定文件，跑 CPU 全流水线
//
// 存在的意义（structure_v3_plan.md 阶段 3）：
//   * `stereo_process_frame` 原来只被测试调用，这是第一个"站在库外面"的真实使用者；
//   * **只 #include 公开 C 头**（stereo/stereo_c_api.h），不碰任何 .hpp ——
//     这同时验证了"公开面是否够用"；
//   * 零第三方依赖：图像用 PNM/PFM，标定用 key = value 文本。
//
// 用法：
//   stereo_cli --left L.pgm --right R.pgm --calib calib.txt
//              [--depth out.pfm] [--disp out.pfm] [--labels out.pgm] [--ply out.ply]

#include "stereo/stereo_c_api.h"

#include "calib.h"
#include "pnm.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

struct Options {
    std::string left, right, calib;
    std::string depth = "depth.pfm";
    std::string disp, labels, ply;
};

void usage(const char* argv0) {
    std::printf(
        "usage: %s --left L.pgm --right R.pgm --calib calib.txt\n"
        "          [--depth out.pfm] [--disp out.pfm] [--labels out.pgm] [--ply out.ply]\n",
        argv0);
}

bool take_value(int argc, char** argv, int& i, std::string& dst, std::string& err) {
    if (i + 1 >= argc) {
        err = std::string("missing value for ") + argv[i];
        return false;
    }
    dst = argv[++i];
    return true;
}

bool parse_args(int argc, char** argv, Options& o, std::string& err) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        bool ok = true;
        if (a == "--left") ok = take_value(argc, argv, i, o.left, err);
        else if (a == "--right") ok = take_value(argc, argv, i, o.right, err);
        else if (a == "--calib") ok = take_value(argc, argv, i, o.calib, err);
        else if (a == "--depth") ok = take_value(argc, argv, i, o.depth, err);
        else if (a == "--disp") ok = take_value(argc, argv, i, o.disp, err);
        else if (a == "--labels") ok = take_value(argc, argv, i, o.labels, err);
        else if (a == "--ply") ok = take_value(argc, argv, i, o.ply, err);
        else if (a == "-h" || a == "--help") {
            usage(argv[0]);
            std::exit(0);
        } else {
            err = "unknown argument: " + a;
            return false;
        }
        if (!ok) return false;
    }
    if (o.left.empty() || o.right.empty() || o.calib.empty()) {
        err = "--left / --right / --calib are required";
        return false;
    }
    return true;
}

/// 原始图（P5/P6）→ stereo_image POD。非 BGRA8 也可以：库内部会统一成 BGRA8。
stereo_image make_view(const cli::Image& img) {
    stereo_image v{};
    v.data = img.pixels.data();
    v.width = img.width;
    v.height = img.height;
    v.stride_bytes = img.stride();
    v.format = img.channels == 1 ? STEREO_FORMAT_GRAY8 : STEREO_FORMAT_RGB8;
    return v;
}

/// 原始图 → 紧凑 BGRA8（只给 PLY 上色用；CLI 是应用层，允许这种循环）。
std::vector<uint8_t> to_bgra(const cli::Image& img) {
    std::vector<uint8_t> out(static_cast<size_t>(img.width) * static_cast<size_t>(img.height) * 4,
                             255);
    for (int y = 0; y < img.height; ++y) {
        const uint8_t* row = img.pixels.data() + static_cast<size_t>(y) * img.stride();
        uint8_t* dst = out.data() + static_cast<size_t>(y) * img.width * 4;
        for (int x = 0; x < img.width; ++x) {
            if (img.channels == 1) {
                dst[x * 4 + 0] = row[x];
                dst[x * 4 + 1] = row[x];
                dst[x * 4 + 2] = row[x];
            } else {
                dst[x * 4 + 0] = row[x * 3 + 2];
                dst[x * 4 + 1] = row[x * 3 + 1];
                dst[x * 4 + 2] = row[x * 3 + 0];
            }
        }
    }
    return out;
}

/// 点云 PLY：先问精确尺寸（out == nullptr），再按尺寸分配。内参用**矫正后**的 K。
bool write_ply(const std::string& path, const float* depth, int w, int h,
               const cli::Image& color, float fx, float fy, float cx, float cy,
               std::string& err) {
    const std::vector<uint8_t> bgra = to_bgra(color);
    size_t need = 0;
    if (stereo_point_cloud(depth, w, w, h, fx, fy, cx, cy, bgra.data(), w * 4, w, h, nullptr, 0,
                           &need) != 0 ||
        need == 0) {
        err = "point cloud size query failed";
        return false;
    }
    std::vector<char> buf(need);
    size_t written = 0;
    if (stereo_point_cloud(depth, w, w, h, fx, fy, cx, cy, bgra.data(), w * 4, w, h, buf.data(),
                           buf.size(), &written) != 0) {
        err = "point cloud export failed";
        return false;
    }
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        err = "cannot write " + path;
        return false;
    }
    const bool ok = std::fwrite(buf.data(), 1, written, f) == written;
    std::fclose(f);
    if (!ok) err = "short write: " + path;
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    std::string err;
    if (!parse_args(argc, argv, opt, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        usage(argv[0]);
        return 2;
    }

    cli::Calibration cal;
    if (!cli::load_calibration(opt.calib, cal, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }

    cli::Image left, right;
    if (!cli::read_pnm(opt.left, left, err) || !cli::read_pnm(opt.right, right, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    if (left.width != right.width || left.height != right.height) {
        std::fprintf(stderr, "error: left/right size mismatch (%dx%d vs %dx%d)\n", left.width,
                     left.height, right.width, right.height);
        return 1;
    }
    if (left.width != cal.src_width) {
        std::fprintf(stderr, "error: images are %d wide but calib src_width = %d\n", left.width,
                     cal.src_width);
        return 1;
    }
    if (left.channels != right.channels) {
        std::fprintf(stderr, "error: left/right channel count differs\n");
        return 1;
    }

    stereo_context* ctx = stereo_create();
    if (ctx == nullptr) {
        std::fprintf(stderr, "error: stereo_create failed (out of memory)\n");
        return 1;
    }

    stereo_rectify_input geometry{};
    geometry.wide.fx = cal.fx;
    geometry.wide.fy = cal.fy;
    geometry.wide.cx = cal.cx;
    geometry.wide.cy = cal.cy;
    geometry.ultra.fx = cal.ufx;
    geometry.ultra.fy = cal.ufy;
    geometry.ultra.cx = cal.ucx;
    geometry.ultra.cy = cal.ucy;
    for (int i = 0; i < 9; ++i) geometry.R[i] = cal.R[i];
    for (int i = 0; i < 3; ++i) geometry.t[i] = cal.t[i];
    geometry.src_width = cal.src_width;
    geometry.work_width = cal.work_width;
    geometry.work_height = cal.work_height;
    geometry.flip = cal.flip;
    geometry.dy = cal.dy;

    stereo_config cfg{};
    stereo_config_default(&cfg);
    cfg.max_disp = cal.max_disp;
    cfg.p1 = cal.p1;
    cfg.p2 = cal.p2;
    cfg.uniqueness = cal.uniqueness;
    cfg.lr_check = cal.lr_check;

    const int W = cal.work_width, H = cal.work_height;
    const size_t n = static_cast<size_t>(W) * static_cast<size_t>(H);
    std::vector<float> depth(n, 0.0f);
    std::vector<float> disp;
    std::vector<unsigned char> labels;
    if (!opt.disp.empty()) disp.assign(n, 0.0f);
    if (!opt.labels.empty()) labels.assign(n, 0);

    stereo_image li = make_view(left);
    stereo_image ri = make_view(right);

    stereo_frame_input in{};
    in.left = &li;
    in.right = &ri;
    in.geometry = &geometry;
    in.config = &cfg;
    in.min_z = cal.min_z;
    in.max_z = cal.max_z;
    in.analysis.fx = 0;   // 会被矫正后的 K 覆盖
    in.analysis.max_blobs = 24;
    in.analysis.min_cells = 40;
    static const float kCenter[4] = {0.475f, 0.455f, 0.525f, 0.545f};
    in.center_rect = kCenter;
    in.center_q = 0.5f;

    stereo_frame_output out{};
    out.depth = depth.data();
    out.depth_stride_px = W;
    out.disp = disp.empty() ? nullptr : disp.data();
    out.disp_stride_px = W;
    out.labels = labels.empty() ? nullptr : labels.data();

    stereo_frame_timings timing{};
    const int rc = stereo_process_frame(ctx, &in, &out, &timing);
    if (rc != 0) {
        std::fprintf(stderr, "error: stereo_process_frame failed: %s\n", stereo_last_error(ctx));
        stereo_destroy(ctx);
        return 1;
    }

    size_t valid_depth = 0, valid_disp = 0;
    for (size_t i = 0; i < n; ++i) {
        if (depth[i] > 0.0f) ++valid_depth;
        if (!disp.empty() && disp[i] > 0.0f) ++valid_disp;
    }

    std::printf("work %dx%d · dir %+d · fB %.3f · K (%.2f, %.2f, %.2f, %.2f)\n", W, H,
                out.direction, static_cast<double>(out.focal_baseline),
                static_cast<double>(out.k.fx), static_cast<double>(out.k.fy),
                static_cast<double>(out.k.cx), static_cast<double>(out.k.cy));
    std::printf("valid depth %zu/%zu", valid_depth, n);
    if (!disp.empty()) std::printf(" · valid disp %zu/%zu", valid_disp, n);
    std::printf(" · blobs %d\n", out.analysis.blob_count);
    std::printf("timings ms: rectify %.2f · match %.2f · depth %.2f · unrectify %.2f · "
                "analyze %.2f · total %.2f\n",
                timing.rectify, timing.match, timing.depth, timing.unrectify, timing.analyze,
                timing.total);

    if (!cli::write_pfm(opt.depth, depth.data(), W, H, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        stereo_destroy(ctx);
        return 1;
    }
    std::printf("wrote %s\n", opt.depth.c_str());

    if (!disp.empty()) {
        if (!cli::write_pfm(opt.disp, disp.data(), W, H, err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            stereo_destroy(ctx);
            return 1;
        }
        std::printf("wrote %s\n", opt.disp.c_str());
    }
    if (!labels.empty()) {
        if (!cli::write_pgm(opt.labels, labels.data(), W, H, err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            stereo_destroy(ctx);
            return 1;
        }
        std::printf("wrote %s\n", opt.labels.c_str());
    }
    if (!opt.ply.empty()) {
        if (!write_ply(opt.ply, depth.data(), W, H, left,
                       out.k.fx, out.k.fy, out.k.cx, out.k.cy, err)) {
            std::fprintf(stderr, "error: %s\n", err.c_str());
            stereo_destroy(ctx);
            return 1;
        }
        std::printf("wrote %s\n", opt.ply.c_str());
    }

    stereo_destroy(ctx);
    return 0;
}
