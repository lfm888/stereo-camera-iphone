#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
sgm_ref.py — StereoVision 的 GPU 立体管线在 numpy 上的逐行「孪生实现」

用途
----
在没有 Mac / Xcode / Metal 的机器上调试这套立体匹配算法。每个函数都对应
StereoVision/Shaders.metal 里的一个 kernel，行号写在 docstring 里，可以对着看。

设计原则（很重要）
----------------
1. **数值等价优先于「写法优雅」**。所有取整、截断、饱和、哨兵值、比较符号
   都按 Metal 源码逐字翻译，因为你要用它当基准去判断「真机上为什么不对」。
2. **并行结构也照抄**。sgmPath 在 GPU 上「线程组=一条扫描线、线程=一个视差」，
   这里就写成「一次处理全部扫描线、numpy 向量沿视差轴」——结构一样，所以
   将来想改 GPU 端也能 1:1 对照。
3. 不做任何「顺手优化」（比如替换成滑窗代价、加后处理滤波），否则就不是孪生了。

跑法
----
    uv run --with numpy --with pillow tools/sgm_ref.py                # 复现 README 的精度表
    uv run --with numpy --with pillow tools/sgm_ref.py --dump out/    # 顺便导出中间结果
    uv run --with numpy --with pillow tools/sgm_ref.py --split pair.png   # 跑截图里的 Rectified pair

对照的真机代码
------------
    Shaders.metal:12-24    rectify             ← 本文件未翻译（见 main 里的说明）
    Shaders.metal:44-68    census              ← census()
    Shaders.metal:129-142  costVolume          ← cost_volume()
    Shaders.metal:148-188  sgmPath             ← sgm_path()
    Shaders.metal:190-204  wtaRight            ← wta_right()
    Shaders.metal:206-228  wtaLeft             ← wta_left()
    Shaders.metal:241-262  toDepth             ← to_depth()
    Shaders.metal:71-123   match（块匹配备选）  ← match_block()
    scripts/verify_stereo.swift                ← synthetic_scene() / benchmark()
"""

from __future__ import annotations

import argparse
import dataclasses
import os
import sys

import numpy as np

# Windows 控制台默认 GBK，中文会变乱码
try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

# 与 StereoEngine.swift:110 对齐
WORK_W, WORK_H, MAX_D = 640, 360, 96

# 与 Shaders.metal:9 对齐
LUMA = np.array([0.299, 0.587, 0.114], dtype=np.float32)

BIG = np.uint32(0xFFFFFFFF)          # wta 里的 second 初值
SENTINEL = np.uint32(0x3FFFFFFF)     # sgmPath 里 prev[] 两端的哨兵


# ----------------------------------------------------------------------------
# popcount：Metal 的 popcount() 是硬件指令，这里等价实现
# ----------------------------------------------------------------------------
if hasattr(np, "bitwise_count"):
    def popcount(a):
        return np.bitwise_count(a).astype(np.uint8)
else:  # numpy < 2.0 的退路
    _LUT = np.array([bin(i).count("1") for i in range(65536)], dtype=np.uint8)

    def popcount(a):
        a = np.asarray(a, dtype=np.uint64)
        return (_LUT[a & 0xFFFF] + _LUT[(a >> 16) & 0xFFFF]
                + _LUT[(a >> 32) & 0xFFFF] + _LUT[(a >> 48) & 0xFFFF]).astype(np.uint8)


def rgb_to_luma(img: np.ndarray) -> np.ndarray:
    """img: (H,W,3) float32 in [0,1]（对应 bgra8Unorm 读出来的 .rgb）"""
    return (img.astype(np.float32) @ LUMA).astype(np.float32)


# ----------------------------------------------------------------------------
# 参数：逐字段对应 Shaders.metal:125 的 SGMParams / MatchParams
# ----------------------------------------------------------------------------
@dataclasses.dataclass
class SGMParams:
    W: int = WORK_W
    H: int = WORK_H
    D: int = 64
    dir: int = 1          # +1：右图在左图右边（视差为正）
    dx: int = 0
    dy: int = 0
    P1: int = 8
    P2: int = 96
    first: int = 1
    uniqueness: float = 0.1
    lrCheck: int = 1


# ============================================================================
# 1. census — Shaders.metal:44-68
# ============================================================================
def census(img: np.ndarray) -> np.ndarray:
    """
    7x7 邻域（去掉中心，48 个比较）压成一个 48 bit 整数描述子。

    Metal 版本把前 32 位放进 dst.x、后 16 位放进 dst.y（rg32Uint）；
    因为两者只做 XOR+popcount，等价于一个 48 bit 整数，所以这里用 uint64。
    位被置 1 的条件是「邻居比中心暗」，中心带 -0.004 偏置抑制平坦区噪声抖动。

    返回 (H,W) uint64
    """
    assert img.shape[2] == 3
    h, w = img.shape[:2]
    luma = rgb_to_luma(img)
    center = (luma - np.float32(0.004)).astype(np.float32)

    bits = np.zeros((h, w), dtype=np.uint64)
    k = 0
    for dy in range(-3, 4):          # Metal: for (int dy = -3; dy <= 3; dy++)
        ys = np.clip(np.arange(h) + dy, 0, h - 1)          # 坐标 clamp，不是跳过
        for dx in range(-3, 4):
            if dx == 0 and dy == 0:
                continue
            xs = np.clip(np.arange(w) + dx, 0, w - 1)
            darker = (luma[np.ix_(ys, xs)] < center)       # bool
            bits |= darker.astype(np.uint64) << np.uint64(k)
            k += 1
    assert k == 48
    return bits


# ============================================================================
# 2. costVolume — Shaders.metal:129-142
# ============================================================================
def cost_volume(cl: np.ndarray, cr: np.ndarray, p: SGMParams) -> np.ndarray:
    """
    匹配代价 = Census 汉明距离，存 uchar（0..48），48 兼作「搜索越界」的标记。

    ★ 布局是 AoS：C[y, x, d]，和 Metal 的 [(y*W+x)*D + d] 完全一致。
      这是 GPU 上一切 coalescing 的来源，改动它会让真机慢一个数量级。

    返回 (H,W,D) uint8
    """
    h, w = cl.shape
    d_max = p.D
    C = np.full((h, w, d_max), 48, dtype=np.uint8)   # 越界默认 48
    for d in range(d_max):
        if p.dir > 0:                 # xr = x - d，需要 x >= d
            if d >= w:
                break
            C[:, d:, d] = popcount(cl[:, d:] ^ cr[:, : w - d])
        else:                         # xr = x + d，需要 x < w-d
            if d >= w:
                break
            C[:, : w - d, d] = popcount(cl[:, : w - d] ^ cr[:, d:])
    return C


# ============================================================================
# 3. sgmPath — Shaders.metal:148-188   ★ 全篇核心
# ============================================================================
def sgm_path(C: np.ndarray, S: np.ndarray, luma: np.ndarray,
             p: SGMParams, dx: int, dy: int, first: bool) -> None:
    """
    沿一个方向做 SGM 路径聚合，把结果累加/覆盖进 S（uint16，(H,W,D)）。

    GPU 端结构：一个线程组 = 一条扫描线，组内 D 个线程 = D 个视差，
    沿扫描线串行推进（DP 的方向依赖只能在扫描线方向上是串行的）。
    这里用 numpy 一次并行处理「所有扫描线」，沿扫描线位置循环 —— 结构同构。

    递推（原文 Shaders.metal:167-170）：
        P2' = max(P1+1, P2 / (1 + 16*|I - I_prev|))      ← 梯度自适应罚项
        m   = min(prev[d+1], min(prev[d], prev[d+2]) + P1, min_d(prev) + P2')
        L   = C + m - min_d(prev)                        ← 减去上一像素最小值做自归一化

    注意 prev 的索引偏移：prev[d] / prev[d+1] / prev[d+2] 对应上一像素的
    L[d-1] / L[d] / L[d+1]，两端是 0x3FFFFFFF 哨兵（模拟视差越界）。
    """
    h, w, D = C.shape
    horizontal = dx != 0
    n_lines = h if horizontal else w          # 扫描线条数（= 线程组数）
    steps = w if horizontal else h            # 每条扫描线的长度

    prev = np.full((n_lines, D + 2), SENTINEL, dtype=np.uint32)
    prev_min = np.zeros(n_lines, dtype=np.uint32)
    Ip = np.zeros(n_lines, dtype=np.float32)

    line_idx = np.arange(n_lines)
    P1 = np.uint32(p.P1)
    P2 = np.uint32(p.P2)

    for s in range(steps):
        if horizontal:
            y = line_idx
            x = np.full(n_lines, s if dx > 0 else w - 1 - s)
        else:
            x = line_idx
            y = np.full(n_lines, s if dy > 0 else h - 1 - s)

        c = C[y, x, :].astype(np.uint32)                 # (lines, D)
        I = luma[y, x]                                   # (lines,)

        if s == 0:
            L = c                                        # Metal: uint L = c;
        else:
            pm = prev_min                                # (lines,)
            # P2' = max(P1+1, uint(P2 / (1 + |I-Ip|*16)))   —— float→uint 截断
            p2_eff = np.maximum(P1 + np.uint32(1),
                                (P2.astype(np.float32) /
                                 (np.float32(1.0) + np.abs(I - Ip) * np.float32(16.0))
                                 ).astype(np.uint32))
            a = prev[:, 0:D]                             # L[d-1]
            b = prev[:, 1:D + 1]                         # L[d]
            e = prev[:, 2:D + 2]                         # L[d+1]
            inner = np.minimum(a, e) + P1
            m = np.minimum(np.minimum(b, inner), (pm + p2_eff)[:, None])
            L = c + m - pm[:, None]

        prev[:, 1:D + 1] = L                             # Metal: prev[d+1] = L
        if first:
            S[y, x, :] = L.astype(np.uint16)             # 第一条路径直接覆盖
        else:
            S[y, x, :] = np.minimum(S[y, x, :].astype(np.uint32) + L,
                                    np.uint32(65535)).astype(np.uint16)   # 饱和累加
        prev_min = L.min(axis=1)                         # simd_min + mins[] 归约
        Ip = I


def sgm_all_paths(C, luma, p, first_reset=True) -> np.ndarray:
    """4 条路径：(+x, -x, +y, -y)，顺序与 StereoEngine.swift:277 一致。"""
    h, w, D = C.shape
    S = np.zeros((h, w, D), dtype=np.uint16)
    for i, (dx, dy) in enumerate([(1, 0), (-1, 0), (0, 1), (0, -1)]):
        sgm_path(C, S, luma, p, dx, dy, first=(i == 0 if first_reset else False))
    return S


# ============================================================================
# 4. wtaRight — Shaders.metal:190-204
# ============================================================================
def wta_right(S: np.ndarray, p: SGMParams) -> np.ndarray:
    """
    右视差图（只有整数 WTA，无亚像素、无有效性判断），仅供左右一致性检查使用。
    返回 (H,W) float32
    """
    h, w, D = S.shape
    best = np.full((h, w), BIG, dtype=np.uint32)
    bd = np.zeros((h, w), dtype=np.int32)
    for d in range(D):
        n = w - d                    # xl 有效范围
        if n <= 0:
            break
        c = np.full((h, w), BIG, dtype=np.uint32)
        if p.dir > 0:                # xl = x + d
            c[:, :n] = S[:, d:, d]
        else:                        # xl = x - d
            c[:, d:] = S[:, :n, d]
        upd = c < best
        best = np.where(upd, c, best)
        bd = np.where(upd, d, bd)
    return bd.astype(np.float32)


# ============================================================================
# 5. wtaLeft — Shaders.metal:206-228
# ============================================================================
def wta_left(S: np.ndarray, dispR: np.ndarray, p: SGMParams) -> np.ndarray:
    """
    WTA + uniqueness + 左右一致性 + 亚像素抛物线拟合，一次遍历全做完。
    无输出 = 0.0（约定为「无效」，下游所有模块都认这个约定）。
    返回 (H,W) float32
    """
    h, w, D = S.shape
    best = np.full((h, w), BIG, dtype=np.uint32)
    bd = np.zeros((h, w), dtype=np.int32)

    # Metal: count = 满足 0 <= x - dir*d < W 的 d 的个数（至少 3 个才继续）
    xs = np.arange(w)
    cnt = np.minimum(D, (xs + 1) if p.dir > 0 else (w - xs)).astype(np.int32)[None, :]
    cnt = np.broadcast_to(cnt, (h, w))

    # ★ 注意：wtaLeft 直接读左参考代价体积的 S[y, x, d]，**不平移**，
    #    与 wtaRight 的 xl = x + dir*d 不同。只把 d >= count 的那些屏蔽成 BIG。
    for d in range(D):
        c = np.where(d < cnt, S[:, :, d].astype(np.uint32), BIG)
        upd = c < best
        best = np.where(upd, c, best)
        bd = np.where(upd, d, bd)

    second = np.full((h, w), BIG, dtype=np.uint32)
    for d in range(D):
        c = np.where(d < cnt, S[:, :, d].astype(np.uint32), BIG)
        take = (np.abs(d - bd) > 1) & (c < second)     # |d-bd|>1 才算 second
        second = np.where(take, c, second)

    bestf = best.astype(np.float32)
    ok = ((bd > 0) & (bd < cnt - 1)
          & (bestf * np.float32(1.0 + p.uniqueness) < second.astype(np.float32)))
    ok &= (cnt >= 3)

    # 左右一致性：拿右视差图在 x - dir*bd 处比对
    if p.lrCheck:
        src_x = np.arange(w)[None, :] - p.dir * bd
        inb = (src_x >= 0) & (src_x < w) & ok
        dr = np.zeros((h, w), dtype=np.float32)
        np.copyto(dr, dispR[np.arange(h)[:, None], np.clip(src_x, 0, w - 1)],
                  where=inb)
        ok &= inb & (np.abs(dr - bd.astype(np.float32)) <= 1.0)

    # 亚像素：抛物线拟合 s[bd-1], s[bd], s[bd+1]
    idx = np.arange(h)[:, None] * w + np.arange(w)[None, :]
    Sf = S.reshape(-1, D)
    bd_c = np.clip(bd, 0, D - 1)
    c0 = np.where(bd > 0, Sf[idx, np.maximum(bd_c - 1, 0)].astype(np.float32), 0.0)
    c2 = np.where(bd < D - 1, Sf[idx, np.minimum(bd_c + 1, D - 1)].astype(np.float32), 0.0)
    den = c0 - 2.0 * bestf + c2
    with np.errstate(divide="ignore", invalid="ignore"):
        sub = np.clip(0.5 * (c0 - c2) / den, -0.5, 0.5)
    sub = np.where(np.isfinite(sub), sub, 0.0)

    out = np.where(ok, bd.astype(np.float32) + sub, np.float32(0.0))
    return out.astype(np.float32)


# ============================================================================
# 6. toDepth — Shaders.metal:241-262
# ============================================================================
def to_depth(disp: np.ndarray, fB: float, minZ: float, maxZ: float) -> np.ndarray:
    """
    视差 → 深度，先做 3x3 中值（在视差域做，因为 1/d 的中值 ≠ 深度的中值），
    少于 5 个有效邻居则整块判为无效（0）。
    """
    h, w = disp.shape
    pad = np.pad(disp, 1, mode="edge")                    # Metal 是 clamp 坐标
    win = np.lib.stride_tricks.sliding_window_view(pad, (3, 3))  # (H,W,3,3)
    win = win.reshape(h, w, 9).copy()
    valid = win > 0
    n = valid.sum(axis=2)
    win = np.where(valid, win, np.float32(np.inf))
    win.sort(axis=2)
    med = np.take_along_axis(win, (n // 2)[..., None], axis=2)[..., 0]
    z = np.where(n >= 5, fB / np.maximum(med, np.float32(1e-6)), 0.0).astype(np.float32)
    z[(z < minZ) | (z > maxZ)] = 0.0
    return z


# ============================================================================
# 7. match（块匹配备选路径）— Shaders.metal:71-123
# ============================================================================
def match_block(cl: np.ndarray, cr: np.ndarray, p: SGMParams) -> np.ndarray:
    """
    Census 5x5 窗口的汉明距离求和 + WTA + uniqueness + 亚像素。
    README 里那张精度表的第一行就是它（62.2% / 6.5% / 31.2%）。
    """
    R = 2                      # 半窗宽：5x5
    h, w = cl.shape
    D = min(p.D, 96)

    def box5(a):
        """5x5 盒式求和（积分图，等价于 Metal 里 25 次 texel 读取累积）。"""
        ah, aw = a.shape
        ii = np.zeros((ah + 1, aw + 1), dtype=np.uint32)
        ii[1:, 1:] = a.cumsum(0).cumsum(1)
        y0 = np.clip(np.arange(ah) - R, 0, ah)
        y1 = np.clip(np.arange(ah) + R + 1, 0, ah)
        x0 = np.clip(np.arange(aw) - R, 0, aw)
        x1 = np.clip(np.arange(aw) + R + 1, 0, aw)
        return (ii[np.ix_(y1, x1)] - ii[np.ix_(y0, x1)]
                - ii[np.ix_(y1, x0)] + ii[np.ix_(y0, x0)])

    def cost_map(d):
        """整幅图的 d 代价图；越界处填 BIG（Metal 里是 break，等价于不可选）。"""
        c = np.full((h, w), BIG, dtype=np.uint32)
        if d >= w:
            return c
        # 先在平移后的坐标系算逐像素汉明距离，再盒滤波（平移与卷积可交换），
        # 最后切回左侧图像坐标。无效列填 0，它们不可能落到可选的 x 上。
        dist = np.zeros((h, w), dtype=np.uint32)
        if p.dir > 0:                       # xr = x - d
            dist[:, d:] = popcount(cl[:, d:] ^ cr[:, : w - d]).astype(np.uint32)
            c[:, d:] = box5(dist)[:, d:]
        else:                               # xr = x + d
            dist[:, : w - d] = popcount(cl[:, : w - d] ^ cr[:, d:]).astype(np.uint32)
            c[:, : w - d] = box5(dist)[:, : w - d]
        return c

    best = np.full((h, w), BIG, dtype=np.uint32)
    bd = np.zeros((h, w), dtype=np.int32)
    costs = []
    for d in range(min(D, w)):
        c = cost_map(d)
        costs.append(c)
        upd = c < best
        best = np.where(upd, c, best)
        bd = np.where(upd, d, bd)

    # count：x 在 [R, w-R) 且 y 同样在 [R, h-R) 内（Metal 开头就 return 掉了）
    xs = np.arange(w)[None, :]
    ys = np.arange(h)[:, None]
    cnt = np.broadcast_to(np.minimum(D, (xs - R + 1) if p.dir > 0 else ((w - R) - xs)),
                          (h, w)).astype(np.int32)
    border = (xs < R) | (xs >= w - R) | (ys < R) | (ys >= h - R)
    cnt = np.where(np.broadcast_to(border, (h, w)), 0, cnt)

    second = np.full((h, w), BIG, dtype=np.uint32)
    for d, c in enumerate(costs):
        take = (np.abs(d - bd) > 1) & (c < second)
        second = np.where(take, c, second)

    bestf = best.astype(np.float32)
    ok = ((cnt >= 3) & (bd > 0) & (bd < cnt - 1)
          & (bestf * np.float32(1.0 + p.uniqueness) < second.astype(np.float32)))

    cost_vol = np.stack(costs + [np.full((h, w), 0xFFFF, np.uint32)] * (D - len(costs)),
                        axis=2)
    bd_c = np.clip(bd, 0, D - 2)
    idx = np.arange(h)[:, None] * w + np.arange(w)[None, :]
    Cf = cost_vol.reshape(-1, D)
    c0 = np.where(bd > 0, Cf[idx, np.maximum(bd_c - 1, 0)].astype(np.float32), 0.0)
    c2 = np.where(bd < D - 1, Cf[idx, np.minimum(bd_c + 1, D - 1)].astype(np.float32), 0.0)
    den = c0 - 2.0 * bestf + c2
    with np.errstate(divide="ignore", invalid="ignore"):
        sub = np.clip(0.5 * (c0 - c2) / den, -0.5, 0.5)
    sub = np.where(np.isfinite(sub), sub, 0.0)

    return np.where(ok, bd.astype(np.float32) + sub, np.float32(0.0)).astype(np.float32)


# ============================================================================
# 8. 合成场景 —— 逐字对应 scripts/verify_stereo.swift:20-60
# ============================================================================
def synthetic_scene(seed: int = 0):
    """
    斜面 d = 8 + y/H*6、矩形 d = 30、y∈[200,330) 一条对比度仅 4% 的低纹理带。
    返回 (left_rgb, right_rgb) 各 (H,W,3) float32 in [0,1]，以及 truth(x,y)。
    """
    h, w = WORK_H, WORK_W
    rng = np.random.default_rng(seed)
    tex_w = w + 64

    base = rng.random((h, tex_w), dtype=np.float32)
    blurred = base.copy()
    blurred[1:-1, 1:-1] = (base[0:-2, 0:-2] + base[0:-2, 1:-1] + base[0:-2, 2:]
                           + base[1:-1, 0:-2] + base[1:-1, 1:-1] + base[1:-1, 2:]
                           + base[2:, 0:-2] + base[2:, 1:-1] + base[2:, 2:]) / 9.0

    def sample(xf, y):
        x0 = np.clip(xf.astype(np.int32), 0, tex_w - 2)
        f = xf.astype(np.float32) - x0
        v = blurred[y, x0] * (1 - f) + blurred[y, x0 + 1] * f
        contrast = np.where((y > 200) & (y < 330), 0.04, 1.0).astype(np.float32)
        return np.float32(0.5) + (v - np.float32(0.5)) * contrast

    def truth(x, y):
        rect = (x >= 260) & (x < 420) & (y >= 110) & (y < 250)
        return np.where(rect, 30.0, 8.0 + y.astype(np.float32) / h * 6.0).astype(np.float32)

    yy = np.repeat(np.arange(h, dtype=np.float32)[:, None], w, axis=1)
    xx = np.repeat(np.arange(w, dtype=np.float32)[None, :], h, axis=0)
    tr = truth(xx, yy)

    def quantize(v, rng):
        px = np.clip(v * 255.0 + rng.uniform(-3, 3, v.shape).astype(np.float32), 0, 255)
        return px.astype(np.uint8).astype(np.float32) / 255.0

    left = np.repeat(quantize(sample(xx, yy.astype(np.int32)), rng)[..., None], 3, axis=2)
    right = np.repeat(quantize(sample(xx + tr, yy.astype(np.int32)), rng)[..., None], 3, axis=2)
    return left, right, tr


# ============================================================================
# 9. 精度评估 —— 对应 verify_stereo.swift 底部的统计窗口
# ============================================================================
def score(disp: np.ndarray, truth: np.ndarray, D: int):
    """窗口 y∈[8,H-8)、x∈[D+8, W-8)，与 Swift 版一致。"""
    h, w = disp.shape
    sl = disp[8:h - 8, D + 8:w - 8]
    st = truth[8:h - 8, D + 8:w - 8]
    n = sl.size
    invalid = int((sl <= 0).sum())
    good = int((np.abs(sl - st) <= 1.0).sum())
    bad = n - good - invalid
    return 100.0 * good / n, 100.0 * bad / n, 100.0 * invalid / n


# ============================================================================
# 10. 中间结果可视化（这是本地孪生相对真机的最大优势）
# ============================================================================
def turbo(x: np.ndarray) -> np.ndarray:
    """Shaders.metal:264-282 的 turbo 多项式，用于和 App 的 Depth map 视图对色。"""
    x = np.clip(x, 0, 1).astype(np.float32)
    v4 = np.stack([np.ones_like(x), x, x * x, x ** 3], -1)
    v2 = v4[..., 2:4] * v4[..., 2:3]        # Metal: float2 v2 = v4.zw * v4.z → (x^4, x^5)
    kr4 = np.array([0.13572138, 4.61539260, -42.66032258, 132.13108234], np.float32)
    kg4 = np.array([0.09140261, 2.19418839, 4.84296658, -14.18503333], np.float32)
    kb4 = np.array([0.10667330, 12.64194608, -60.58204836, 110.36276771], np.float32)
    kr2 = np.array([-152.94239396, 59.28637943], np.float32)
    kg2 = np.array([4.27729857, 2.82956604], np.float32)
    kb2 = np.array([-89.90310912, 27.34824973], np.float32)
    r = v4 @ kr4 + v2 @ kr2
    g = v4 @ kg4 + v2 @ kg2
    b = v4 @ kb4 + v2 @ kb2
    return np.clip(np.stack([r, g, b], -1), 0, 1)


def save_png(path: str, img: np.ndarray) -> None:
    """img: (H,W) in [0,1] 灰度 / (H,W,3) in [0,1]"""
    try:
        from PIL import Image
    except ImportError:
        np.save(path.replace(".png", ".npy"), img)
        return
    a = np.clip(img * 255.0 + 0.5, 0, 255).astype(np.uint8)
    Image.fromarray(a).save(path)


def dump_all(outdir, stages: dict, disp, depth, fB, minZ, maxZ):
    os.makedirs(outdir, exist_ok=True)
    for name, img in stages.items():
        save_png(os.path.join(outdir, name), img)
    if disp is not None:
        save_png(os.path.join(outdir, "05_disp.png"), disp / max(disp.max(), 1.0))
        dnorm = np.where(disp > 0, disp / max(disp.max(), 1.0), 0)
        save_png(os.path.join(outdir, "05_disp_turbo.png"), turbo(dnorm))
    if depth is not None:
        z = np.where(depth > 0, 1.0 - (depth - minZ) / (maxZ - minZ), 0)
        save_png(os.path.join(outdir, "06_depth_turbo.png"), turbo(np.clip(z, 0, 1)))
    print(f"  → 中间结果已写入 {outdir}/")


# ============================================================================
# 11.5 自检：把 Metal 的循环逐字写成朴素版，与上面的向量版逐元素对比
#      —— 用来证明「孪生实现」真的等价，而不是“看起来像”
# ============================================================================
def _naive_cost_volume(cl, cr, p):
    h, w = cl.shape
    C = np.full((h, w, p.D), 48, dtype=np.uint8)
    for y in range(h):
        for x in range(w):
            for d in range(p.D):
                xr = x - p.dir * d
                if 0 <= xr < w:
                    C[y, x, d] = bin(int(cl[y, x] ^ cr[y, xr])).count("1")
    return C


def _naive_sgm_path(C, S, luma, p, dx, dy, first):
    """
    Shaders.metal:161-187 的字面翻译，一条扫描线一个循环。

    要点：Metal 里 threadgroup_barrier 位于「读 prev」之后、「写 prev」之前，
    所以同一像素内所有 d 读到的都是**上一个像素**的路径代价。这里用 new[]
    先收集、循环结束再整体换成 prev[]，顺序与 barrier 语义一致。
    """
    h, w, D = C.shape
    n_lines = h if dx != 0 else w
    steps = w if dx != 0 else h
    for line in range(n_lines):
        if dx != 0:
            x, y = (0, line) if dx > 0 else (w - 1, line)
        else:
            x, y = (line, 0) if dy > 0 else (line, h - 1)
        prev = [0x3FFFFFFF] * (D + 2)          # prev[0]、prev[D+1] 永不修改
        prev_min = 0
        Ip = np.float32(0.0)
        for s in range(steps):
            row = C[y, x, :]
            I = np.float32(luma[y, x])
            new = [0] * D
            for d in range(D):
                cc = int(row[d])
                if s == 0:
                    L = cc
                else:
                    p2 = max(np.uint32(p.P1 + 1),
                             np.uint32(np.float32(p.P2)
                                       / (np.float32(1.0) + np.abs(I - Ip) * np.float32(16.0))))
                    m = min(prev[d + 1], min(prev[d], prev[d + 2]) + p.P1, int(prev_min) + int(p2))
                    L = cc + m - int(prev_min)
                new[d] = L
                S[y, x, d] = np.uint16(L) if first else np.uint16(min(int(S[y, x, d]) + L, 65535))
            prev[1:D + 1] = new
            prev_min = min(new)
            Ip = I
            x += dx
            y += dy


def _naive_wta_right(S, p):
    h, w, D = S.shape
    out = np.zeros((h, w), np.float32)
    for y in range(h):
        for x in range(w):
            best, bd = 0xFFFFFFFF, 0
            for d in range(D):
                xl = x + p.dir * d
                if not (0 <= xl < w):
                    break
                c = int(S[y, xl, d])
                if c < best:
                    best, bd = c, d
            out[y, x] = bd
    return out


def _naive_wta_left(S, dispR, p):
    h, w, D = S.shape
    out = np.zeros((h, w), np.float32)
    for y in range(h):
        for x in range(w):
            count = 0
            for d in range(D):
                xr = x - p.dir * d
                if not (0 <= xr < w):
                    break
                count = d + 1
            if count < 3:
                continue
            s = S[y, x, :]
            bd = 0
            for d in range(1, count):
                if s[d] < s[bd]:
                    bd = d
            second = 0xFFFFFFFF
            for d in range(count):
                if abs(d - bd) > 1:
                    second = min(second, int(s[d]))
            best = np.float32(s[bd])
            ok = (bd > 0 and bd < count - 1
                  and best * np.float32(1.0 + p.uniqueness) < np.float32(second))
            if ok and p.lrCheck:
                dr = dispR[y, x - p.dir * bd]
                ok = abs(dr - float(bd)) <= 1.0
            sub = np.float32(0.0)
            if ok:
                c0, c2 = np.float32(s[bd - 1]), np.float32(s[bd + 1])
                den = c0 - np.float32(2.0) * best + c2
                if den > 0:
                    sub = np.clip(np.float32(0.5) * (c0 - c2) / den, -0.5, 0.5)
            out[y, x] = bd + sub if ok else 0.0
    return out


def selftest(verbose=True):
    rng = np.random.default_rng(7)
    h, w, D = 9, 21, 8
    p = SGMParams(W=w, H=h, D=D, dir=1, P1=8, P2=96, uniqueness=0.1, lrCheck=1)
    cl = rng.integers(0, 1 << 48, size=(h, w), dtype=np.uint64)
    cr = rng.integers(0, 1 << 48, size=(h, w), dtype=np.uint64)
    luma = rng.random((h, w), dtype=np.float32)
    ok = True

    C_fast = cost_volume(cl, cr, p)
    C_slow = _naive_cost_volume(cl, cr, p)
    ok &= bool(np.array_equal(C_fast, C_slow))
    if verbose:
        print(f"  cost_volume         {'OK' if np.array_equal(C_fast, C_slow) else 'FAIL'}")

    for (dx, dy), first in zip([(1, 0), (-1, 0), (0, 1), (0, -1)], [True, False, False, False]):
        S_fast = np.zeros((h, w, D), np.uint16)
        sgm_path(C_fast, S_fast, luma, p, dx, dy, first)
        S_slow = np.zeros((h, w, D), np.uint16)
        _naive_sgm_path(C_fast, S_slow, luma, p, dx, dy, first)
        same = np.array_equal(S_fast, S_slow)
        ok &= same
        if verbose:
            print(f"  sgm_path dx={dx:+d} dy={dy:+d} {'OK' if same else 'FAIL'}")

    dR_fast, dR_slow = wta_right(C_fast, p), _naive_wta_right(C_fast, p)
    same = np.array_equal(dR_fast, dR_slow)
    ok &= same
    if verbose:
        print(f"  wta_right           {'OK' if same else 'FAIL'}")

    dL_fast = wta_left(C_fast, dR_fast, p)
    dL_slow = _naive_wta_left(C_fast, dR_slow, p)
    same = np.allclose(dL_fast, dL_slow, atol=1e-6)
    ok &= same
    if verbose:
        print(f"  wta_left            {'OK' if same else 'FAIL'}")

    # 语义自检：一对完全相同的图 → 最优点是 d=0，但 bd>0 的要求会把它判为无效
    flat = np.zeros((h, w), np.uint64)
    S = sgm_all_paths(cost_volume(flat, flat, p), np.zeros((h, w), np.float32), p)
    d = wta_left(S, wta_right(S, p), p)
    same_img_ok = bool((d == 0).all())
    ok &= same_img_ok
    if verbose:
        print(f"  相同图 → 全无效     {'OK' if same_img_ok else 'FAIL'}")
    return ok


# ============================================================================
# 12. 完整管线 + benchmark
# ============================================================================
def pipeline(left, right, p: SGMParams, use_sgm: bool, lr: bool, dump_dir=None):
    fB = 0.66          # px·m：f≈470 px × B≈1.4 mm（README 给的量级）
    minZ, maxZ = 0.12, 3.0

    cl, cr = census(left), census(right)
    stages = {
        "00_left.png": left,
        "01_census_left_popcount.png": popcount(cl).astype(np.float32) / 48.0,
    }
    p2 = dataclasses.replace(p, lrCheck=int(lr))

    if use_sgm:
        C = cost_volume(cl, cr, p2)
        stages["02_cost_d32.png"] = C[:, :, 32].astype(np.float32) / 48.0
        luma = rgb_to_luma(left)
        # 每条路径单独跑一次并留住结果（方便定位是哪条路径带来的梯子条纹，真机上做不到）
        Sdirs = np.zeros((4, p2.H, p2.W, p2.D), np.uint16)
        for i, (dx, dy) in enumerate([(1, 0), (-1, 0), (0, 1), (0, -1)]):
            sgm_path(C, Sdirs[i], luma, p2, dx, dy, True)
        # 4 条路径饱和累加，等价于连续 4 次 first=1/0 的累加
        S = np.minimum(Sdirs.astype(np.uint32).sum(axis=0), np.uint32(65535)).astype(np.uint16)
        for i, name in enumerate(["x+", "x-", "y+", "y-"]):
            stages[f"03_path_{name}.png"] = (Sdirs[i, :, :, 32].astype(np.float32)
                                             / max(int(Sdirs[i].max()), 1))
        stages["03_pathsum_d32.png"] = S[:, :, 32].astype(np.float32) / max(int(S.max()), 1)
        dispR = wta_right(S, p2)
        disp = wta_left(S, dispR, p2)
        stages["04_disp_right.png"] = dispR / max(dispR.max(), 1)
    else:
        disp = match_block(cl, cr, p2)

    depth = to_depth(disp, fB, minZ, maxZ)
    if dump_dir:
        dump_all(dump_dir, stages, disp, depth, fB, minZ, maxZ)
    return disp, depth


def load_pair(args):
    """从截图 / 图像文件读入一对已矫正的图像。--split 表示左右并排。"""
    try:
        from PIL import Image
    except ImportError:
        raise SystemExit("需要 pillow：uv run --with numpy --with pillow tools/sgm_ref.py ...")
    if args.split:
        img = Image.open(args.split).convert("RGB")
        w, h = img.size
        a = img.crop((0, 0, w // 2, h))
        b = img.crop((w // 2, 0, w, h))
    else:
        a = Image.open(args.left).convert("RGB")
        b = Image.open(args.right).convert("RGB")
    a = np.asarray(a.resize((WORK_W, WORK_H), Image.BILINEAR), np.float32) / 255.0
    b = np.asarray(b.resize((WORK_W, WORK_H), Image.BILINEAR), np.float32) / 255.0
    return a, b


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--D", type=int, default=64, help="最大视差（真机默认 64，上限 96）")
    ap.add_argument("--p1", type=int, default=8)
    ap.add_argument("--p2", type=int, default=96)
    ap.add_argument("--uniq", type=float, default=0.1)
    ap.add_argument("--dump", metavar="DIR", help="导出中间结果 PNG")
    ap.add_argument("--split", metavar="IMG", help="左右并排的 Rectified pair 截图")
    ap.add_argument("--selftest", action="store_true", help="向量版 vs 朴素循环版逐元素对比")
    ap.add_argument("--left"), ap.add_argument("--right")
    args = ap.parse_args()

    if args.selftest:
        print("自检（向量版 vs Metal 循环的逐字翻译）:")
        print("全部通过" if selftest() else "有不一致，请检查")
        return

    p = SGMParams(D=min(args.D, MAX_D), P1=args.p1, P2=args.p2, uniqueness=args.uniq)

    if args.split or (args.left and args.right):
        left, right = load_pair(args)
        print(f"输入已载入 {left.shape[1]}x{left.shape[0]}  D={p.D}")
        disp, depth = pipeline(left, right, p, use_sgm=True, lr=True, dump_dir=args.dump)
        valid = disp > 0
        print(f"有效视差 {100.0*valid.mean():.1f}%  "
              f"中位数 {np.median(disp[valid]) if valid.any() else float('nan'):.2f} px  "
              f"深度中位数 {np.median(depth[depth>0]) if (depth>0).any() else float('nan'):.2f} m")
        return

    # ---- 复现 README 的精度表 ----
    left, right, truth = synthetic_scene(args.seed)
    rows = [("Block 5x5  ", False, False), ("SGM-4      ", True, False), ("SGM-4 + LR ", True, True)]
    print(f"合成场景 (seed={args.seed})：斜面 d=8..14、矩形 d=30、y∈[200,330) 对比度 4%")
    print(f"{'算法':<12} {'correct(±1px)':>14} {'wrong':>8} {'invalid':>9}")
    for name, sgm, lr in rows:
        disp, _ = pipeline(left, right, p, use_sgm=sgm, lr=lr,
                           dump_dir=(args.dump if name.startswith("SGM-4 + LR") else None))
        g, b, iv = score(disp, truth, p.D)
        print(f"{name:<12} {g:13.1f}% {b:7.1f}% {iv:8.1f}%")
    print("\nREADME 记录的参考值（纹理随机，数字会小幅浮动）：")
    print("  Block 5x5    62.2%   6.5%  31.2%")
    print("  SGM-4        75.2%  13.9%  10.8%")
    print("  SGM-4 + LR   72.4%   9.3%  18.3%")


if __name__ == "__main__":
    main()
