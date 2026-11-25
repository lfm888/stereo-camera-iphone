# models/

放**训练产出的原始 checkpoint**（segzoo 的 `best.pth`）。仓库里只维护这一份，CoreML 与 IPA 由 CI 生成，
不往仓库回写模型文件。

| 文件 | 说明 |
|---|---|
| `lens-seg.pth` | 当前线上模型。segzoo 训练产出：`celebamask_glasses/yolo11_n_256x256/best.pth`，YOLO11n + 语义分割头，输入 256×256，2 类 `background` / `glasses`，ImageNet 归一化（0~1 量纲） |

来源：`E:\lfm\segzoo\runs\celebamask_glasses\yolo11_n_256x256\best.pth`
（mDice 0.9575 / mIoU 0.9185 / 2.35 M 参数，见同目录的 `results.json`）
sha256 `9ce91666b4c74b668fdc2f9f026eb0d7b67cb0e25ac323ea7cdbfaab9f135c39`

## 导出流程

**不要**在 Windows 上导出：Windows 的 coremltools 没有原生库（`libmilstoragepython` / `libmodelpackage`），
写不了 `mlpackage`，只能存旧的 neural network 格式（float32 的 `.mlmodel`），它在 iPhone 上跑不到 GPU / ANE，
只能吃 CPU。

把 pth 推到 GitHub 即可，`.github/workflows/export-model.yml` 会一条龙出安装包：

1. `export`（ubuntu-latest）：装 torch 2.2.2 + 固定 commit 的 [segzoo](https://github.com/lfm888/segzoo) + coremltools 9.0，
   跑 `scripts/export_lens_seg.py --format mlpackage` → **mlprogram、float16、iOS17+**，这正是能被 Core ML
   调度到 Metal / 神经引擎上的格式；导出前后脚本会自己做一次 trace 对拍（max|Δ| ~0）。
2. `ipa`（macos-15-intel）：把 mlpackage 放进 `StereoVision/Models/`，`xcodebuild` 编译（preBuild 用
   `coremlcompiler` 把它编成 `lens-seg.mlmodelc`），打包成未签名的 `StereoVision-unsigned.ipa` 并上传 artifact。

换模型 = 覆盖 `models/lens-seg.pth` 后推送（或手动跑 `Build IPA with a freshly exported model`），
不需要改 App 代码：类名、输入尺寸、归一化参数都写在 CoreML metadata 里。

流水线里**没有**在 Linux 上验证 pth 的步骤（那要把 torch + segzoo 整套环境再装一遍，白花时间）；
能验证的只有导出脚本自己做的一次 trace 对拍，剩下的交给 `coremlcompiler` 与真机。

## 拿安装包到本地

```sh
./scripts/fetch_ipa.sh          # 最近一次成功的「导出 + 打包」→ build/StereoVision-unsigned.ipa
./scripts/fetch_ipa.sh --all    # 最近一次成功的 ipa.yml（用仓库里已提交的模型打包）
```

脚本用 `$GITHUB_TOKEN`，没有就问本机 git 的 credential helper（能 push 就能下）。

## 本机导出（仅 macOS / Linux，可选）

```sh
python scripts/export_lens_seg.py --ckpt models/lens-seg.pth --segzoo ../segzoo   # 默认 mlpackage
```
