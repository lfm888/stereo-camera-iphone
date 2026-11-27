//
//  stereo/stereo_c_api.h — 总头（兼容旧 include 路径）。
//
//  真正的声明按领域分在 9 个子头里，每个都能被纯 C 编译器单独包含：
//    stereo_types.h      版本宏、像素格式、stereo_image、内参
//    stereo_context.h    不透明句柄、stereo_config、生命周期、错误串
//    stereo_match.h      stereo_match、stereo_disparity_to_depth
//    stereo_rectify.h    矫正几何、重采样、深度回转
//    stereo_analyze.h    网格尺寸、准星采样、连通域分析
//    stereo_export.h     点云 / 裁剪 / 毫米打包 / 跨度拷贝
//    stereo_calibrate.h  自动标定（CPU 打分 / 平台打分回调）
//    stereo_params.h     参数描述表
//    stereo_pipeline.h   第 2..5 步 CPU 全流水线
//
//  为什么保留总头：Bridge/*.mm、C 冒烟测试、Swift 桥接头都只 include 这一个路径；
//  拆头不能让别人改代码。新代码建议按领域 include 子头。
//
//  C 契约的稳定面由 tools/check_api_surface.py + core/tests/api_surface.txt 锁定。
//

#ifndef STEREO_C_API_H
#define STEREO_C_API_H

#include "stereo/stereo_types.h"
#include "stereo/stereo_context.h"
#include "stereo/stereo_match.h"
#include "stereo/stereo_rectify.h"
#include "stereo/stereo_analyze.h"
#include "stereo/stereo_export.h"
#include "stereo/stereo_calibrate.h"
#include "stereo/stereo_params.h"
#include "stereo/stereo_pipeline.h"

#endif /* STEREO_C_API_H */
