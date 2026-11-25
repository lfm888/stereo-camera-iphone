/*
 * c_api_smoke.c — 用**纯 C 编译器**（不是 C++）验证 stereo.h 的接口能直接用。
 *
 * 这是三层架构的第一条互操作约束：C 头必须能被 C 编译、被 Swift 桥接导入、
 * 被 Objective-C++ 包含，而不用任何 C++ 语法。
 *
 * 不做算法对拍（那是 golden_test 的事），只验证：
 *   - 头文件在 C 里能编译
 *   - create / configure / match / to_depth / destroy 的生命周期正常
 *   - 尺寸检查、NULL 检查、错误字符串会正常返回
 */

#include "stereo/stereo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 64
#define H 48

static int failures = 0;

static void check(const char *name, int ok, const char *detail) {
    printf("  %-28s %s%s%s\n", name, ok ? "OK  " : "FAIL",
           (detail && detail[0]) ? "  " : "", detail ? detail : "");
    if (!ok) ++failures;
}

int main(void) {
    printf("C API 冒烟测试（纯 C 编译）  版本 %d\n", STEREO_API_VERSION);

    stereo_config cfg;
    stereo_config_default(&cfg);
    check("stereo_config_default", cfg.max_disp == 64 && cfg.lr_check == 1, "");
    cfg.max_disp = 0; /* 非法值 */

    stereo_context *ctx = stereo_create();
    check("stereo_create", ctx != NULL, "");
    if (!ctx) return 1;

    check("stereo_configure 拒绝非法 max_disp", stereo_configure(ctx, &cfg) != 0,
          stereo_last_error(ctx));

    stereo_config_default(&cfg);
    cfg.max_disp = 32;
    check("stereo_configure 接受 32", stereo_configure(ctx, &cfg) == 0, "");

    /* 造一对假图：右图 = 左图整体右移 3 像素 */
    const int stride = W * 3;
    unsigned char *l = (unsigned char *)calloc((size_t)H * stride, 1);
    unsigned char *r = (unsigned char *)calloc((size_t)H * stride, 1);
    float *disp = (float *)calloc((size_t)W * H, sizeof(float));
    float *depth = (float *)calloc((size_t)W * H, sizeof(float));
    if (!l || !r || !disp || !depth) return 1;

    unsigned seed = 12345u;
    for (int i = 0; i < W * H; ++i) {
        seed = seed * 1664525u + 1013904223u;
        const unsigned char v = (unsigned char)(seed >> 24);
        l[i * 3 + 0] = v; l[i * 3 + 1] = v; l[i * 3 + 2] = v;
    }
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const int sx = x - 3 < 0 ? 0 : x - 3;
            memcpy(&r[(y * W + x) * 3], &l[(y * W + sx) * 3], 3);
        }
    }

    stereo_image li = {l, W, H, stride, STEREO_FORMAT_RGB8};
    stereo_image ri = {r, W, H, stride, STEREO_FORMAT_RGB8};

    check("stereo_match 拒绝 NULL", stereo_match(ctx, NULL, &ri, 1, disp, 0) != 0, "");
    check("stereo_match 成功", stereo_match(ctx, &li, &ri, 1, disp, W) == 0,
          stereo_last_error(ctx));

    int valid = 0;
    for (int i = 0; i < W * H; ++i) if (disp[i] > 0.0f) ++valid;
    char buf[96];
    snprintf(buf, sizeof(buf), "有效视差 %d/%d", valid, W * H);
    check("stereo_match 有有效输出", valid > 0, buf);

    check("to_depth 成功",
          stereo_disparity_to_depth(ctx, disp, W, W, H, 0.66f, 0.12f, 3.0f, depth, W) == 0, "");
    int dvalid = 0;
    for (int i = 0; i < W * H; ++i) if (depth[i] > 0.0f) ++dvalid;
    snprintf(buf, sizeof(buf), "有效深度 %d/%d", dvalid, W * H);
    check("to_depth 有有效输出", dvalid > 0, buf);

    /* 再跑一次：缓冲区复用，不应该崩 */
    check("二次调用（缓冲区复用）", stereo_match(ctx, &li, &ri, 1, disp, W) == 0, "");

    stereo_destroy(ctx);
    check("stereo_destroy(NULL) 安全", (stereo_destroy(NULL), 1), "");

    free(l); free(r); free(disp); free(depth);
    printf(failures ? "\n有 %d 项失败\n" : "\n全部通过\n", failures);
    return failures ? 1 : 0;
}
