// apps/cli/calib.hpp — 标定文件（key = value）读取
//
// 只依赖公开 C 头（stereo_c_api.h）与标准库。默认值全部来自**参数表**
// （stereo_param_find + STEREO_PARAM_*），CLI 里不各写一份默认值。
//
// 文件格式（每行一条，# 之后是注释）：
//   fx / fy / cx / cy         宽（主）相机内参，单位：原始图像像素
//   ufx / ufy / ucx / ucy     超广角相机内参（缺省 = 与宽相机相同）
//   R = r00 .. r08            外参旋转，行主序：X_wide = R * X_ultra + t
//   t = tx ty tz              外参平移，单位：毫米
//   src_width                 原始图宽（内参就是按它给的）
//   work_width / work_height  工作分辨率（缺省 640 / 360）
//   flip / dy                 基线方向与行偏移（自动标定的初值）
//   max_disp / p1 / p2 / uniqueness / lr_check   匹配参数（缺省取参数表）
//   min_z / max_z             深度范围（缺省取参数表）

#pragma once

#include <string>

namespace cli {

struct Calibration {
    float fx = 0, fy = 0, cx = 0, cy = 0;
    float ufx = 0, ufy = 0, ucx = 0, ucy = 0;
    float R[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    float t[3] = {0, 0, 0};
    int src_width = 0;
    int work_width = 640;
    int work_height = 360;
    int flip = 0;
    float dy = 0.0f;
    int max_disp = 0;
    int p1 = 0;
    int p2 = 0;
    float uniqueness = 0.0f;
    int lr_check = 1;
    float min_z = 0.0f;
    float max_z = 0.0f;
};

/// 读标定文件；缺省值先按参数表填好，再被文件覆盖。失败时 err 带原因。
bool load_calibration(const std::string& path, Calibration& out, std::string& err);

}  // namespace cli
