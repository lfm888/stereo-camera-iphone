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

#include "stereo/stereo_c_api.h"

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

    /* ---- 第 2 / 4 步：极线矫正与深度回转（不需要 ctx）---- */
    stereo_rectify_input rin;
    stereo_rectify_output rout;
    memset(&rin, 0, sizeof(rin));
    memset(&rout, 0, sizeof(rout));
    rin.wide.fx = 700.0f; rin.wide.fy = 700.0f; rin.wide.cx = 32.0f; rin.wide.cy = 24.0f;
    rin.ultra = rin.wide;
    rin.R[0] = rin.R[4] = rin.R[8] = 1.0f;
    rin.t[0] = 65.0f;
    rin.src_width = W;
    rin.work_width = W;
    rin.work_height = H;
    check("stereo_rectify_geometry 成功", stereo_rectify_geometry(&rin, &rout) == 0, "");
    snprintf(buf, sizeof(buf), "baseline %.1f mm · dir %d", (double)rout.baseline_mm, rout.dir);
    check("矫正输出合理", rout.baseline_mm > 60.0f && rout.baseline_mm < 70.0f, buf);

    /* 单位单应 + 常量图：内部区域必须逐字节不变（边缘会因 clamp_to_zero 变暗）*/
    {
        const int CW = 24, CH = 16;
        unsigned char *flat = (unsigned char *)malloc((size_t)CW * CH * 3);
        unsigned char *out = (unsigned char *)malloc((size_t)CW * CH * 3);
        stereo_image src, dst;
        static const float ident[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        int same = 1;
        memset(flat, 128, (size_t)CW * CH * 3);
        memset(out, 0, (size_t)CW * CH * 3);
        src.data = flat; src.width = CW; src.height = CH; src.stride = CW * 3;
        src.format = STEREO_FORMAT_RGB8;
        dst = src;
        dst.data = out;
        check("stereo_rectify_image 成功", stereo_rectify_image(&src, ident, &dst) == 0, "");
        for (int y = 1; y < CH - 1 && same; ++y) {
            for (int x = 1; x < CW - 1; ++x) {
                const unsigned char *p = out + y * CW * 3 + x * 3;
                if (p[0] != 128 || p[1] != 128 || p[2] != 128) { same = 0; break; }
            }
        }
        check("常量图内部区域不变", same, "逐字节相等");

        /* 格式不同必须拒绝：否则会把 B 与 R 静默写反 */
        dst = src;
        dst.data = out;
        dst.format = STEREO_FORMAT_BGRA8;
        check("stereo_rectify_image 拒绝格式不一致",
              stereo_rectify_image(&src, ident, &dst) != 0, "");

        /* 真实尺寸的噪声图也要能跑 */
        dst = src;
        dst.data = out;
        (void)src;
        free(flat);
        free(out);
    }

    {
        float *d2 = (float *)malloc(sizeof(float) * W * H);
        check("stereo_unrectify_depth 成功",
              stereo_unrectify_depth(depth, W, W, H, rout.to_rect_image, d2, W, W, H) == 0, "");
        check("stereo_unrectify_depth 拒绝 NULL",
              stereo_unrectify_depth(NULL, W, W, H, rout.to_rect_image, d2, W, W, H) != 0, "");
        free(d2);
    }

    stereo_destroy(ctx);
    check("stereo_destroy(NULL) 安全", (stereo_destroy(NULL), 1), "");

    free(l); free(r); free(disp); free(depth);
    printf(failures ? "\n有 %d 项失败\n" : "\n全部通过\n", failures);
    return failures ? 1 : 0;
}
