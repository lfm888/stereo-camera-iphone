#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
make_demo_export.py — 生成一份「假的」导出目录，用来在没有 iPhone 的时候
试 tools/viewer.html。

产出（结构与 Capture.swift:169-215 的真实导出完全一致）：
    rgb.png        1280×720 彩色图（真实导出是相机原图）
    depth_mm.png   640×360 16 位灰度，单位毫米，0 = 无效
    meta.json      内参 / 基线 / 物体列表
    view.png       模拟 App 屏幕上的画面（这里画的是 Depth map 视图）
    pair.png       模拟 Rectified pair 截图（左右并排）

跑法：
    uv run --with numpy --with pillow tools/make_demo_export.py out/
    # 然后把 out/viewer.html 和这些文件放在一起，用浏览器打开
"""

import json
import os
import shutil
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sgm_ref as R  # noqa: E402

W, H = R.WORK_W, R.WORK_H
OUT_W, OUT_H = 1280, 720      # rgb.png 的分辨率（深度图是其一半）


def turbo_img(x: np.ndarray) -> np.ndarray:
    return (R.turbo(x) * 255.0 + 0.5).astype(np.uint8)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "demo-export"
    os.makedirs(out, exist_ok=True)

    from PIL import Image

    # ---- 1. 用已验证的孪生管线算视差 / 深度 -------------------------------
    left, right, truth = R.synthetic_scene(0)
    p = R.SGMParams(D=64, P1=8, P2=96, uniqueness=0.1, lrCheck=1)
    cl, cr = R.census(left), R.census(right)
    C = R.cost_volume(cl, cr, p)
    S = R.sgm_all_paths(C, R.rgb_to_luma(left), p)
    disp = R.wta_left(S, R.wta_right(S, p), p)

    fB = 6.0                      # px·m，让 d=8..14 → 0.43..0.75 m 这种室内尺度
    depth = np.where(disp > 0, fB / np.maximum(disp, 1e-3), 0.0)
    depth[(depth < 0.12) | (depth > 3.0)] = 0.0

    # ---- 2. depth_mm.png（16 位灰度，单位毫米）----------------------------
    mm = np.clip(np.round(depth * 1000.0), 0, 65535).astype(np.uint16)
    Image.fromarray(mm).save(os.path.join(out, "depth_mm.png"))

    # ---- 3. rgb.png（App 导出的相机原图，深度图的 2 倍分辨率）--------------
    rgb = Image.fromarray((np.clip(left, 0, 1) * 255).astype(np.uint8))
    rgb = rgb.resize((OUT_W, OUT_H), Image.BICUBIC)
    rgb.save(os.path.join(out, "rgb.png"))

    # ---- 4. view.png（模拟屏幕上看到的 Depth map 视图）--------------------
    zbuf = np.clip(np.round(depth * 100 / 0) if False else depth, 0, 1e9)
    t = np.where(depth > 0, 1.0 - np.clip((depth - 0.12) / (3.0 - 0.12), 0, 1), 0.0)
    heat = turbo_img(t.astype(np.float32))
    heat[depth <= 0] = 0
    Image.fromarray(heat).resize((OUT_W, OUT_H), Image.NEAREST).save(os.path.join(out, "view.png"))

    # ---- 5. pair.png（模拟 Rectified pair 视图：左右并排）------------------
    #      右图按每像素视差向左平移，视觉上和真机的配准对一致
    yy, xx = np.mgrid[0:H, 0:W]
    sx = np.clip(xx + np.round(disp).astype(int), 0, W - 1)
    shifted = left[yy, sx]
    pair = np.concatenate([left, shifted], axis=1)
    Image.fromarray((np.clip(pair, 0, 1) * 255).astype(np.uint8)).resize(
        (OUT_W, OUT_H // 2), Image.BILINEAR).save(os.path.join(out, "pair.png"))

    # ---- 6. meta.json（字段与真实导出一致）--------------------------------
    fx = 470.0 / (W / OUT_W)      # 注意：真实导出记录的是「深度图像素单位」的内参
    fx = 470.0
    cx, cy = W / 2.0, H / 2.0
    meta = {
        "app": "StereoVision",
        "date": "2024-11-22T20:00:00Z",
        "source": "stereo",
        "mode": "Depth map",
        "matcher": "sgm",
        "max_disparity": 64,
        "lr_check": True,
        "p1": 8,
        "p2": 96,
        "uniqueness": 0.1,
        "range_m": [0.12, 3.0],
        "rectification": {"flip": False, "dy_px": 0.0,
                          "baseline_angle_deg": -135.0, "camera_extrinsics": True},
        "detector": "YOLO11n-seg · 80 COCO classes + masks",
        "depth": {"width": W, "height": H, "unit": "mm", "file": "depth_mm.png", "invalid": 0},
        "intrinsics": {"fx": fx, "fy": fx, "cx": cx, "cy": cy,
                       "note": "pixel units of the depth map"},
        "baseline_mm": 14.0,
        "point_cloud": "cloud.ply, camera frame: x right, y down, z forward, meters",
        "note": "合成场景（tools/sgm_ref.py），非真机数据",
        "objects": [
            {"id": 1, "label": "box", "confidence": 0.86,
             "bbox_norm": [0.41, 0.31, 0.25, 0.39],
             "distance_m": float(np.median(depth[120:240, 270:410][depth[120:240, 270:410] > 0])),
             "x_m": 0.0, "width_m": 0.25 * W * 0.21 / fx, "height_m": 0.39 * H * 0.21 / fx},
            {"id": 2, "label": "chair", "confidence": 0.71,
             "bbox_norm": [0.10, 0.52, 0.22, 0.30],
             "distance_m": float(np.median(depth[200:280, 70:200][depth[200:280, 70:200] > 0])),
             "x_m": -0.45, "width_m": 0.30, "height_m": 0.42},
        ],
    }
    # 用真实的 x / width 覆盖上面的粗估
    for o, (y0, y1, x0, x1) in zip(meta["objects"], [(120, 240, 270, 410), (200, 280, 70, 200)]):
        sub = depth[y0:y1, x0:x1]
        sub = sub[sub > 0]
        if sub.size:
            z = float(np.median(sub))
            o["distance_m"] = z
            o["x_m"] = float((x0 + x1) / 2 - cx) * z / fx
            o["width_m"] = float(x1 - x0) * z / fx
            o["height_m"] = float(y1 - y0) * z / fx

    with open(os.path.join(out, "meta.json"), "w", encoding="utf-8") as f:
        json.dump(meta, f, indent=2, ensure_ascii=False)

    # ---- 7. 顺手把查看器也复制过去，双击即可打开 -------------------------
    here = os.path.dirname(os.path.abspath(__file__))
    shutil.copy(os.path.join(here, "viewer.html"), os.path.join(out, "viewer.html"))

    nvalid = int((depth > 0).sum())
    print(f"已写入 {out}/")
    for name in ["rgb.png", "depth_mm.png", "view.png", "pair.png", "meta.json", "viewer.html"]:
        p2 = os.path.join(out, name)
        print(f"  {name:<14} {os.path.getsize(p2):>9,d} B")
    print(f"有效深度 {100.0 * nvalid / depth.size:.1f}%  "
          f"范围 {depth[depth > 0].min():.2f}–{depth[depth > 0].max():.2f} m")


if __name__ == "__main__":
    main()
