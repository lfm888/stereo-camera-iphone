import Accelerate
import CoreML
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
///   输入  image，RGB，img_size×img_size（训练时直接 resize，所以这里用 .scaleFill，不做 letterbox）
///   输出  probs，(1, num_classes, H, W) 的逐像素 softmax 概率，通道 0 = background
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
            r.imageCropAndScaleOption = .scaleFill
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
            w = c.pixelWidth
            h = c.pixelHeight
        }
        return SegNet(labels: labels, width: w, height: h)
    }

    var latest: (seq: Int, items: [Detection]) {
        lock.withLock { (seq, results) }
    }

    func submit(_ pb: CVPixelBuffer, minConfidence: Float) {
        let go: Bool = lock.withLock {
            if busy { return false }
            busy = true
            return true
        }
        guard go else { return }
        queue.async { [self] in
            let r: [Detection]
            if let request, let net {
                try? VNImageRequestHandler(cvPixelBuffer: pb, orientation: .up).perform([request])
                r = decodeSemantic(request, net, minConfidence)
            } else {
                r = runBuiltin(pb, minConfidence)
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

    private func decodeSemantic(_ req: VNCoreMLRequest, _ net: SegNet, _ minConf: Float) -> [Detection] {
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
            let rect = CGRect(x: CGFloat(a.x0) / CGFloat(w), y: CGFloat(a.y0) / CGFloat(h),
                              width: CGFloat(mw) / CGFloat(w), height: CGFloat(mh) / CGFloat(h))
            let label = k - 1 < net.labels.count ? net.labels[k - 1] : "class \(k)"
            out.append(Detection(rect: rect, label: label, confidence: a.sum / Float(a.count),
                                 mask: Self.makeMask(bits, mw, mh, rect, hue: labelHue(label))))
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

    private static func makeMask(_ bits: [UInt8], _ w: Int, _ h: Int, _ rect: CGRect, hue: Double) -> Mask? {
        var r: CGFloat = 0, g: CGFloat = 0, b: CGFloat = 0
        UIColor(hue: hue, saturation: 0.8, brightness: 0.95, alpha: 1).getRed(&r, green: &g, blue: &b, alpha: nil)
        let a: CGFloat = 0.45
        let px = [UInt8(r * a * 255), UInt8(g * a * 255), UInt8(b * a * 255), UInt8(a * 255)]
        var rgba = [UInt8](repeating: 0, count: w * h * 4)
        for i in 0..<(w * h) where bits[i] != 0 {
            rgba[i * 4] = px[0]; rgba[i * 4 + 1] = px[1]; rgba[i * 4 + 2] = px[2]; rgba[i * 4 + 3] = px[3]
        }
        guard let provider = CGDataProvider(data: Data(rgba) as CFData),
              let img = CGImage(width: w, height: h, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: w * 4,
                                space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedLast.rawValue),
                                provider: provider, decode: nil, shouldInterpolate: true, intent: .defaultIntent) else { return nil }
        return Mask(rect: rect, w: w, h: h, bits: bits, image: img)
    }

    private func runBuiltin(_ pb: CVPixelBuffer, _ minConf: Float) -> [Detection] {
        let handler = VNImageRequestHandler(cvPixelBuffer: pb, orientation: .up)
        let humans = VNDetectHumanRectanglesRequest()
        humans.upperBodyOnly = false
        let animals = VNRecognizeAnimalsRequest()
        let saliency = VNGenerateObjectnessBasedSaliencyImageRequest()
        try? handler.perform([humans, animals, saliency])

        var out: [Detection] = (humans.results ?? []).filter { $0.confidence >= minConf }.map {
            Detection(rect: Self.topLeft($0.boundingBox), label: "person", confidence: $0.confidence)
        }
        for a in animals.results ?? [] {
            guard let l = a.labels.first, l.confidence >= minConf else { continue }
            out.append(Detection(rect: Self.topLeft(a.boundingBox), label: l.identifier.lowercased(), confidence: l.confidence))
        }
        let salient = saliency.results?.first?.salientObjects ?? []
        for s in salient.prefix(4) {
            let r = Self.topLeft(s.boundingBox)
            guard !out.contains(where: { iou($0.rect, r) > 0.4 }) else { continue }
            let cls = VNClassifyImageRequest()
            cls.regionOfInterest = s.boundingBox
            try? handler.perform([cls])
            let top = (cls.results ?? []).first { $0.confidence > 0.12 }
            out.append(Detection(rect: r, label: top?.identifier.replacingOccurrences(of: "_", with: " ") ?? "object", confidence: top?.confidence ?? s.confidence))
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
