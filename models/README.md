# models/

放**训练产出的原始 checkpoint**（segzoo 的 `best.pth`），仓库里只维护这一份，导出产物由 CI 生成。

| 文件 | 说明 |
|---|---|
| `lens-seg.pth` | 当前线上模型。segzoo 训练产出：`celebamask_glasses/yolo11_n_256x256/best.pth`，YOLO11n + 语义分割头，输入 256×256，2 类 `background` / `glasses`，ImageNet 归一化（0~1 量纲） |

来源：`E:\lfm\segzoo\runs\celebamask_glasses\yolo11_n_256x256\best.pth`
（mDice 0.9575 / mIoU 0.9185 / 2.35 M 参数，见同目录的 `results.json`）
sha256 `9ce91666b4c74b668fdc2f9f026eb0d7b67cb0e25ac323ea7cdbfaab9f135c39`

## 导出流程

**不要**在 Windows 上导出：Windows 的 coremltools 没有原生库，写不了 `mlpackage`，只能存旧的 neural
network 格式（float32 的 `.mlmodel`），它在 iPhone 上跑不到 GPU / ANE，只能吃 CPU。

正确的做法是把 pth 推到 GitHub，由 `.github/workflows/export-model.yml` 在 Linux + macOS 上导出：

1. `export`（ubuntu-latest）：装 torch 2.2.2 + 固定 commit 的 [segzoo](https://github.com/lfm888/segzoo) + coremltools 9.0，
   跑 `scripts/export_lens_seg.py --format mlpackage` → mlprogram、float16、iOS17+，这正是能被
   Core ML 调度到 Metal / ANE 的格式；同时用 `scripts/verify_lens_seg.py dump` 存一份 PyTorch 参考输出。
2. `verify`（macos-15-intel）：coremltools 的推理运行时只在 macOS 上有。真跑一遍 mlpackage 与 PyTorch 参考对拍
   （max|Δ| / argmax 一致率 / softmax 求和），再用 `coremlcompiler` 编译一次（编译不过 = 手机上起不来）。
3. `commit`：验证通过才把 `StereoVision/Models/lens-seg.mlpackage` 提交回 `main`，并删掉旧的 `.mlmodel`。
   随后 `ipa.yml` 会由 `workflow_run` 触发，出一个已经打进新模型的安装包。

换模型 = 覆盖 `models/lens-seg.pth` 后推送（或手动跑 `Export lens-seg CoreML model` 这个 workflow），
不需要改 App 代码：类名、输入尺寸、归一化参数都写在 CoreML metadata 里。

## 本机临时导出（仅限 macOS / Linux）

```sh
python scripts/export_lens_seg.py --ckpt models/lens-seg.pth --segzoo ../segzoo          # 默认 mlpackage
python scripts/verify_lens_seg.py dump  --ckpt models/lens-seg.pth --out build/ref.npz   # 可选：留参考
python scripts/verify_lens_seg.py check --mlpackage StereoVision/Models/lens-seg.mlpackage --ref build/ref.npz
```
