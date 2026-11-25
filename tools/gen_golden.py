#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_golden.py — 为 C++ 移植生成「金标准测试向量」(.npy)

sgm_ref.py 的 `--dump` 导出的是 8 位 PNG 可视化图，只能看、不能当逐位对拍的
向量。这个脚本把每个阶段的**原始数值**存成 .npy（未压缩、C 顺序、小端），
C++ 侧只要读 128 字节头就能拿到，不需要 zlib。

对着 Shaders.metal 的每个 kernel 产出一份输入和一份期望输出：

    left_u8.npy      (H,W,3) uint8   ← 合成场景左图（= 已矫正的左图）
    right_u8.npy     (H,W,3) uint8   ← 合成场景右图（= 已矫正的右图）
    luma_f32.npy     (H,W)   float32 ← dot(rgb, (0.299,0.587,0.114))
    censusL_u64.npy  (H,W)   uint64  ← census()
    censusR_u64.npy  (H,W)   uint64  ← census()
    cost_u8.npy      (H,W,D) uint8   ← costVolume()
    sum_u16.npy      (H,W,D) uint16  ← 4 条路径聚合后的 sgmPath()
    dispR_f32.npy    (H,W)   float32 ← wtaRight()
    disp_f32.npy     (H,W)   float32 ← wtaLeft()
    depth_f32.npy    (H,W)   float32 ← toDepth()
    meta.json                        ← 参数、形状、dtype、每个文件的 sha256

跑法（不用任何参数，默认出一份很小的、足以覆盖所有分支的向量）：
    python tools/gen_golden.py
    python tools/gen_golden.py --size small     # 320x180, D=32
    python tools/gen_golden.py --size full      # 640x360, D=64（真机工作分辨率）

注意 census 的位序：Metal 把 bit0..31 存进 dst.x、bit32..47 存进 dst.y；
本文件用 uint64，bit k 就是 uint64 的第 k 位，两者在 XOR+popcount 下等价。
"""

import hashlib
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sgm_ref as R  # noqa: E402

# ---------------------------------------------------------------------------
# 所有命令行参数集中在这里，直接运行就能用；命令行同名参数可覆盖
# ---------------------------------------------------------------------------
DEFAULTS = {
    "out": "",            # 空 = core/tests/golden（相对仓库根）
    "size": "tiny",       # tiny | small | full
    "D": 0,               # 0 = 跟随 size 的默认值
    "seed": 0,
    "p1": 8,
    "p2": 96,
    "uniq": 0.1,
    "lr": 1,
    "fB": 0.66,           # px·m，与 sgm_ref.pipeline() 一致
    "minZ": 0.12,
    "maxZ": 3.0,
}

# size → (W, H, D)
SIZES = {
    "tiny": (64, 48, 16),     # 提交进仓库，覆盖边界/首末路径/WTA 分支
    "small": (320, 180, 32),
    "full": (640, 360, 64),   # 真机工作分辨率
}


def save_npy(path: str, arr: np.ndarray) -> dict:
    """保存 .npy 并返回它的描述（形状/dtype/sha256/字节数）。"""
    arr = np.ascontiguousarray(arr)
    np.save(path, arr, allow_pickle=False)
    raw = arr.tobytes()
    return {
        "file": os.path.basename(path),
        "shape": list(arr.shape),
        "dtype": str(arr.dtype),
        "bytes": int(arr.nbytes),
        "sha256": hashlib.sha256(raw).hexdigest(),
    }


def main() -> None:
    import argparse

    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    for k, v in DEFAULTS.items():
        ap.add_argument("--" + k, type=type(v) if v != "" else str, default=v)
    args = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = args.out or os.path.join(root, "core", "tests", "golden")
    w, h, d_default = SIZES[args.size]
    D = args.D or d_default
    os.makedirs(out, exist_ok=True)

    p = R.SGMParams(W=w, H=h, D=D, dir=1, P1=args.p1, P2=args.p2,
                    uniqueness=args.uniq, lrCheck=args.lr)

    # ---- 1. 合成场景（= 已经矫正好的左右图，视差真值已知）------------------
    left, right, truth = R.synthetic_scene(args.seed, w=w, h=h)
    left_u8 = np.clip(np.round(left * 255.0), 0, 255).astype(np.uint8)
    right_u8 = np.clip(np.round(right * 255.0), 0, 255).astype(np.uint8)
    # 反过来用 u8 重建 float，确保 C++ 看到的输入和这里的数值完全一致
    left_f = (left_u8.astype(np.float32) / np.float32(255.0))
    right_f = (right_u8.astype(np.float32) / np.float32(255.0))

    luma_l = R.rgb_to_luma(left_f)
    luma_r = R.rgb_to_luma(right_f)

    # ---- 2. census → costVolume → sgmPath×4 → wtaRight/wtaLeft → toDepth --
    cl = R.census(left_f)
    cr = R.census(right_f)
    C = R.cost_volume(cl, cr, p)
    S = R.sgm_all_paths(C, luma_l, p)
    dispR = R.wta_right(S, p)
    disp = R.wta_left(S, dispR, p)
    depth = R.to_depth(disp, args.fB, args.minZ, args.maxZ)

    # ---- 3. 落盘 -----------------------------------------------------------
    stages = {}
    for name, arr in [
        ("left_u8", left_u8),
        ("right_u8", right_u8),
        ("luma_f32", luma_l),
        ("censusL_u64", cl),
        ("censusR_u64", cr),
        ("cost_u8", C),
        ("sum_u16", S),
        ("truth_f32", truth),
        ("dispR_f32", dispR),
        ("disp_f32", disp),
        ("depth_f32", depth),
    ]:
        stages[name] = save_npy(os.path.join(out, name + ".npy"), arr)

    meta = {
        "generator": "tools/gen_golden.py",
        "sgm_ref": "tools/sgm_ref.py",
        "numpy": np.__version__,
        "size": args.size,
        "seed": args.seed,
        "params": {
            "W": w, "H": h, "D": D, "dir": p.dir,
            "P1": p.P1, "P2": p.P2,
            "uniqueness": p.uniqueness, "lrCheck": p.lrCheck,
            "fB": args.fB, "minZ": args.minZ, "maxZ": args.maxZ,
        },
        "note": (
            "left/right 是合成场景，等价于 rectify 之后的输入。"
            "census 用 uint64：bit k = 第 k 个邻居（dy 外层、dx 内层，跳过中心），"
            "与 Metal 的 bits.x/bits.y 在 XOR+popcount 下等价。"
        ),
        "arrays": stages,
    }
    with open(os.path.join(out, "meta.json"), "w", encoding="utf-8") as f:
        json.dump(meta, f, indent=2, ensure_ascii=False)

    # ---- 4. 报告 -----------------------------------------------------------
    valid = disp > 0
    print(f"已写入 {out}/  ({w}x{h}, D={D})")
    total = 0
    for name, d in stages.items():
        total += d["bytes"] + 128
        print(f"  {name + '.npy':<16} {str(d['shape']):<14} {d['dtype']:<9} {d['bytes']:>9,d} B")
    print(f"  合计约 {total / 1e6:.2f} MB")
    print(f"有效视差 {100.0 * valid.mean():.1f}%  "
          f"深度中位数 {np.median(depth[depth > 0]) if (depth > 0).any() else float('nan'):.2f} m")


if __name__ == "__main__":
    main()
