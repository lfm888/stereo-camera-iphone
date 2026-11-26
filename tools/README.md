# tools/ — 没有 Mac 也能调这套立体管线

三个离线工具，配合 App 导出的数据，可以在 Windows/Linux 上把「标定 → 矫正 → 匹配 → 显示」整条链路
拆开看、逐像素对照。都不需要 Xcode、Metal 或 iPhone。

| 文件 | 作用 |
|---|---|
| `sgm_ref.py` | `Shaders.metal` 六个 kernel 的 numpy 逐行孪生实现 + 精度基准 + 中间结果导出 |
| `viewer.html` | 单文件查看器，把导出数据按 **iPhone 上的 App 界面**（外框 / 侧边栏 / HUD / 设置页）回放 |
| `make_demo_export.py` | 生成一份假的导出目录，用来在没有手机时试用上面两个工具 |

## 1. `sgm_ref.py` — 算法的孪生实现

```bash
# 自检：向量版 vs Metal 循环的逐字翻译，逐元素对比
uv run --with numpy tools/sgm_ref.py --selftest

# 复现 FAQ.md 的精度表（约 3.5 s）
uv run --with numpy tools/sgm_ref.py

# 导出全部中间结果
uv run --with numpy --with pillow tools/sgm_ref.py --dump out/

# 用真机 Rectified pair 截图跑一遍
uv run --with numpy --with pillow tools/sgm_ref.py --split pair.png --D 96

# 改参数做实验
uv run --with numpy tools/sgm_ref.py --p1 4 --p2 128 --D 32
```

与 FAQ.md 的对照（同一合成场景，纹理随机所以会小幅浮动）：

| 算法 | 本工具 | FAQ.md |
|---|---|---|
| Block 5×5 | 62.3 / 6.4 / 31.4 | 62.2 / 6.5 / 31.2 |
| SGM-4 | 75.8 / 13.6 / 10.6 | 75.2 / 13.9 / 10.8 |
| SGM-4 + LR | 73.0 / 9.0 / 18.0 | 72.4 / 9.3 / 18.3 |

函数与 kernel 的对应关系写在每个函数的 docstring 里（`Shaders.metal:44-68` 等）。
`--dump` 会输出 12 张 PNG，其中 **4 条路径各自的聚合结果**是在真机上拿不到的——
定位「梯子条纹」是哪条路径造成的就靠它。

## 2. `viewer.html` — 手机界面回放

```bash
# 生成演示数据（内含 viewer.html 副本）
uv run --with numpy --with pillow tools/make_demo_export.py demo-export
# 浏览器打开 demo-export/viewer.html，或者双击后把文件拖进去
```

真实数据：文件 App → 我的 iPhone → StereoVision → 时间戳目录，把整个目录拖进窗口。

复刻的 App 界面元素：

| 界面 | 对应源码 |
|---|---|
| 左上角 HUD（模式名 / 深度图 / `B β f` / `Δy matcher D LR`） | `ContentView.swift:96-126` |
| 右侧 62pt 竖排按钮（9 个模式 + 来源徽标 + 检测 + 标定/拍照/录像/设置） | `ContentView.swift:128-172` |
| 设置页（Depth range / Stereo matching / Point cloud / Detection / Source） | `ContentView.swift:174-237` |
| 9 个视图模式的着色 | `Shaders.metal:300-352` |
| 点云（拖动旋转、滚轮缩放、点击复位） | `Shaders.metal:354-375` |
| 3D 分割 / 俯视扫描线 | `Analyzer.swift:87-165` |
| 俯视图 | `OverlayView.swift:66-101` |

能实时调：`Min` / `Max` / `Overlay opacity` / 点云 `Step`、`Point size` / 检测开关。

> **注意**：`test` 分支已删除 `Bird's-eye` 与语义分割模型（见仓库根的 `test.md` §15），
> 所以现在的 App 只有 8 个视图模式、也没有物体框/掩码。`viewer.html` 仍然保留了这两项，
> 用来回放**旧导出**（历史 `meta.json` 里的 `objects` 与 `scan`）；新导出的 `objects` 是空数组。
只读（来自 `meta.json`，导出那刻就固化了）：算法、最大视差、P1/P2、Δy、flip、基线、内参。

**两个实现要点：**

1. `depth_mm.png` 是 16 位灰度 PNG，浏览器的 `<img>`/`<canvas>` 只给 8 位（毫米变 256 级台阶）。
   查看器自带一个最小 PNG 解码器（`DecompressionStream` 解 zlib + 5 种反滤波），把毫米原样读出来。
2. 「对比 view.png」把 App 真实画面半透明叠上来，可以直接看渲染是否与真机一致（逐像素对差）。

## 3. 典型工作流

```
真机拍一张 → 下载导出目录 → viewer.html 里看是哪一步不对
                              ├─ Rectified pair 绿线上特征错位 → 标定/矫正问题（看 dy、flip）
                              ├─ Depth map 大面积黑 → 匹配问题 → sgm_ref.py 改 P1/P2/D 验证
                              └─ 「对比 view.png」有偏差 → 是查看器/显示环节的问题，不是算法
```

改算法就改 `sgm_ref.py`（秒级迭代），确认有效后再同步到 `Shaders.metal`（`sgmPath` 的
向量版与 GPU 的「线程组=扫描线」结构是一一对应的），最后用 `ipa.yml` 云端构建装到真机验证。
