#!/usr/bin/env python3
"""把 segzoo 训练出来的分割模型（best.pth）导出为 iOS 用的 CoreML 模型。

用法（先 conda activate segzoo）：
    python scripts/export_lens_seg.py \
        --ckpt E:/lfm/segzoo/runs/lens_dataset/yolo11_n_256x256/best.pth --segzoo E:/lfm/segzoo

不带 --ckpt 时自动取 <segzoo>/runs 下修改时间最新的 best.pth。
输出默认 StereoVision/Models/lens-seg.mlpackage（project.yml 的 preBuild 会编译成 .mlmodelc）。

为什么不能直接用 pth：train.py 存的是 {"model": state_dict, "meta": {...}}，没有 pickle 模型对象，
结构靠 meta 里的 arch / encoder / sota / in_channels / num_classes / img_size 描述，必须用 segzoo
的 models.load_checkpoint() 重建，所以这个脚本需要 segzoo 源码在 --segzoo 路径上。
（coremltools 8/9 也不再支持直接转换 ONNX，所以这里从 pth 追踪 PyTorch 图，不读 model.onnx。）

两种输出格式（--format auto 默认）：
  mlpackage  mlprogram，float16，iOS17+，首选；需要 coremltools 的原生库（macOS / Linux 的 wheel）
  mlmodel    neural network，float32，iOS12+；Windows 上 coremltools 只能存这种（纯 Python protobuf）
两者在 App 里走同一条代码路径：编译后都是 lens-seg.mlmodelc。

导出的模型契约（与 inference/preprocess.py 的部署契约一致）：
  输入  image  0~255 像素；模型内先 /255，再 (x - mean) / std（imagenet，0~1 量纲）
  输出  probs  形状 (1, num_classes, H, W)，逐像素 softmax 概率，argmax 后 0 = background
"""
import argparse
import importlib.util
import os
import sys
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn

REPO = Path(__file__).resolve().parents[1]
DEFAULT_OUT = REPO / "StereoVision" / "Models" / "lens-seg"


def find_segzoo(cli_value):
    """按 --segzoo > $SEGZOO_DIR > 仓库同级 segzoo 的顺序找 segzoo 根目录。"""
    for c in (cli_value, os.environ.get("SEGZOO_DIR"), REPO.parent / "segzoo"):
        if c and (Path(c) / "models.py").is_file():
            return Path(c).resolve()
    raise SystemExit("找不到 segzoo 源码目录，请用 --segzoo /path/to/segzoo 指定（需要其中的 models.py 与 sota/）")


def find_ckpt(cli_value, segzoo):
    """按 --ckpt > $LENS_CKPT > <segzoo>/runs 下最新的 best.pth 的顺序找权重。"""
    for c in (cli_value, os.environ.get("LENS_CKPT")):
        if c:
            p = Path(c).expanduser()
            if not p.is_file():
                raise SystemExit("权重文件不存在: %s" % p)
            return p.resolve()
    found = sorted((segzoo / "runs").glob("**/best.pth"), key=lambda p: p.stat().st_mtime, reverse=True)
    if not found:
        raise SystemExit("没在 %s 下找到 best.pth，请用 --ckpt 指定" % (segzoo / "runs"))
    return found[0].resolve()


class ExportWrapper(nn.Module):
    """把 /255、归一化与 softmax 全部放进图里，CoreML 只负责把像素交进来。

    CoreML 的 imageType 输入在不设 scale 时给的就是 0~255 的像素值；这里 /255 后再
    (x - mean) / std，与训练 / inference/preprocess.py 完全一致。
    归一化不交给 ct.ImageType(scale=...) 的原因：neural network 后端会丢掉那个 scale
    （导出的 .mlmodel 里没有 scaler 层，像素会以 0~255 直接进网络），写进图里两种格式都正确。
    """

    def __init__(self, model, mean, std):
        super().__init__()
        self.model = model
        self.register_buffer("mean", torch.tensor(mean, dtype=torch.float32).view(1, -1, 1, 1))
        self.register_buffer("std", torch.tensor(std, dtype=torch.float32).view(1, -1, 1, 1))

    def forward(self, x):
        return torch.softmax(self.model((x / 255.0 - self.mean) / self.std), dim=1)


def can_write_mlpackage():
    """mlpackage 的读写依赖 coremltools 的原生库（macOS / Linux 的 wheel 里有，Windows 上没有）。"""
    return all(importlib.util.find_spec("coremltools." + m) is not None
               for m in ("libmilstoragepython", "libmodelpackage"))


def save_mlmodel(ml, out, shape):
    """neural network -> 单个 .mlmodel 文件（纯 protobuf，任何平台都能写）。"""
    spec = ml.get_spec()
    out_type = spec.description.output[0].type.multiArrayType
    if len(out_type.shape) == 0:                 # coremltools 不给 NN 输出填 shape，补上以免运行时歧义
        out_type.shape.extend(shape)
    if not out_type.dataType:
        out_type.dataType = 65568                 # FLOAT32
    out.write_bytes(spec.SerializeToString())


def verify_coreml(ml, wrapper, c, h, w):
    """用 CoreML 真跑一遍并与 PyTorch 对拍。只有 macOS 上的 coremltools 能推理，其它平台自动跳过。"""
    try:
        from PIL import Image
    except ImportError:
        print("[verify] 没装 Pillow，跳过 CoreML 推理对拍（pip install pillow）")
        return
    rng = np.random.RandomState(0)
    px = rng.randint(0, 256, size=(h, w, c), dtype=np.uint8)      # 0~255 像素，RGB
    img = Image.fromarray(px[:, :, 0] if c == 1 else px, mode="L" if c == 1 else "RGB")
    with torch.no_grad():
        ref = wrapper(torch.from_numpy(px.transpose(2, 0, 1)[None].astype(np.float32))).numpy()
    try:
        got = np.asarray(ml.predict({"image": img})["probs"])
    except Exception as e:                                        # 非 macOS：没有 CoreML 运行时
        print("[verify] CoreML 推理不可用（%s），跳过；建议在 macOS 上跑一遍这个脚本" % type(e).__name__)
        return
    got = got.reshape(ref.shape)
    print("[verify] CoreML vs PyTorch：max|Δ| = %.2e，argmax 一致率 = %.4f" % (
        float(np.abs(ref - got).max()), float((ref.argmax(1) == got.argmax(1)).mean())))


def main():
    ap = argparse.ArgumentParser(description="segzoo best.pth -> CoreML（mlpackage / mlmodel）")
    ap.add_argument("--ckpt", default="", help="best.pth 路径；空 = <segzoo>/runs 下最新的")
    ap.add_argument("--segzoo", default="", help="segzoo 仓库根目录；空 = $SEGZOO_DIR 或 ../segzoo")
    ap.add_argument("--out", default=str(DEFAULT_OUT), help="输出路径前缀（不含扩展名）")
    ap.add_argument("--format", choices=["auto", "mlpackage", "mlmodel"], default="auto",
                    help="auto = 优先 mlpackage，存不了就回退 mlmodel")
    ap.add_argument("--skip-verify", action="store_true", help="跳过 trace 前后的数值对拍")
    args = ap.parse_args()

    segzoo = find_segzoo(args.segzoo)
    ckpt = find_ckpt(args.ckpt, segzoo)
    sys.path.insert(0, str(segzoo))

    from models import load_checkpoint                      # noqa: E402  segzoo 的模型重建
    from inference.preprocess import PreprocessSpec         # noqa: E402  归一化契约（0~1 量纲）

    model, meta = load_checkpoint(ckpt, "cpu")
    spec = PreprocessSpec.from_meta(meta)
    c, (h, w) = spec.in_channels, spec.img_size
    names = list(meta.get("class_names") or [])
    if len(names) != meta["num_classes"]:
        names = ["background"] + ["class%d" % i for i in range(1, meta["num_classes"])]
    arch = (meta.get("sota") or {}).get("name") or meta["arch"]
    print("[export] ckpt  %s" % ckpt)
    print("[export] 结构  %s/%s  输入 (1,%d,%d,%d)  类别 %s" % (
        arch, meta["encoder"], c, h, w, names))
    print("[export] 归一化 mean=%s std=%s（先 /255 到 0~1）" % (
        [round(v, 4) for v in spec.mean], [round(v, 4) for v in spec.std]))
    print("[export] 参数量 %.2f M" % (sum(p.numel() for p in model.parameters()) / 1e6))

    wrapper = ExportWrapper(model, spec.mean, spec.std).eval()
    dummy = torch.rand(1, c, h, w) * 255.0          # 0~255 像素，与 CoreML imageType 输入一致
    with torch.no_grad():
        ref = wrapper(dummy)

    traced = torch.jit.trace(wrapper, dummy, check_trace=False).eval()
    with torch.no_grad():
        got = traced(dummy)
    err = (ref - got).abs().max().item()
    print("[export] trace 对拍 max|Δ| = %.2e（应 ~0）" % err)
    if not args.skip_verify:
        if err > 1e-5:
            raise SystemExit("trace 后的输出与原始 PyTorch 不一致，停止导出")
        sums = (float(ref.sum(1).min()), float(ref.sum(1).max()))
        print("[export] softmax 每像素求和范围 [%.6f, %.6f]（应 = 1）" % sums)

    import coremltools as ct

    layout = ct.colorlayout.RGB if c == 3 else ct.colorlayout.GRAYSCALE
    common = dict(
        inputs=[ct.ImageType(name="image", shape=(1, c, h, w), color_layout=layout)],
        outputs=[ct.TensorType(name="probs")],
    )
    prereq = dict(
        classes=",".join(names),                          # Swift 端按逗号切分，0 号必须是 background
        img_size="%dx%d" % (h, w),
        num_classes=str(meta["num_classes"]),
        norm=spec.norm,
        mean=",".join("%.6f" % v for v in spec.mean),
        std=",".join("%.6f" % v for v in spec.std),
        norm_units="0-1",
        color_order="RGB" if c == 3 else "GRAY",
        input_range="0-255 (normalize inside the model)",
        output="softmax probabilities (1,num_classes,H,W)",
        source_ckpt=str(ckpt),
    )
    desc = "segzoo %s/%s · %dx%d · %d classes" % (arch, meta["encoder"], h, w, meta["num_classes"])

    out = Path(args.out).with_suffix("")
    out.parent.mkdir(parents=True, exist_ok=True)
    written = None

    use_package = args.format == "mlpackage" or (args.format == "auto" and can_write_mlpackage())
    if args.format == "mlpackage" and not can_write_mlpackage():
        raise SystemExit("这个平台的 coremltools 没有原生库，存不了 mlpackage；去掉 --format mlpackage 让它回退到 .mlmodel")

    if use_package:
        target = getattr(ct.target, "iOS18", getattr(ct.target, "iOS17", ct.target.iOS16))
        ml = ct.convert(traced, convert_to="mlprogram", minimum_deployment_target=target,
                        compute_precision=ct.precision.FLOAT16, **common)
        ml.short_description = desc
        ml.author = "segzoo"
        ml.user_defined_metadata.update(prereq)
        written = out.with_suffix(".mlpackage")
        ml.save(str(written))
    elif args.format == "auto":
        print("[export] 这个平台的 coremltools 没有原生库，改用 .mlmodel（neural network，float32）；"
              "在 macOS / Linux 上重跑可得到 float16 的 mlpackage")

    if written is None:
        ml = ct.convert(traced, convert_to="neuralnetwork", minimum_deployment_target=ct.target.iOS14, **common)
        ml.short_description = desc
        ml.author = "segzoo"
        ml.user_defined_metadata.update(prereq)
        written = out.with_suffix(".mlmodel")
        save_mlmodel(ml, written, (1, int(meta["num_classes"]), h, w))
        print("[export] 注意：.mlmodel 是旧格式，能用但比 mlpackage 慢一点；建议在 macOS/Linux 上导出 mlpackage")

    other = out.with_suffix(".mlmodel" if written.suffix == ".mlpackage" else ".mlpackage")
    if other.exists():          # 两种格式编译出同名的 .mlmodelc，preBuild 里 mlpackage 优先
        print("[export] 注意：%s 也在，同名时 preBuild 只编译 mlpackage；不需要就自己删掉它" % other.name)

    if not args.skip_verify:
        verify_coreml(ml, wrapper, c, h, w)

    size = sum(p.stat().st_size for p in written.rglob("*") if p.is_file()) if written.is_dir() else written.stat().st_size
    print("[export] 完成 -> %s （%.1f MB）" % (written, size / 1e6))
    print("[export] metadata: classes=%s img_size=%dx%d" % (",".join(names), h, w))
    print("[export] 下一步：xcodebuild 时 project.yml 的 preBuild 会把 Models/ 下的模型编译进 App")


if __name__ == "__main__":
    main()
