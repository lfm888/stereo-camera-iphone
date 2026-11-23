import Accelerate
import CoreML
import ImageIO
import UIKit
import Vision

struct Mask: @unchecked Sendable {
    let rect: CGRect
    let w: Int, h: Int
    let bits: [UInt8]
    let image: CGImage
}

struct Detection: Sendable {
    var rect: CGRect
    var label: String
    var confidence: Float
    var mask: Mask?
}

func labelHue(_ label: String) -> Double {
    let h = label.unicodeScalars.reduce(UInt32(7)) { ($0 &* 31) &+ $1.value }
    return Double(h % 360) / 360
}

/// 语义分割检测器：加载 segzoo 训练并导出的 CoreML 模型（默认 lens-seg）。
///
/// 模型契约由 scripts/export_lens_seg.py 写进 CoreML metadata：
///   输入  image，RGB，img_size×img_size；预处理 = 取长宽 min 为边长的**中央正方形**再缩到 img_size
///        （训练时是按长宽相等的正方形样本喂的，整幅拉伸会把镜头拉变形成椭圆，所以用 .centerCrop）
///   输出  probs，(1, num_classes, H, W) 的逐像素 softmax 概率，通道 0 = background
///
/// 采集到的 buffer 一直是传感器的横屏坐标系，手机竖着拿时画面在 buffer 里躺了 90°，而模型只认识正立的
/// 物体（训练数据就是正立的人脸 / 眼镜）。所以这里把当前 DisplayOrientation 交给 Vision，让它先把图转正
/// （转完之后才做中央裁剪 + 缩放），解出来的掩码再按裁剪偏移和方向转回 buffer 坐标系给测距用。
/// 解码：逐像素取 argmax，概率 ≥ 置信度且类别 ≥ 1 才算前景 → 去掉过小的连通域 → 同一类的所有
/// 连通域合并成一个物体（比如一副眼镜的左右两片镜片算一个），再生成框、置信度与掩码。
/// 模型缺失时退回 Apple 内置检测器（人 / 动物 / 显著物体），保证没有模型也能用。
final class Detector: @unchecked Sendable {
    let name: String
    private let queue = DispatchQueue(label: "detector", qos: .userInitiated)
    private let lock = NSLock()
    private var busy = false
    private var results: [Detection] = []
    private var seq = 0
    private let request: VNCoreMLRequest?
    private let net: SegNet?

    /// 从 CoreML metadata 读出来的模型信息（前景类名按通道顺序，通道 0 是 background 已去掉）。
    private struct SegNet {
        let labels: [String]
        let width: Int
        let height: Int
    }

    private static let resource = "lens-seg"

    init() {
        let cfg = MLModelConfiguration()
        cfg.computeUnits = .all
        let ml = Bundle.main.url(forResource: Self.resource, withExtension: "mlmodelc")
            .flatMap { try? MLModel(contentsOf: $0, configuration: cfg) }
        if let ml, let vn = try? VNCoreMLModel(for: ml) {
            let r = VNCoreMLRequest(model: vn)
            r.imageCropAndScaleOption = .centerCrop      // 长宽 min 的中央正方形，再等比缩到模型输入
            request = r
            let net = Self.describe(ml)
            self.net = net
            name = "LensSeg (\(net.labels.joined(separator: "/")) · \(net.width)×\(net.height))"
        } else {
            request = nil
            net = nil
            name = "Vision built-in (people, animals, saliency)"
        }
    }

    private static func describe(_ ml: MLModel) -> SegNet {
        let desc = ml.modelDescription
        let meta = desc.metadata[.creatorDefinedKey] as? [String: String] ?? [:]
        let names = (meta["classes"] ?? "").split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces) }
        let labels = names.count > 1 ? Array(names.dropFirst()) : ["Lens"]
        var w = 256, h = 256
        if let c = desc.inputDescriptionsByName.values.compactMap({ $0.imageConstraint }).first {
            w = c.pixelsWide
            h = c.pixelsHigh
        }
        return SegNet(labels: labels, width: w, height: h)
    }

    var latest: (seq: Int, items: [Detection]) {
        lock.withLock { (seq, results) }
    }

    func submit(_ pb: CVPixelBuffer, minConfidence: Float, orientation: DisplayOrientation = .landscapeRight) {
        let go: Bool = lock.withLock {
            if busy { return false }
            busy = true
            return true
        }
        guard go else { return }
        queue.async { [self] in
            let r: [Detection]
            if let request, let net {
                // 按当前屏幕方向把 buffer 转正，模型看到的才和训练时一致
                try? VNImageRequestHandler(cvPixelBuffer: pb, orientation: orientation.visionOrientation).perform([request])
                r = decodeSemantic(request, net, minConfidence, orientation,
                                   frame: (CVPixelBufferGetWidth(pb), CVPixelBufferGetHeight(pb)))
            } else {
                r = runBuiltin(pb, minConfidence, orientation)
            }
            lock.withLock {
                results = r
                seq += 1
                busy = false
            }
        }
    }

    private static func topLeft(_ r: CGRect) -> CGRect {
        CGRect(x: r.minX, y: 1 - r.maxY, width: r.width, height: r.height)
    }

    private func decodeSemantic(_ req: VNCoreMLRequest, _ net: SegNet, _ minConf: Float,
                                _ orientation: DisplayOrientation, frame: (w: Int, h: Int)) -> [Detection] {
        let arrays = (req.results as? [VNCoreMLFeatureValueObservation] ?? []).compactMap { $0.featureValue.multiArrayValue }
        guard let probs = arrays.first(where: { $0.shape.count == 4 }) else { return [] }
        let c = probs.shape[1].intValue, h = probs.shape[2].intValue, w = probs.shape[3].intValue
        guard c > 1, w > 1, h > 1 else { return [] }
        let read = reader(probs)
        let sc = probs.strides[1].intValue, sh = probs.strides[2].intValue, sw = probs.strides[3].intValue

        // 逐像素 argmax：0 = background，k = 第 k 个前景类（概率不足置信度的按背景处理）
        let n = w * h
        var cls = [UInt8](repeating: 0, count: n)
        var conf = [Float](repeating: 0, count: n)
        for y in 0..<h {
            let row = y * sh
            for x in 0..<w {
                let base = row + x * sw
                var best: Float = 0, bc = 0
                for k in 0..<c {
                    let v = read(k * sc + base)
                    if v > best { best = v; bc = k }
                }
                if bc > 0 && best >= minConf {
                    let i = y * w + x
                    cls[i] = UInt8(truncatingIfNeeded: bc)
                    conf[i] = best
                }
            }
        }

        // 4 邻接连通域：keep = 1 保留，2 太小丢弃（去掉零星噪点，避免把框撑大）
        var keep = [UInt8](repeating: 0, count: n)
        var stack: [Int] = []
        let minArea = max(24, n / 500)
        for start in 0..<n where cls[start] != 0 && keep[start] == 0 {
            let label = cls[start]
            var comp: [Int] = []
            keep[start] = 1
            stack.append(start)
            while let p = stack.popLast() {
                comp.append(p)
                let px = p % w, py = p / w
                if px > 0 { push(&stack, &keep, cls, p - 1, label) }
                if px < w - 1 { push(&stack, &keep, cls, p + 1, label) }
                if py > 0 { push(&stack, &keep, cls, p - w, label) }
                if py < h - 1 { push(&stack, &keep, cls, p + w, label) }
            }
            if comp.count < minArea { for p in comp { keep[p] = 2 } }
        }

        // 同一前景类的所有连通域合并成一个物体
        struct Acc { var x0 = Int.max, y0 = Int.max, x1 = -1, y1 = -1, count = 0; var sum: Float = 0 }
        var accs = [Int: Acc]()
        for i in 0..<n where keep[i] == 1 {
            let k = Int(cls[i]), x = i % w, y = i / w
            var a = accs[k] ?? Acc()
            a.x0 = min(a.x0, x); a.x1 = max(a.x1, x)
            a.y0 = min(a.y0, y); a.y1 = max(a.y1, y)
            a.count += 1
            a.sum += conf[i]
            accs[k] = a
        }

        var out: [Detection] = []
        for (k, a) in accs.sorted(by: { $0.value.count > $1.value.count }) {
            let mw = a.x1 - a.x0 + 1, mh = a.y1 - a.y0 + 1
            var bits = [UInt8](repeating: 0, count: mw * mh)
            for y in a.y0...a.y1 {
                for x in a.x0...a.x1 {
                    let i = y * w + x
                    if keep[i] == 1 && Int(cls[i]) == k { bits[(y - a.y0) * mw + (x - a.x0)] = 1 }
                }
            }
            // 这里的 rect/bits 在“转正 + 中央裁剪”后的图像空间，先按裁剪偏移换回转正后的整幅图（= 屏幕空间），
            // 再用 orientation.buffer 转回相机 buffer 坐标系：rect 给测距采样，image 留屏幕空间。
            let vw = CGFloat(orientation.isPortrait ? frame.h : frame.w)   // Vision 转正后的宽高
            let vh = CGFloat(orientation.isPortrait ? frame.w : frame.h)
            let side = min(vw, vh)                                         // .centerCrop 的边长
            let sx = side / vw, sy = side / vh
            let ox = (vw - side) / 2 / vw, oy = (vh - side) / 2 / vh
            let gw = CGFloat(w), gh = CGFloat(h)                           // 模型输出的网格
            let rectS = CGRect(x: ox + CGFloat(a.x0) / gw * sx, y: oy + CGFloat(a.y0) / gh * sy,
                               width: CGFloat(mw) / gw * sx, height: CGFloat(mh) / gh * sy)
            let rectB = orientation.buffer(rectS)
            let back = Self.oriented(bits, mw, mh, orientation.inverse)
            let label = k - 1 < net.labels.count ? net.labels[k - 1] : "class \(k)"
            out.append(Detection(rect: rectB, label: label, confidence: a.sum / Float(a.count),
                                 mask: Self.maskImage(bits, mw, mh, hue: labelHue(label)).map {
                                     Mask(rect: rectB, w: back.w, h: back.h, bits: back.bits, image: $0)
                                 }))
        }
        return out
    }

    private func push(_ stack: inout [Int], _ keep: inout [UInt8], _ cls: [UInt8], _ i: Int, _ label: UInt8) {
        if keep[i] == 0 && cls[i] == label {
            keep[i] = 1
            stack.append(i)
        }
    }

    private func reader(_ a: MLMultiArray) -> (Int) -> Float {
        let p = a.dataPointer
        if a.dataType == .float16 {
            return { Float(p.load(fromByteOffset: $0 * 2, as: Float16.self)) }
        }
        return { p.load(fromByteOffset: $0 * 4, as: Float.self) }
    }

    private static func maskImage(_ bits: [UInt8], _ w: Int, _ h: Int, hue: Double) -> CGImage? {
        var r: CGFloat = 0, g: CGFloat = 0, b: CGFloat = 0
        UIColor(hue: hue, saturation: 0.8, brightness: 0.95, alpha: 1).getRed(&r, green: &g, blue: &b, alpha: nil)
        let a: CGFloat = 0.45
        let px = [UInt8(r * a * 255), UInt8(g * a * 255), UInt8(b * a * 255), UInt8(a * 255)]
        var rgba = [UInt8](repeating: 0, count: w * h * 4)
        for i in 0..<(w * h) where bits[i] != 0 {
            rgba[i * 4] = px[0]; rgba[i * 4 + 1] = px[1]; rgba[i * 4 + 2] = px[2]; rgba[i * 4 + 3] = px[3]
        }
        guard let provider = CGDataProvider(data: Data(rgba) as CFData) else { return nil }
        return CGImage(width: w, height: h, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: w * 4,
                       space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedLast.rawValue),
                       provider: provider, decode: nil, shouldInterpolate: true, intent: .defaultIntent)
    }

    /// 把掩码位图从一个坐标系转成另一个（与 DisplayOrientation.screen 一致）。
    private static func oriented(_ bits: [UInt8], _ w: Int, _ h: Int, _ o: DisplayOrientation) -> (bits: [UInt8], w: Int, h: Int) {
        guard o != .landscapeRight else { return (bits, w, h) }
        var d = [UInt8](repeating: 0, count: bits.count)
        for y in 0..<h {
            for x in 0..<w {
                let v = bits[y * w + x]
                switch o {
                case .landscapeRight: d[y * w + x] = v
                case .portrait: d[x * h + (h - 1 - y)] = v                 // 顺时针 90°
                case .landscapeLeft: d[(h - 1 - y) * w + (w - 1 - x)] = v  // 180°
                case .portraitUpsideDown: d[(w - 1 - x) * h + y] = v        // 逆时针 90°
                }
            }
        }
        return (d, o.isPortrait ? h : w, o.isPortrait ? w : h)
    }

    private func runBuiltin(_ pb: CVPixelBuffer, _ minConf: Float, _ orientation: DisplayOrientation) -> [Detection] {
        // 内置检测器同样先把图转正（不转的话竖屏下画面是躺倒的，识别会变差），再把结果转回 buffer 坐标系
        let handler = VNImageRequestHandler(cvPixelBuffer: pb, orientation: orientation.visionOrientation)
        let humans = VNDetectHumanRectanglesRequest()
        humans.upperBodyOnly = false
        let animals = VNRecognizeAnimalsRequest()
        let saliency = VNGenerateObjectnessBasedSaliencyImageRequest()
        try? handler.perform([humans, animals, saliency])

        var out: [Detection] = (humans.results ?? []).filter { $0.confidence >= minConf }.map {
            Detection(rect: orientation.buffer(Self.topLeft($0.boundingBox)), label: "person", confidence: $0.confidence)
        }
        for a in animals.results ?? [] {
            guard let l = a.labels.first, l.confidence >= minConf else { continue }
            out.append(Detection(rect: orientation.buffer(Self.topLeft(a.boundingBox)), label: l.identifier.lowercased(), confidence: l.confidence))
        }
        let salient = saliency.results?.first?.salientObjects ?? []
        for s in salient.prefix(4) {
            let r = Self.topLeft(s.boundingBox)
            guard !out.contains(where: { iou($0.rect, orientation.buffer(r)) > 0.4 }) else { continue }
            let cls = VNClassifyImageRequest()
            cls.regionOfInterest = s.boundingBox
            try? handler.perform([cls])
            let top = (cls.results ?? []).first { $0.confidence > 0.12 }
            out.append(Detection(rect: orientation.buffer(r), label: top?.identifier.replacingOccurrences(of: "_", with: " ") ?? "object", confidence: top?.confidence ?? s.confidence))
        }
        return out
    }
}

func iou(_ a: CGRect, _ b: CGRect) -> CGFloat {
    let i = a.intersection(b)
    guard !i.isNull else { return 0 }
    let ia = i.width * i.height
    return ia / (a.width * a.height + b.width * b.height - ia)
}
