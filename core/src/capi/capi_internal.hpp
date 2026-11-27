// capi/capi_internal.hpp — C 门面的**唯一**内部头
//
// 这里放三样东西（structure_v3_plan.md §4.2 / §4.4）：
//   1. `struct stereo_context` 的**唯一定义**（按领域分组的 Workspace + 配置 + 错误串）；
//   2. 各领域共用的校验与 POD↔C++ 小工具；
//   3. 不变量：其它 .cpp 只能通过这里访问 context。
//
// 约束：本文件不出现像素循环、不出现算法。`capi/*.cpp` 只做
// "校验参数 → 换乘类型 → 调 stereo:: → 把失败转成返回码与错误串"。

#pragma once

#include "stereo/color.hpp"      // format_channels（usable() 用）
#include "stereo/params.hpp"     // config_default / param_*
#include "stereo/stereo_c_api.h"
#include "stereo/workspace.hpp"

#include <cstdio>
#include <cstring>

/// C 侧唯一句柄。成员按领域分组，**只在用到某领域时才分配**（WorkSpace 里是空 vector）。
struct stereo_context {
    stereo_config cfg{};
    stereo::Workspace ws;   ///< match / analyze / calibrate / frame
    char err[256] = {0};
};

namespace capi {

/// 写 last_error 并返回失败码（C API 的失败返回统一是 -1；0/1 的例外见 sample_depth）。
inline int fail(stereo_context* ctx, const char* msg) {
    if (ctx != nullptr) std::snprintf(ctx->err, sizeof(ctx->err), "%s", msg);
    return -1;
}

/// 只校验"指针和尺寸看起来能用"（不校验格式/跨度）。
inline bool valid_image(const stereo_image* img) {
    return img != nullptr && img->data != nullptr && img->width > 0 && img->height > 0 &&
           img->stride > 0;
}

/// 校验"格式认识、跨度足够"——匹配用（要按格式转亮度）。
inline bool usable(const stereo_image* img) {
    if (img == nullptr) return false;
    const int bpp = stereo::format_channels(img->format);
    return img->data != nullptr && img->width > 0 && img->height > 0 && bpp > 0 &&
           img->stride >= img->width * bpp;
}

inline stereo::Intrinsics to_intrinsics(const stereo_intrinsics& k) {
    stereo::Intrinsics out;
    out.fx = k.fx;
    out.fy = k.fy;
    out.cx = k.cx;
    out.cy = k.cy;
    return out;
}

inline stereo::Mat3 to_mat3(const float m[9]) {
    stereo::Mat3 out;
    for (int i = 0; i < 9; ++i) out.m[i] = m[i];
    return out;
}

inline void from_mat3(const stereo::Mat3& m, float out[9]) {
    for (int i = 0; i < 9; ++i) out[i] = m.m[i];
}

/// 第 1/2 步的 POD 换乘：C 的 stereo_rectify_input → stereo::RectifyInput。
inline stereo::RectifyInput to_rectify_input(const stereo_rectify_input& in) {
    stereo::RectifyInput ri;
    ri.wide = to_intrinsics(in.wide);
    ri.ultra = to_intrinsics(in.ultra);
    ri.extrinsic.R = to_mat3(in.R);
    ri.extrinsic.t = stereo::Vec3{in.t[0], in.t[1], in.t[2]};
    ri.src_width = in.src_width;
    ri.work_width = in.work_width;
    ri.work_height = in.work_height;
    ri.flip = in.flip;
    ri.dy = in.dy;
    return ri;
}

/// stereo::BlobRecord → stereo_blob（字段同名同序；仍然逐字段拷，不靠"恰好对齐"）。
inline void copy_blobs(const stereo::BlobRecord* src, int count, stereo_blob* dst) {
    if (src == nullptr || dst == nullptr) return;
    for (int i = 0; i < count; ++i) {
        const stereo::BlobRecord& r = src[i];
        dst[i].identifier = r.identifier;
        dst[i].cells = r.cells;
        dst[i].distance = r.distance;
        dst[i].x = r.x;
        dst[i].width = r.width;
        for (int k = 0; k < 4; ++k) dst[i].rect[k] = r.rect[k];
    }
}

}  // namespace capi
