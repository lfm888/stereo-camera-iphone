// capi/context.cpp — 句柄生命周期、配置、错误串
//
// 领域：stereo_context.h。这里只做 C ↔ C++ 的换乘；默认值来自参数表
// （stereo::config_default），缓冲区的失效交给 Workspace。

#include "capi_internal.hpp"

#include <new>

extern "C" {

stereo_context *stereo_create(void) {
    stereo_context *ctx = new (std::nothrow) stereo_context();
    if (ctx != nullptr) stereo_config_default(&ctx->cfg);
    return ctx;
}

void stereo_destroy(stereo_context *ctx) { delete ctx; }

void stereo_config_default(stereo_config *cfg) {
    if (cfg == nullptr) return;
    // 默认值来自参数描述表（core/src/params.cpp），不在这里各写一份
    stereo::config_default(*cfg);
}

int stereo_configure(stereo_context *ctx, const stereo_config *cfg) {
    if (ctx == nullptr) return -1;
    if (cfg == nullptr) return capi::fail(ctx, "stereo_configure: cfg is NULL");
    if (cfg->max_disp < 4 || cfg->max_disp > 96) {
        return capi::fail(ctx, "stereo_configure: max_disp must be within 4..96");
    }
    if (cfg->p1 < 0 || cfg->p2 < 0) return capi::fail(ctx, "stereo_configure: p1/p2 must be >= 0");
    ctx->cfg = *cfg;
    // 参数变了，下一帧重新按新 D 准备缓冲区（等价于改造前把 W/H/D 清 0）
    ctx->ws.match.invalidate();
    return 0;
}

const char *stereo_last_error(const stereo_context *ctx) {
    if (ctx == nullptr) return "stereo_last_error: ctx is NULL";
    return ctx->err;
}

} /* extern "C" */
