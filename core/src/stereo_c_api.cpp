// stereo_c_api.cpp — 纯 C 接口的实现（三层架构里的「第一层对外门面」）
//
// 这一层**只做三件事**：参数校验、临时缓冲区管理、把 C 调用翻译成 C++ 算法调用。
// 算法本身在 sgm.cpp 里，不含任何平台相关代码。
//
// 缓冲区策略：跟着 context 走，尺寸不变就复用。所以第一帧之后每帧零分配——
// 这是把「C++ 核心」接进实时管线时最要紧的一条。

#include "stereo/stereo_c_api.h"

#include "stereo/geometry.hpp"
#include "stereo/rectify.hpp"
#include "stereo/sgm.hpp"

#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

// 与 C 头里的 typedef 对应（C++ 里必须写成 struct stereo_context）。
struct stereo_context {
    stereo_config cfg{};
    int W = 0;
    int H = 0;
    int D = 0;

    // 临时缓冲区（按尺寸复用）
    std::vector<float> luma_l;
    std::vector<float> luma_r;
    std::vector<uint64_t> census_l;
    std::vector<uint64_t> census_r;
    std::vector<uint8_t> cost;
    std::vector<uint16_t> sum;
    std::vector<float> disp_r;
    std::vector<float> disp_l;

    char err[256] = {0};
};

namespace {

/// 按格式把一行像素转成亮度。返回 false 表示格式不认识。
bool to_luma(const stereo_image &img, float *luma) {
    const int w = img.width, h = img.height;
    for (int y = 0; y < h; ++y) {
        const uint8_t *row = img.data + static_cast<size_t>(y) * static_cast<size_t>(img.stride);
        float *out = luma + static_cast<size_t>(y) * w;
        switch (img.format) {
            case STEREO_FORMAT_GRAY8:
                for (int x = 0; x < w; ++x) out[x] = static_cast<float>(row[x]) / 255.0f;
                break;
            case STEREO_FORMAT_RGB8:
                for (int x = 0; x < w; ++x) {
                    const uint8_t *p = row + x * 3;
                    out[x] = static_cast<float>(p[0]) / 255.0f * 0.299f +
                             static_cast<float>(p[1]) / 255.0f * 0.587f +
                             static_cast<float>(p[2]) / 255.0f * 0.114f;
                }
                break;
            case STEREO_FORMAT_BGR8:
                for (int x = 0; x < w; ++x) {
                    const uint8_t *p = row + x * 3;
                    out[x] = static_cast<float>(p[2]) / 255.0f * 0.299f +
                             static_cast<float>(p[1]) / 255.0f * 0.587f +
                             static_cast<float>(p[0]) / 255.0f * 0.114f;
                }
                break;
            case STEREO_FORMAT_RGBA8:
                for (int x = 0; x < w; ++x) {
                    const uint8_t *p = row + x * 4;
                    out[x] = static_cast<float>(p[0]) / 255.0f * 0.299f +
                             static_cast<float>(p[1]) / 255.0f * 0.587f +
                             static_cast<float>(p[2]) / 255.0f * 0.114f;
                }
                break;
            case STEREO_FORMAT_BGRA8:
                for (int x = 0; x < w; ++x) {
                    const uint8_t *p = row + x * 4;
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

int bytes_per_pixel(int format) {
    switch (format) {
        case STEREO_FORMAT_GRAY8: return 1;
        case STEREO_FORMAT_RGB8:
        case STEREO_FORMAT_BGR8: return 3;
        case STEREO_FORMAT_BGRA8:
        case STEREO_FORMAT_RGBA8: return 4;
        default: return 0;
    }
}

int fail(stereo_context *ctx, const char *msg) {
    std::snprintf(ctx->err, sizeof(ctx->err), "%s", msg);
    return -1;
}

/// 按 (W,H,D) 准备缓冲区；尺寸没变就什么都不做。
bool ensure(stereo_context *ctx, int w, int h, int d) {
    if (ctx->W == w && ctx->H == h && ctx->D == d) return true;
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    const size_t nc = n * static_cast<size_t>(d);
    try {
        ctx->luma_l.assign(n, 0.0f);
        ctx->luma_r.assign(n, 0.0f);
        ctx->census_l.assign(n, 0);
        ctx->census_r.assign(n, 0);
        ctx->cost.assign(nc, 0);
        ctx->sum.assign(nc, 0);
        ctx->disp_r.assign(n, 0.0f);
        ctx->disp_l.assign(n, 0.0f);
    } catch (const std::bad_alloc &) {
        return false;
    }
    ctx->W = w;
    ctx->H = h;
    ctx->D = d;
    return true;
}

bool usable(const stereo_image *img) {
    return img != nullptr && img->data != nullptr && img->width > 0 && img->height > 0 &&
           bytes_per_pixel(img->format) > 0 && img->stride >= img->width * bytes_per_pixel(img->format);
}

// 第 2 / 4 步的 POD → C++ 类型翻译。
//
// 这些**必须**放在 `extern "C" {` **外面**：它们返回 stereo::Intrinsics / stereo::Mat3
// 这类 C++ 类型，带 C 链接的声明 clang 会直接报
// `-Wreturn-type-c-linkage`（开了 -Werror 就是编译失败）。
stereo::Intrinsics to_intrinsics(const stereo_intrinsics &k) {
    stereo::Intrinsics out;
    out.fx = k.fx;
    out.fy = k.fy;
    out.cx = k.cx;
    out.cy = k.cy;
    return out;
}

stereo::Mat3 to_mat3(const float m[9]) {
    stereo::Mat3 out;
    for (int i = 0; i < 9; ++i) out.m[i] = m[i];
    return out;
}

void from_mat3(const stereo::Mat3 &m, float out[9]) {
    for (int i = 0; i < 9; ++i) out[i] = m.m[i];
}

bool valid_image(const stereo_image *img) {
    return img != nullptr && img->data != nullptr && img->width > 0 && img->height > 0 &&
           img->stride > 0;
}

}  // namespace

extern "C" {

stereo_context *stereo_create(void) {
    stereo_context *ctx = new (std::nothrow) stereo_context();
    if (ctx != nullptr) stereo_config_default(&ctx->cfg);
    return ctx;
}

void stereo_destroy(stereo_context *ctx) { delete ctx; }

void stereo_config_default(stereo_config *cfg) {
    if (cfg == nullptr) return;
    cfg->max_disp = 64;
    cfg->p1 = 8;
    cfg->p2 = 96;
    cfg->uniqueness = 0.1f;
    cfg->lr_check = 1;
    cfg->threads = 0;
}

int stereo_configure(stereo_context *ctx, const stereo_config *cfg) {
    if (ctx == nullptr) return -1;
    if (cfg == nullptr) return fail(ctx, "stereo_configure: cfg is NULL");
    if (cfg->max_disp < 4 || cfg->max_disp > 96) {
        return fail(ctx, "stereo_configure: max_disp must be within 4..96");
    }
    if (cfg->p1 < 0 || cfg->p2 < 0) return fail(ctx, "stereo_configure: p1/p2 must be >= 0");
    ctx->cfg = *cfg;
    // 参数变了，下一帧重新按新 D 准备缓冲区
    ctx->W = ctx->H = ctx->D = 0;
    return 0;
}

const char *stereo_last_error(const stereo_context *ctx) {
    if (ctx == nullptr) return "stereo_last_error: ctx is NULL";
    return ctx->err;
}

int stereo_match(stereo_context *ctx,
                 const stereo_image *left,
                 const stereo_image *right,
                 int dir,
                 float *disp,
                 int disp_stride) {
    if (ctx == nullptr) return -1;
    if (disp == nullptr) return fail(ctx, "stereo_match: disp is NULL");
    if (!usable(left) || !usable(right)) return fail(ctx, "stereo_match: bad left/right image");
    if (left->width != right->width || left->height != right->height) {
        return fail(ctx, "stereo_match: left/right size mismatch");
    }
    if (dir != 1 && dir != -1) return fail(ctx, "stereo_match: dir must be +1 or -1");

    const int W = left->width, H = left->height, D = ctx->cfg.max_disp;
    if (disp_stride <= 0) disp_stride = W;
    if (!ensure(ctx, W, H, D)) return fail(ctx, "stereo_match: out of memory");

    if (!to_luma(*left, ctx->luma_l.data())) return fail(ctx, "stereo_match: bad left format");
    if (!to_luma(*right, ctx->luma_r.data())) return fail(ctx, "stereo_match: bad right format");

    stereo::SgmParams p;
    p.W = W;
    p.H = H;
    p.D = D;
    p.dir = dir;
    p.P1 = static_cast<unsigned>(ctx->cfg.p1);
    p.P2 = static_cast<unsigned>(ctx->cfg.p2);
    p.uniqueness = ctx->cfg.uniqueness;
    p.lrCheck = ctx->cfg.lr_check;

    stereo::census(ctx->luma_l.data(), W, H, ctx->census_l.data());
    stereo::census(ctx->luma_r.data(), W, H, ctx->census_r.data());
    stereo::cost_volume(ctx->census_l.data(), ctx->census_r.data(), p, ctx->cost.data());
    stereo::sgm_all_paths(ctx->cost.data(), ctx->sum.data(), ctx->luma_l.data(), p);

    // wta_right 的结果是 wta_left 的输入（左右一致性检查），两张图不能是同一块缓冲区。
    stereo::wta_right(ctx->sum.data(), p, ctx->disp_r.data());
    stereo::wta_left(ctx->sum.data(), ctx->disp_r.data(), p, ctx->disp_l.data());

    // 输出到调用方的缓冲区（可能带行跨度）
    for (int y = 0; y < H; ++y) {
        std::memcpy(disp + static_cast<size_t>(y) * static_cast<size_t>(disp_stride),
                    ctx->disp_l.data() + static_cast<size_t>(y) * W,
                    static_cast<size_t>(W) * sizeof(float));
    }
    ctx->err[0] = '\0';
    return 0;
}

int stereo_disparity_to_depth(stereo_context *ctx,
                              const float *disp,
                              int disp_stride,
                              int width,
                              int height,
                              float fB,
                              float min_z,
                              float max_z,
                              float *depth,
                              int depth_stride) {
    if (ctx == nullptr) return -1;
    if (disp == nullptr || depth == nullptr) return fail(ctx, "to_depth: NULL buffer");
    if (width <= 0 || height <= 0) return fail(ctx, "to_depth: bad size");
    if (disp_stride <= 0) disp_stride = width;
    if (depth_stride <= 0) depth_stride = width;

    // to_depth 现在支持行跨度，所以不再需要搬进紧凑缓冲区。
    stereo::to_depth(disp, disp_stride, width, height, fB, min_z, max_z, depth, depth_stride);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 第 2 步：极线矫正 + 第 4 步的深度回转                                  */
/* ------------------------------------------------------------------ */

int stereo_rectify_geometry(const stereo_rectify_input *in, stereo_rectify_output *out) {
    if (in == nullptr || out == nullptr) return -1;

    stereo::RectifyInput ri;
    ri.wide = to_intrinsics(in->wide);
    ri.ultra = to_intrinsics(in->ultra);
    ri.extrinsic.R = to_mat3(in->R);
    ri.extrinsic.t = stereo::Vec3{in->t[0], in->t[1], in->t[2]};
    ri.src_width = in->src_width;
    ri.work_width = in->work_width;
    ri.work_height = in->work_height;
    ri.flip = in->flip;
    ri.dy = in->dy;

    stereo::RectifyOutput g;
    if (!stereo::rectify_geometry(ri, &g)) return -1;

    from_mat3(g.to_wide_image, out->to_wide_image);
    from_mat3(g.to_ultra_image, out->to_ultra_image);
    from_mat3(g.to_rect_image, out->to_rect_image);
    out->k.fx = g.K.fx;
    out->k.fy = g.K.fy;
    out->k.cx = g.K.cx;
    out->k.cy = g.K.cy;
    out->dir = g.dir;
    out->focal_baseline = g.focal_baseline;
    out->baseline_mm = g.baseline_mm;
    out->baseline_deg = g.baseline_deg;
    from_mat3(g.Rl, out->rl);
    from_mat3(g.Rr, out->rr);
    for (int i = 0; i < 12; ++i) {
        out->p_left[i] = g.P_left[i];
        out->p_right[i] = g.P_right[i];
    }
    for (int i = 0; i < 16; ++i) out->q[i] = g.Q[i];
    return 0;
}

int stereo_rectify_image(const stereo_image *src, const float m[9], stereo_image *dst) {
    if (!valid_image(src) || !valid_image(dst) || m == nullptr) return -1;

    stereo::ImageView s;
    s.data = src->data;
    s.width = src->width;
    s.height = src->height;
    s.stride = src->stride;
    s.format = src->format;

    stereo::MutableImageView d;
    d.data = const_cast<uint8_t *>(dst->data);
    d.width = dst->width;
    d.height = dst->height;
    d.stride = dst->stride;
    d.format = dst->format;

    return stereo::rectify_image(s, to_mat3(m), d) ? 0 : -1;
}

int stereo_unrectify_depth(const float *src, int src_stride, int src_width, int src_height,
                           const float m[9], float *dst, int dst_stride, int dst_width,
                           int dst_height) {
    if (src == nullptr || dst == nullptr || m == nullptr) return -1;
    return stereo::unrectify_depth(src, src_width, src_height, src_stride, to_mat3(m), dst,
                                   dst_width, dst_height, dst_stride)
               ? 0
               : -1;
}

} /* extern "C" */
