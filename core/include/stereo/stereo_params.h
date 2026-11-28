//
//  stereo/stereo_params.h — 参数描述表（界面用）。
//  
//  这是"有哪些参数、范围、默认值、步长"的**唯一真源**：
//  `stereo_config_default()` 的默认值也来自这张表。界面按表生成控件，不要各写一份。
//  
//  注意：表里的 min/max 是**建议给界面的范围**，不一定等于库能接受的硬范围。
//

#ifndef STEREO_PARAMS_H
#define STEREO_PARAMS_H

#include "stereo/stereo_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 参数描述表（界面用）                                                  */
/*                                                                     */
/* 这是"有哪些参数、范围、默认值、步长"的**唯一真源**：stereo_config_default() */
/* 的默认值也来自这张表。Swift / Qt 界面应该按表生成滑杆，不要各写一份。      */
/*                                                                     */
/* 2.00 起同时给出**库硬范围**（hard_*）：界面用建议范围（min_value/max_value）  */
/* 做滑杆，用硬范围做越界校验。hard_*_enforced == 0 表示这一侧没有单独校验     */
/* （例如库接受 max_disp 4..96，而 App 的滑杆只给 32/64/96 三档）。            */
/* ------------------------------------------------------------------ */

/** 参数 id。已发布的顺序不要改，新参数往末尾加。 */
enum stereo_param_id {
    STEREO_PARAM_MIN_Z = 0,      /**< 深度下限（米） */
    STEREO_PARAM_MAX_Z,          /**< 深度上限（米） */
    STEREO_PARAM_MATCHER,        /**< 0 = SGM 4 paths，1 = 块匹配 */
    STEREO_PARAM_MAX_DISP,       /**< 最大视差（像素） */
    STEREO_PARAM_P1,             /**< 1 像素小台阶罚项 */
    STEREO_PARAM_P2,             /**< 深度跳变罚项 */
    STEREO_PARAM_LR_CHECK,       /**< 左右一致性检查 */
    STEREO_PARAM_UNIQUENESS,     /**< 唯一性阈值 */
    STEREO_PARAM_Y_OFFSET,       /**< 矫正行偏移（像素） */
    STEREO_PARAM_FLIP,           /**< 交换基线方向 */
    STEREO_PARAM_OVERLAY_ALPHA,  /**< 叠加层不透明度（纯显示） */
    STEREO_PARAM_COUNT
};

/** 参数类型。 */
enum stereo_param_kind {
    STEREO_PARAM_FLOAT = 0,
    STEREO_PARAM_INT = 1,
    STEREO_PARAM_BOOL = 2,
    STEREO_PARAM_ENUM = 3
};

/** 一条参数描述。字符串都是静态字面量，进程生命周期内有效。 */
typedef struct stereo_param_desc {
    int id;                 /**< enum stereo_param_id */
    const char *key;        /**< 稳定标识，例如 "max_disparity" */
    const char *label;      /**< 显示名，例如 "Max disparity" */
    const char *group;      /**< 分组："depth" / "matching" / "display" */
    int kind;               /**< enum stereo_param_kind */
    float min_value;          /**< 建议范围（界面滑杆） */
    float max_value;
    float step;
    float default_value;
    float hard_min;           /**< 库硬下限（hard_min_enforced != 0 时有效） */
    float hard_max;           /**< 库硬上限（hard_max_enforced != 0 时有效） */
    int hard_min_enforced;
    int hard_max_enforced;
    const char *unit;       /**< "m" / "px" / ""（可为空串，不会是 NULL） */
    const char *enum_labels;/**< kind==ENUM 时用 '|' 分隔；否则 NULL */
} stereo_param_desc;

/** 表的条目数。 */
int stereo_param_count(void);

/** 表的首地址（只读，进程生命周期内有效）。条目按界面上希望出现的顺序排列。 */
const stereo_param_desc *stereo_param_table(void);

/** 按 id 取描述；id 非法返回 NULL。 */
const stereo_param_desc *stereo_param_find(int id);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* STEREO_PARAMS_H */
