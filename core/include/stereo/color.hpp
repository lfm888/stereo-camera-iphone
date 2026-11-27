// stereo/color.hpp — 像素格式的搬运：任意 stereo_format → 亮度 / 紧凑 BGRA8
//
// 这两件事原来长在 C 门面里（`stereo_c_api.cpp` 的 `to_luma` / `convert_to_bgra8`），
// 但它们是**行为**（逐像素、有固定系数），不是"翻译"，所以搬到算法层：
//   * C 门面只做参数校验与类型转换，不再出现像素循环；
//   * C++ 调用者（`process_frame`、测试、将来的 CLI）不用经过 C 边界也能用。
//
// 语义与 Shaders.metal 的 census 前处理一致：
//   亮度 = 0.299 R + 0.587 G + 0.114 B，分量先各自 /255；
//   BGRA8 统一成内存序 B,G,R,A（alpha 由格式决定：三通道/灰度补 255，四通道保留）。
//
// 浮点运算顺序与改造前**逐字一致**（逐位对拍依赖它），不要"顺手优化"成点积或先合并系数。

#pragma once

#include <cstdint>

#include "stereo/rectify.hpp"   // ImageView / kFormat*

namespace stereo {

/// 任意格式 → 行跨度 = width 的紧凑亮度（0..1）。格式不认识返回 false。
bool to_luma(const ImageView& src, float* luma);

/// 任意格式 → 行跨度 = width*4 的紧凑 BGRA8。格式不认识返回 false。
bool to_bgra8(const ImageView& src, uint8_t* dst);

}  // namespace stereo
