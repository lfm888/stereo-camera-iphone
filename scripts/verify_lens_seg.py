#!/usr/bin/env python3
"""CoreML 导出的跨平台数值对拍：Linux 上存 PyTorch 参考，macOS 上跑真 CoreML 对比。

为什么要拆成两半：Windows 的 coremltools 没有原生库（存不了 mlpackage，也没有推理运行时），
而「能跑 torch 的机器」和「能跑 CoreML 的机器」很难是同一台。GitHub Actions 里正好一个
ubuntu-latest（torch，写 mlpackage）加一个 macOS（coremltools 的推理运行时 + coremlcompiler）。
两边用同一张固定噪声图（numpy seed 0，与 export_lens_seg.py 的 verify_coreml 一致），
于是浮点结果可以直接比。

    # Linux / 任意有 torch 的平台（配合 segzoo 源码）
    python scripts/verify_lens_seg.py dump  --ckpt models/lens-seg.pth --segzoo /tmp/segzoo \
        --out build/ci/lens-seg-ref.npz

    # macOS（真 CoreML）
    python scripts/verify_lens_seg.py check \
        --mlpackage StereoVision/Models/lens-seg.mlpackage --ref build/ci/lens-seg-ref.npz

dump 把参考概率存成 float16（256×256×2 只有 256 KB），check 给出 max|Δ|、argmax 一致率，
并核对模型契约（输入尺寸 / 颜色空间 / 输出形状 / metadata 里的类名）。
"""
import argparse
import sys
from pathlib import Path

import numpy as np

SCRIPTS = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPTS))

from export_lens_seg import ExportWrapper, find_ckpt, find_segzoo  # noqa: E402


def noise(h, w, c):
    """与 export_lens_seg.verify_coreml 完全相同的固定输入：seed 0 的 0~255 像素。"""
    return np.random.RandomState(0).randint(0, 256, size=(h, w, c), dtype=np.uint8)


def to_pil(px):
    from PIL import Image
    c = px.shape[2]
    return Image.fromarray(px[:, :, 0] if c == 1 else px, mode="L" if c == 1 else "RGB")


def dump(args):
    """PyTorch 侧：重建模型 -> 跑固定输入 -> 存 px 与 probs。"""
    import torch

    segzoo = find_segzoo(args.segzoo)
    ckpt = find_ckpt(args.ckpt, segzoo)
    sys.path.insert(0, str(segzoo))

    from models import load_checkpoint                      # noqa: E402
    from inference.preprocess import PreprocessSpec         # noqa: E402

    model, meta = load_checkpoint(ckpt, "cpu")
    spec = PreprocessSpec.from_meta(meta)
    c, (h, w) = spec.in_channels, spec.img_size
    wrapper = ExportWrapper(model, spec.mean, spec.std).eval()
    px = noise(h, w, c)
    with torch.no_grad():
        probs = wrapper(torch.from_numpy(px.transpose(2, 0, 1)[None].astype(np.float32))).numpy()

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(out, px=px, probs=probs.astype(np.float16))
    names = list(meta.get("class_names") or [])
    print("[dump] %s" % ckpt)
    print("[dump] probs %s  前景像素 %.4f  可信度均值 %.4f" % (
        tuple(probs.shape), float((probs.argmax(1) != 0).mean()), float(probs.max(1).mean())))
    print("[dump] 类别 %s" % names)
    print("[dump] -> %s (%.0f KB)" % (out, out.stat().st_size / 1024))


def load_coreml(path):
    """载入 mlpackage。跑 check 的机器是 Intel Mac（没有 ANE），默认 computeUnits 可能某些 op 跑不了，
    那就改用 CPU only 重试，保证“模型本身对不对”与“这台机器有没有那个单元”两件事分开看。"""
    import coremltools as ct
    try:
        return ct.models.MLModel(path)
    except Exception as e:
        print("[check] 默认 computeUnits 加载失败（%s: %s），改用 CPU only" % (type(e).__name__, str(e)[:160]))
        return ct.models.MLModel(path, compute_units=ct.ComputeUnit.CPU_ONLY)


class _CpuFallback:
    """装 ml 的薄包装：predict 失败时用 CPU only 重新载入再从同一路径推理。"""

    def __init__(self, path):
        self.path = path
        self.ml = load_coreml(path)

    def predict(self, image):
        import coremltools as ct
        try:
            return np.asarray(self.ml.predict({"image": image})["probs"], dtype=np.float32)
        except Exception as e:
            print("[check] 默认 computeUnits 推理失败（%s: %s），改用 CPU only 重试" % (type(e).__name__, str(e)[:160]))
            self.ml = ct.models.MLModel(self.path, compute_units=ct.ComputeUnit.CPU_ONLY)
            return np.asarray(self.ml.predict({"image": image})["probs"], dtype=np.float32)


def check(args):
    """macOS 侧：真 CoreML 跑一遍，与 PyTorch 参考对拍，并核对模型契约。"""
    ref = np.load(args.ref)
    px, ref_probs = ref["px"], ref["probs"].astype(np.float32)
    h, w, c = px.shape

    net = _CpuFallback(args.mlpackage)                # 非 macOS 上这里会抛（没有 CoreML 运行时）
    ml = net.ml
    desc = ml.modelDescription
    meta = dict(ml.user_defined_metadata)            # App 侧读的就是 creator-defined metadata

    # ---- 契约检查：输入 / 输出 / metadata ----
    img = next(iter(desc.inputDescriptionsByName.values())).imageConstraint
    assert img is not None, "输入不是 imageType（App 侧走 VNImageRequestHandler 的 centerCrop）"
    assert (img.pixelsWide, img.pixelsHigh) == (w, h), \
        "输入尺寸 %dx%d 与 checkpoint 的 %dx%d 不一致" % (img.pixelsWide, img.pixelsHigh, w, h)
    out_desc = desc.outputDescriptionsByName["probs"]
    arr = out_desc.multiArrayType
    shape = tuple(arr.shape) if len(arr.shape) else ()
    assert shape == ref_probs.shape, "输出形状 %s != 参考 %s" % (shape, ref_probs.shape)
    classes = meta.get("classes", "")
    assert classes, "metadata 里没有 classes（App 靠它显示类名）"
    assert len(classes.split(",")) == ref_probs.shape[1], \
        "metadata 的类数 %d 与输出通道 %d 不一致" % (len(classes.split(",")), ref_probs.shape[1])
    print("[check] 输入 %dx%d imageType  输出 probs %s  类别 %s" % (img.pixelsWide, img.pixelsHigh, shape, classes))
    print("[check] metadata: %s" % {k: meta[k] for k in ("img_size", "num_classes", "norm", "mean", "std") if k in meta})

    # ---- 数值对拍 ----
    got = net.predict(to_pil(px)).reshape(ref_probs.shape)
    delta = float(np.abs(got - ref_probs).max())
    agree = float((got.argmax(1) == ref_probs.argmax(1)).mean())
    sums = float(np.abs(got.sum(1) - 1.0).max())
    fg_ref = float((ref_probs.argmax(1) != 0).mean())
    fg_got = float((got.argmax(1) != 0).mean())
    print("[check] max|Δ| = %.2e   argmax 一致率 = %.6f   softmax 求和误差 = %.2e" % (delta, agree, sums))
    print("[check] 前景像素 PyTorch %.4f / CoreML %.4f" % (fg_ref, fg_got))

    fails = []
    if delta > 2e-2:
        fails.append("max|Δ| = %.2e 超过 float16 的合理误差 2e-2" % delta)
    if agree < 0.999:
        fails.append("argmax 一致率 %.6f < 0.999" % agree)
    if sums > 1e-3:
        fails.append("softmax 每像素求和偏差 %.2e > 1e-3（图里的 softmax 被丢了？）" % sums)
    if fails:
        raise SystemExit("[check] 失败：" + "；".join(fails))
    print("[check] 通过：CoreML 与 PyTorch 一致")


def main():
    ap = argparse.ArgumentParser(description="lens-seg 的 PyTorch / CoreML 对拍（dump / check）")
    sub = ap.add_subparsers(dest="cmd", required=True)

    d = sub.add_parser("dump", help="存 PyTorch 参考（需要 torch + segzoo）")
    d.add_argument("--ckpt", default="", help="best.pth 路径；空 = <segzoo>/runs 下最新的")
    d.add_argument("--segzoo", default="", help="segzoo 仓库根目录；空 = $SEGZOO_DIR 或 ../segzoo")
    d.add_argument("--out", required=True, help="输出 .npz")
    d.set_defaults(func=dump)

    c = sub.add_parser("check", help="跑 CoreML 并与参考对拍（需要 macOS）")
    c.add_argument("--mlpackage", required=True, help="导出的 .mlpackage（.mlmodel 也认）")
    c.add_argument("--ref", required=True, help="dump 出来的 .npz")
    c.set_defaults(func=check)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
