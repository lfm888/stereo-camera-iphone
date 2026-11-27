# `stereo_cli` — 第二个前端（只吃公开 C 头）

存在理由：`stereo_process_frame` 原来只有测试在用，iOS 又不走它，
"跨平台库"就没人从外面验证过。这个 CLI 是第一个真实使用者：

* **只 `#include "stereo/stereo_c_api.h"`**（或领域子头），不碰任何 `.hpp`；
* **零第三方依赖**：图像用 PNM/PFM，标定用 `key = value` 文本；
* 同一份 `core/`，Windows / Linux / macOS 都能编。

## 用法

```bash
# 仓库根（core + CLI 一起构建）
cmake -S . -B build/all -DCMAKE_BUILD_TYPE=Release
cmake --build build/all --config Release --parallel --target stereo_cli

./build/all/apps/cli/stereo_cli \
  --left  apps/cli/testdata/left.pgm \
  --right apps/cli/testdata/right.pgm \
  --calib apps/cli/testdata/calib.txt \
  --depth out/depth.pfm --disp out/disp.pfm \
  --labels out/labels.pgm --ply out/cloud.ply
```

输出（stdout）：工作分辨率、`dir`、`fB`、矫正后内参、有效深度/视差计数、
各阶段耗时（ms）、写出的文件。失败时打印 `stereo_last_error`。

## 输入格式

| 文件 | 格式 |
|---|---|
| `--left` / `--right` | 二进制 **P5**（8 位灰度）或 **P6**（8 位 RGB），两张同尺寸；宽度必须等于标定的 `src_width` |
| `--calib` | 每行 `key = value`，`#` 之后是注释；键见下 |

标定键（缺省值全部来自**参数表**，这里不各写一份）：

```
fx fy cx cy              宽相机内参（原始图像像素）
ufx ufy ucx ucy          超广角内参（缺省 = 宽相机）
R = r00 .. r08           外参旋转（行主序，X_wide = R * X_ultra + t）
t = tx ty tz             外参平移（毫米）
src_width                原始图宽（必填）
work_width/work_height   工作分辨率（缺省 640×360）
flip / dy                基线方向与行偏移（自动标定初值）
max_disp p1 p2 uniqueness lr_check   匹配参数
min_z max_z              深度范围
```

## 输出格式

* 深度 / 视差：**PFM `Pf`**（float32，行序自下而上，与标准一致）；
* 标签图：**PGM P5**（0 = 未保留）；
* 点云：ASCII **PLY**（用矫正后的 K 反投影，颜色取自左图）。

## 对阶段 4（API 约定统一）的输入

写这个前端时实际被卡到的地方，按严重程度排：

1. **stride 单位不一致**：图像是**字节**，深度/视差是**像素**；字段名没有单位后缀，
   第一次接的人一定会写错一次。建议 `*_stride_bytes` / `*_stride_px`。
2. **失败只有一个 `-1`**：要拿原因必须持有 `ctx` 再调 `stereo_last_error`；
   无状态函数（矫正/导出）连错误串都没有。建议 `stereo_status` + `stereo_status_string`。
3. **"有效样本不足"用返回码 1 表示**（`stereo_sample_depth`），和"参数非法 -1"混在
   同一个返回值里。建议把它做成输出参数（例如 `has_value`），错误码只表示失败。
4. **`in.config == NULL` 会把 ctx 配置重置成默认值**（文档里写了，但仍然反直觉；
   第一次跑 CLI 时不传 config 就踩到）。建议"NULL = 保持当前配置"。
5. **`stereo_frame_output` 的缓冲全要调用方按 `work_width × height` 预分配**，
   没有"先问需要多大"的入口（点云有 `_capacity`，深度没有）。
6. **内参必须自己从标定文件搬进 `stereo_rectify_input`**（9 + 3 个数），
   没有 `stereo_rectify_input_from_*` 这类构造入口；Qt/CLI 前端都要各抄一遍。
7. `stereo_config.threads` 字段存在但实现恒为单线程——建议删掉或明确标注保留。
