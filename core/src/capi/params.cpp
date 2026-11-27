// capi/params.cpp — 参数描述表（无状态）
//
// 领域：stereo_params.h。表的**数据**在 core/src/params.cpp，这里只是三个转发。

#include "capi_internal.hpp"

extern "C" {

int stereo_param_count(void) { return stereo::param_count(); }

const stereo_param_desc *stereo_param_table(void) { return stereo::param_table(); }

const stereo_param_desc *stereo_param_find(int id) { return stereo::param_desc(id); }

} /* extern "C" */
