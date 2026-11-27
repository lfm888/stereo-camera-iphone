// frame_test.cpp — 第 2..5 步 CPU 全流水线 + 参数描述表的自检
//
// 两块东西的验证思路不一样：
//
//   1) stereo_process_frame：它不是新算法，是**组合**。所以判据是"把同样的 6 个
//      公开函数按同样顺序手工调一遍，结果必须逐位相同"。这样以后谁改了组合顺序、
//      或者在中间动了缓冲区，都会当场暴露。
//
//   2) 参数表：它是"唯一真源"，所以要验证
//      · 表与 stereo_config_default 的默认值一致（证明默认值没有第二份）
//      · key 唯一、min<=default<=max、step>0、ENUM 的标签条数与取值个数一致
//      · 通过 C API 能拿到表（Qt 以后就靠它生成滑杆）

#include "stereo/stereo_c_api.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_fail = 0;

void report(const char* name, bool ok, const char* detail = "") {
    std::printf("  %-46s %s%s%s\n", name, ok ? "OK  " : "FAIL", detail[0] ? "  " : "", detail);
    if (!ok) ++g_fail;
}

constexpr int SRC_W = 320;   // 原始图尺寸
constexpr int SRC_H = 240;
constexpr int WORK_W = 320;  // 工作分辨率（与原始相同 → 矫正单应接近恒等，便于构造）
constexpr int WORK_H = 240;
constexpr int DISP = 5;      // 注入的水平位移（右图内容左移这么多）

/// 逐像素伪随机纹理：横向唯一，Census 才有意义。
unsigned char pattern(int x, int y) {
    unsigned h = static_cast<unsigned>(x / 2) * 73856093u ^ static_cast<unsigned>(y / 2) * 19349663u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return static_cast<unsigned char>(h & 0xFF);
}

/// 原始帧：内容 = pattern(x - sx, y - sy)。
struct Frame {
    std::vector<unsigned char> px;
    int stride = SRC_W * 4;
    Frame(int sx, int sy) : px(static_cast<size_t>(SRC_W) * SRC_H * 4, 0) {
        for (int y = 0; y < SRC_H; ++y) {
            for (int x = 0; x < SRC_W; ++x) {
                const int sxp = x - sx;
                const int syp = y - sy;
                const unsigned char v =
                    (sxp >= 0 && sxp < SRC_W && syp >= 0 && syp < SRC_H) ? pattern(sxp, syp) : 0;
                unsigned char* p = &px[(static_cast<size_t>(y) * SRC_W + x) * 4];
                p[0] = v;
                p[1] = v;
                p[2] = v;
                p[3] = 255;
            }
        }
    }
    stereo_image view() const {
        stereo_image img{};
        img.data = px.data();
        img.width = SRC_W;
        img.height = SRC_H;
        img.stride = stride;
        img.format = STEREO_FORMAT_BGRA8;
        return img;
    }
};

stereo_rectify_input make_geometry() {
    stereo_rectify_input g{};
    g.wide.fx = 200.0f;
    g.wide.fy = 200.0f;
    g.wide.cx = 160.0f;
    g.wide.cy = 120.0f;
    g.ultra = g.wide;
    g.R[0] = g.R[4] = g.R[8] = 1.0f;
    g.t[0] = 65.0f;
    g.src_width = SRC_W;
    g.work_width = WORK_W;
    g.work_height = WORK_H;
    g.flip = 0;
    g.dy = 0.0f;
    return g;
}

}  // namespace

int main() {
    const int W = WORK_W, H = WORK_H;
    const size_t n = static_cast<size_t>(W) * H;

    const Frame left(0, 0);
    const Frame right(-DISP, 0);   // 同名点在右图左侧 → dir = +1
    const stereo_image li = left.view();
    const stereo_image ri = right.view();
    const stereo_rectify_input geom = make_geometry();

    stereo_config cfg;
    stereo_config_default(&cfg);

    // ---------------- 1) 参数描述表 ----------------
    {
        const int count = stereo_param_count();
        char buf[128];
        std::snprintf(buf, sizeof buf, "%d 条", count);
        report("stereo_param_count > 0", count > 0, buf);

        const stereo_param_desc* table = stereo_param_table();
        report("stereo_param_table 非空", table != nullptr);

        int bad_range = 0, bad_step = 0, bad_key = 0, bad_enum = 0;
        std::vector<std::string> keys;
        for (int i = 0; i < count; ++i) {
            const stereo_param_desc& d = table[i];
            if (d.key == nullptr || d.key[0] == '\0' || d.label == nullptr || d.group == nullptr ||
                d.unit == nullptr) {
                ++bad_key;
                continue;
            }
            keys.emplace_back(d.key);
            if (!(d.min_value <= d.max_value)) ++bad_range;
            if (!(d.default_value >= d.min_value && d.default_value <= d.max_value)) ++bad_range;
            if (!(d.step > 0.0f)) ++bad_step;
            if (d.kind == STEREO_PARAM_ENUM) {
                // 标签条数必须等于可选值个数
                int values = static_cast<int>((d.max_value - d.min_value) / d.step) + 1;
                int labels = 1;
                if (d.enum_labels == nullptr) {
                    ++bad_enum;
                } else {
                    for (const char* p = d.enum_labels; *p; ++p) {
                        if (*p == '|') ++labels;
                    }
                    if (labels != values) ++bad_enum;
                }
            } else if (d.enum_labels != nullptr) {
                ++bad_enum;
            }
        }
        report("每条都有 key/label/group/unit", bad_key == 0);
        report("min <= default <= max", bad_range == 0);
        report("step > 0", bad_step == 0);
        report("ENUM 的标签条数与取值个数一致", bad_enum == 0);

        int dup = 0;
        for (size_t i = 0; i < keys.size(); ++i) {
            for (size_t j = i + 1; j < keys.size(); ++j) {
                if (keys[i] == keys[j]) ++dup;
            }
        }
        report("key 不重复", dup == 0);

        report("stereo_param_find 能按 id 找到", stereo_param_find(STEREO_PARAM_MAX_DISP) != nullptr);
        report("stereo_param_find 对非法 id 返回 NULL",
               stereo_param_find(-1) == nullptr && stereo_param_find(STEREO_PARAM_COUNT) == nullptr);
    }

    // ---------------- 2) 默认值只有一份：表 vs stereo_config_default ----------------
    {
        const stereo_param_desc* d_disp = stereo_param_find(STEREO_PARAM_MAX_DISP);
        const stereo_param_desc* d_p1 = stereo_param_find(STEREO_PARAM_P1);
        const stereo_param_desc* d_p2 = stereo_param_find(STEREO_PARAM_P2);
        const stereo_param_desc* d_uniq = stereo_param_find(STEREO_PARAM_UNIQUENESS);
        const stereo_param_desc* d_lr = stereo_param_find(STEREO_PARAM_LR_CHECK);
        const bool ok = d_disp && d_p1 && d_p2 && d_uniq && d_lr &&
                        cfg.max_disp == static_cast<int>(d_disp->default_value) &&
                        cfg.p1 == static_cast<int>(d_p1->default_value) &&
                        cfg.p2 == static_cast<int>(d_p2->default_value) &&
                        cfg.uniqueness == d_uniq->default_value &&
                        cfg.lr_check == static_cast<int>(d_lr->default_value);
        char buf[128];
        std::snprintf(buf, sizeof buf, "D=%d p1=%d p2=%d u=%.2f lr=%d", cfg.max_disp, cfg.p1, cfg.p2,
                      static_cast<double>(cfg.uniqueness), cfg.lr_check);
        report("stereo_config_default 与参数表一致", ok, buf);
    }

    // ---------------- 3) stereo_process_frame ----------------
    stereo_context* ctx = stereo_create();
    report("stereo_create", ctx != nullptr);
    if (ctx == nullptr) return 1;

    std::vector<float> disp_pf(n, 0.0f), depth_pf(n, 0.0f);
    std::vector<unsigned char> labels_pf(n, 0);
    std::vector<stereo_blob> blobs_pf(24);
    stereo_frame_input in{};
    in.left = &li;
    in.right = &ri;
    in.geometry = &geom;
    in.config = &cfg;
    in.min_z = 0.1f;
    in.max_z = 10.0f;
    in.analysis.fx = 0;   // 会被矫正后的 K 覆盖
    in.analysis.max_blobs = 24;
    in.analysis.min_cells = 40;
    static const float center[4] = {0.475f, 0.455f, 0.525f, 0.545f};
    in.center_rect = center;
    in.center_q = 0.5f;

    stereo_frame_output out{};
    out.disp = disp_pf.data();
    out.disp_stride = W;
    out.depth = depth_pf.data();
    out.depth_stride = W;
    out.blobs = blobs_pf.data();
    out.blob_capacity = 24;
    out.labels = labels_pf.data();

    stereo_frame_timings timing{};
    const int rc = stereo_process_frame(ctx, &in, &out, &timing);

    report("stereo_process_frame 成功", rc == 0);
    char buf[160];
    std::snprintf(buf, sizeof buf, "blobs=%d 网格=%dx%d  K=(%.0f,%.0f,%.0f,%.0f)",
                  out.analysis.blob_count, out.analysis.grid_width, out.analysis.grid_height,
                  static_cast<double>(out.k.fx), static_cast<double>(out.k.fy),
                  static_cast<double>(out.k.cx), static_cast<double>(out.k.cy));
    report("输出几何信息已填", out.k.fx > 0 && out.focal_baseline > 0 && out.direction == 1, buf);

    int valid_disp = 0, valid_depth = 0;
    for (size_t i = 0; i < n; ++i) {
        if (disp_pf[i] > 0.0f) ++valid_disp;
        if (depth_pf[i] > 0.0f) ++valid_depth;
    }
    std::snprintf(buf, sizeof buf, "视差 %d/%zu，深度 %d/%zu", valid_disp, n, valid_depth, n);
    report("流水线产出了有效视差与深度", valid_disp > 0 && valid_depth > 0, buf);
    std::snprintf(buf, sizeof buf, "rectify %.1f / match %.1f / depth %.1f / unrect %.1f / total %.1f ms",
                  timing.rectify, timing.match, timing.depth, timing.unrectify, timing.total);
    report("计时非负且 total > 0",
           timing.rectify >= 0 && timing.match >= 0 && timing.depth >= 0 &&
               timing.unrectify >= 0 && timing.analyze >= 0 && timing.total > 0,
           buf);

    // 视差应当集中在注入的 5 附近（几何是恒等，纹理横向唯一）
    double sum = 0;
    int cnt = 0;
    for (size_t i = 0; i < n; ++i) {
        if (disp_pf[i] > 0.0f) {
            sum += disp_pf[i];
            ++cnt;
        }
    }
    const double mean = cnt > 0 ? sum / cnt : -1.0;
    std::snprintf(buf, sizeof buf, "平均视差 %.2f（注入 %d）", mean, DISP);
    report("视差均值接近注入的位移", mean > 3.0 && mean < 7.0, buf);

    // ---------------- 4) 与"手工按顺序调用"逐位一致 ----------------
    {
        stereo_rectify_output g{};
        stereo_rectify_geometry(&geom, &g);

        std::vector<unsigned char> rl(static_cast<size_t>(W) * H * 4), rr(rl.size());
        stereo_image imgl{};
        imgl.data = rl.data();
        imgl.width = W;
        imgl.height = H;
        imgl.stride = W * 4;
        imgl.format = STEREO_FORMAT_BGRA8;
        stereo_image imgr = imgl;
        imgr.data = rr.data();
        const bool ok_rect = stereo_rectify_image(&li, g.to_wide_image, &imgl) == 0 &&
                             stereo_rectify_image(&ri, g.to_ultra_image, &imgr) == 0;

        std::vector<float> disp(n, 0.0f), depth_rect(n, 0.0f), depth(n, 0.0f);
        const bool ok_match = stereo_match(ctx, &imgl, &imgr, g.dir, disp.data(), W) == 0;
        const bool ok_depth = stereo_disparity_to_depth(ctx, disp.data(), W, W, H, g.focal_baseline,
                                                        in.min_z, in.max_z, depth_rect.data(),
                                                        W) == 0;
        const bool ok_unrect = stereo_unrectify_depth(depth_rect.data(), W, W, H, g.to_rect_image,
                                                     depth.data(), W, W, H) == 0;

        stereo_analyze_params ap{};
        ap.fx = g.k.fx;
        ap.fy = g.k.fy;
        ap.cx = g.k.cx;
        ap.cy = g.k.cy;
        ap.max_blobs = 24;
        ap.min_cells = 40;
        std::vector<unsigned char> labels(n, 0);
        std::vector<stereo_blob> blobs(24);
        stereo_analysis analysis{};
        const bool ok_analyze = stereo_analyze(ctx, depth.data(), W, W, H, &ap, center, 0.5f,
                                               blobs.data(), 24, labels.data(), &analysis) == 0;

        const bool ok_all = ok_rect && ok_match && ok_depth && ok_unrect && ok_analyze;
        const bool same_disp = std::memcmp(disp.data(), disp_pf.data(), n * 4) == 0;
        const bool same_depth = std::memcmp(depth.data(), depth_pf.data(), n * 4) == 0;
        const bool same_labels = std::memcmp(labels.data(), labels_pf.data(), n) == 0;
        report("手工 6 步与 process_frame 逐位一致（视差）", ok_all && same_disp);
        report("手工 6 步与 process_frame 逐位一致（深度）", ok_all && same_depth);
        report("手工 6 步与 process_frame 逐位一致（标签图）", ok_all && same_labels);
        report("手工 6 步与 process_frame 一致（面块数/中心读数）",
               ok_all && analysis.blob_count == out.analysis.blob_count &&
                   analysis.has_center == out.analysis.has_center &&
                   std::memcmp(&analysis.center, &out.analysis.center, 4) == 0);
    }

    // ---------------- 5) 确定性：连续跑两遍结果相同（ctx 复用不出错）----------------
    {
        std::vector<float> d2(n, 0.0f);
        stereo_frame_output o2 = out;
        o2.depth = d2.data();
        o2.disp = nullptr;   // 顺便验证"不要视差"的分支
        o2.labels = nullptr;
        o2.blobs = nullptr;
        o2.blob_capacity = 0;
        const int rc2 = stereo_process_frame(ctx, &in, &o2, nullptr);
        report("第二遍（disp/labels/blobs 传 NULL）成功", rc2 == 0);
        report("两遍深度逐位相同", std::memcmp(d2.data(), depth_pf.data(), n * 4) == 0);
        report("第二遍不改几何输出", o2.k.fx == out.k.fx && o2.direction == out.direction);
    }

    // ---------------- 6) 参数校验 ----------------
    {
        stereo_frame_output bad = out;
        report("left 为 NULL -> 拒绝", stereo_process_frame(ctx, nullptr, &bad, nullptr) != 0);
        stereo_frame_input in2 = in;
        in2.right = nullptr;
        report("right 为 NULL -> 拒绝", stereo_process_frame(ctx, &in2, &bad, nullptr) != 0);
        in2 = in;
        in2.geometry = nullptr;
        report("geometry 为 NULL -> 拒绝", stereo_process_frame(ctx, &in2, &bad, nullptr) != 0);
        stereo_frame_output o3 = out;
        o3.depth = nullptr;
        report("depth 缓冲为 NULL -> 拒绝", stereo_process_frame(ctx, &in, &o3, nullptr) != 0);
        in2 = in;
        stereo_rectify_input g2 = geom;
        g2.work_width = 0;
        in2.geometry = &g2;
        report("工作分辨率为 0 -> 拒绝", stereo_process_frame(ctx, &in2, &bad, nullptr) != 0);
        in2 = in;
        in2.config = nullptr;   // NULL = 用默认参数
        report("config 传 NULL 时用默认值", stereo_process_frame(ctx, &in2, &bad, nullptr) == 0 ||
                                               bad.depth != nullptr);
    }

    stereo_destroy(ctx);
    std::printf(g_fail ? "\n有 %d 项不一致\n" : "\n全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
