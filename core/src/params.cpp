// stereo/params.cpp — 参数描述表（**唯一真源**）
//
// 这张表是"有哪些参数、叫什么、范围多少、默认多少、步长多少"的唯一出处：
//   * stereo_config_default() 从这里取默认值（C 侧不再各写一份）
//   * C API 把整张表导出给界面（Swift 现在用的还是硬编码滑杆，Qt 以后直接用表生成）
//   * 新增参数只改这里 + 对应的算法实现
//
// 三条编辑规矩：
//   1. key 必须稳定（界面拿它当标识），一旦发布不要改名，要改名就新增一条
//   2. min <= default <= max，step > 0（有单测保证）
//   3. kind == ENUM 时 enum_labels 用 '|' 分隔，条数必须等于 (max - min) / step + 1

#include "stereo/params.hpp"

#include <cstddef>

namespace stereo {
namespace {

/// 表的顺序就是界面上的顺序。分组：depth → matching → display。
const stereo_param_desc kParams[] = {
    // ---- 深度范围 ----
    {STEREO_PARAM_MIN_Z, "min_z", "Min depth", "depth", STEREO_PARAM_FLOAT,
     0.05f, 1.0f, 0.01f, 0.12f, 0.0f, 0.0f, 0, 0, "m", nullptr},
    {STEREO_PARAM_MAX_Z, "max_z", "Max depth", "depth", STEREO_PARAM_FLOAT,
     0.5f, 8.0f, 0.1f, 3.0f, 0.0f, 0.0f, 0, 0, "m", nullptr},

    // ---- 匹配 ----
    {STEREO_PARAM_MATCHER, "matcher", "Algorithm", "matching", STEREO_PARAM_ENUM,
     0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0, 0, "", "SGM 4 paths|Block matching 5x5"},
    {STEREO_PARAM_MAX_DISP, "max_disparity", "Max disparity", "matching", STEREO_PARAM_INT,
     32.0f, 96.0f, 32.0f, 64.0f, 4.0f, 96.0f, 1, 1, "px", nullptr},
    {STEREO_PARAM_P1, "p1", "P1 (1 px jumps)", "matching", STEREO_PARAM_INT,
     1.0f, 40.0f, 1.0f, 8.0f, 0.0f, 0.0f, 1, 0, "", nullptr},
    {STEREO_PARAM_P2, "p2", "P2 (discontinuities)", "matching", STEREO_PARAM_INT,
     20.0f, 200.0f, 1.0f, 96.0f, 0.0f, 0.0f, 1, 0, "", nullptr},
    {STEREO_PARAM_LR_CHECK, "lr_check", "Left-right check", "matching", STEREO_PARAM_BOOL,
     0.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0, 0, "", nullptr},
    {STEREO_PARAM_UNIQUENESS, "uniqueness", "Uniqueness", "matching", STEREO_PARAM_FLOAT,
     0.0f, 0.5f, 0.01f, 0.1f, 0.0f, 0.0f, 0, 0, "", nullptr},
    {STEREO_PARAM_Y_OFFSET, "y_offset", "Vertical offset", "matching", STEREO_PARAM_FLOAT,
     -12.0f, 12.0f, 0.25f, 0.0f, 0.0f, 0.0f, 0, 0, "px", nullptr},
    {STEREO_PARAM_FLIP, "flip_baseline", "Flip baseline", "matching", STEREO_PARAM_BOOL,
     0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0, 0, "", nullptr},

    // ---- 显示（不参与匹配，只是叠加层/预览的显示参数）----
    {STEREO_PARAM_OVERLAY_ALPHA, "overlay_alpha", "Overlay opacity", "display", STEREO_PARAM_FLOAT,
     0.0f, 1.0f, 0.01f, 0.55f, 0.0f, 0.0f, 0, 0, "", nullptr},
};

static_assert(sizeof(kParams) / sizeof(kParams[0]) == STEREO_PARAM_COUNT,
              "参数表的条数与 stereo_param_id 的 STEREO_PARAM_COUNT 不一致");

}  // namespace

int param_count() { return STEREO_PARAM_COUNT; }

const stereo_param_desc* param_table() { return kParams; }

const stereo_param_desc* param_desc(int id) {
    if (id < 0 || id >= STEREO_PARAM_COUNT) return nullptr;
    // 表的顺序与 id 一致（static_assert 只保证条数，这里逐条核对一次）
    for (int i = 0; i < STEREO_PARAM_COUNT; ++i) {
        if (kParams[i].id == id) return &kParams[i];
    }
    return nullptr;
}

float param_default(int id) {
    const stereo_param_desc* d = param_desc(id);
    return d == nullptr ? 0.0f : d->default_value;
}

void config_default(stereo_config& cfg) {
    // 默认值来自参数描述表，不在这里各写一份
    cfg.max_disp = static_cast<int>(param_default(STEREO_PARAM_MAX_DISP));
    cfg.p1 = static_cast<int>(param_default(STEREO_PARAM_P1));
    cfg.p2 = static_cast<int>(param_default(STEREO_PARAM_P2));
    cfg.uniqueness = param_default(STEREO_PARAM_UNIQUENESS);
    cfg.lr_check = static_cast<int>(param_default(STEREO_PARAM_LR_CHECK));
    cfg.threads = 0;
}

}  // namespace stereo
