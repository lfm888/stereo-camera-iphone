import CoreGraphics

/// 深度连通域切出来的一个「面片块」：3D segmentation 视图用它上色和标注。
struct Blob: Identifiable, Sendable {
    let id: Int
    var rect: CGRect
    var distance: Float
    var x: Float
    var width: Float
    var cells: Int
}

/// 每帧的分析结果。**只来自深度图**，不依赖任何检测/分割模型。
struct Analysis: Sendable {
    var blobs: [Blob] = []
    var labels: [UInt8] = []
    var gridW = 0
    var gridH = 0
    var center: Float?
}

/// 纯传统方法的后处理：把深度图降采样成网格，按深度连续性做 4 邻域连通域，
/// 得到「3D segmentation」用的标签图，以及每个面片块的距离与宽度。
///
/// 全部是逐像素数组运算，没有 Apple 依赖（除了 CGRect），是搬到
/// `core/` 里最直接的一个模块——见 test.md §15.1。
final class Analyzer {
    func analyze(_ f: StereoFrame) -> Analysis {
        var a = Analysis()
        // 准星读数：画面中心那一小块深度的中值
        a.center = f.sample(CGRect(x: 0.475, y: 0.455, width: 0.05, height: 0.09), q: 0.5)
        grid(f, &a)
        return a
    }

    private func grid(_ f: StereoFrame, _ a: inout Analysis) {
        let step = max(1, f.width / 160)
        let gw = f.width / step, gh = f.height / step
        let n = gw * gh
        var g = [Float](repeating: 0, count: n)
        for gy in 0..<gh {
            for gx in 0..<gw {
                var s: Float = 0, c = 0
                for yy in 0..<step {
                    let r = f.row(gy * step + yy)
                    for xx in 0..<step {
                        let v = r[gx * step + xx]
                        if v > 0 { s += v; c += 1 }
                    }
                }
                if c * 2 >= step * step { g[gy * gw + gx] = s / Float(c) }
            }
        }

        struct Comp { var id: Int32; var count = 0; var sumZ: Float = 0; var sumX = 0; var x0 = Int.max, y0 = Int.max, x1 = 0, y1 = 0 }
        var lab = [Int32](repeating: 0, count: n)
        var comps: [Comp] = []
        var stack: [Int] = []
        var cid: Int32 = 0
        for i in 0..<n where g[i] > 0 && lab[i] == 0 {
            cid += 1
            lab[i] = cid
            stack.append(i)
            var c = Comp(id: cid)
            while let p = stack.popLast() {
                let z = g[p], px = p % gw, py = p / gw
                c.count += 1
                c.sumZ += z
                c.sumX += px
                c.x0 = min(c.x0, px); c.x1 = max(c.x1, px)
                c.y0 = min(c.y0, py); c.y1 = max(c.y1, py)
                let tol = 0.02 + 0.04 * z
                for (q, ok) in [(p - 1, px > 0), (p + 1, px < gw - 1), (p - gw, py > 0), (p + gw, py < gh - 1)] where ok {
                    if lab[q] == 0, g[q] > 0, abs(g[q] - z) < tol {
                        lab[q] = cid
                        stack.append(q)
                    }
                }
            }
            comps.append(c)
        }

        let kept = comps.filter { $0.count >= 40 }.sorted { $0.count > $1.count }.prefix(24)
        var remap = [Int32: UInt8]()
        for (k, c) in kept.enumerated() {
            let id = UInt8(k + 1)
            remap[c.id] = id
            let z = c.sumZ / Float(c.count)
            let u = (Float(c.sumX) / Float(c.count) + 0.5) * Float(step)
            let wpx = Float(c.x1 - c.x0 + 1) * Float(step)
            a.blobs.append(Blob(id: Int(id),
                                rect: CGRect(x: CGFloat(c.x0) / CGFloat(gw), y: CGFloat(c.y0) / CGFloat(gh),
                                             width: CGFloat(c.x1 - c.x0 + 1) / CGFloat(gw), height: CGFloat(c.y1 - c.y0 + 1) / CGFloat(gh)),
                                distance: z, x: (u - f.K.cx) * z / f.K.fx, width: wpx * z / f.K.fx, cells: c.count))
        }
        a.labels = lab.map { remap[$0] ?? 0 }
        a.gridW = gw
        a.gridH = gh
    }
}
