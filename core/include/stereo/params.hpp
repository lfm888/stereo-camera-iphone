// stereo/params.hpp — 参数描述表的内部访问器
//
// 表的**数据**在 params.cpp 里（一张静态数组），POD 形状由 stereo_c_api.h 定义。
// C++ 侧（stereo_config_default、stereo_configure）与 C API（导出给 UI）都从这里取，
// 这样"默认值"只有一份。
//
// 约定：表里的 min/max/step 是**给界面生成滑杆用的建议范围**，不一定等于库能接受的
// 硬范围（例如库接受 max_disp 4..96，而 App 的滑杆只给 32/64/96 三档）。

#pragma once

#include "stereo/stereo_c_api.h"

namespace stereo {

/// 参数条目数（= STEREO_PARAM_COUNT）。
int param_count();

/// 静态数组，进程生命周期内有效。不要释放、不要修改。
const stereo_param_desc* param_table();

/// 按 id 取默认值；id 非法返回 0。
float param_default(int id);

/// 按 id 取描述；id 非法返回 nullptr。
const stereo_param_desc* param_desc(int id);

}  // namespace stereo
