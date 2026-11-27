import SwiftUI

/// 叠在预览上的图元：准星读数、面块框、点选测距气泡。
///
/// 只吃 `CaptureUIState`（值类型），所以它既不能改状态，也不需要知道引擎与相机。
struct OverlayView: View {
    let state: CaptureUIState
    var hints = true

    var body: some View {
        Canvas { ctx, size in
            let a = state.analysis
            switch state.mode {
            case .pointCloud:
                guard hints else { break }
                pill(&ctx, "drag: rotate · pinch: zoom · tap: reset",
                     at: CGPoint(x: 10, y: size.height - 8), color: .black)
            case .stereoPair, .anaglyph:
                break
            case .segmentation:
                for b in a.blobs { blob(&ctx, size, b) }
                crosshair(&ctx, size, a.center)
            default:
                crosshair(&ctx, size, a.center)
            }
            if let t = state.tap, state.mode != .pointCloud {
                let p = CGPoint(x: t.x * size.width, y: t.y * size.height)
                ctx.stroke(Path(ellipseIn: CGRect(x: p.x - 9, y: p.y - 9, width: 18, height: 18)),
                           with: .color(.yellow), lineWidth: 2)
                pill(&ctx, state.tapDistance.map { String(format: "%.2f m", $0) } ?? "n/a",
                     at: CGPoint(x: p.x + 12, y: p.y), color: .yellow.opacity(0.9), dark: true)
            }
        }
        .allowsHitTesting(false)
    }

    /// 相机 buffer 坐标（归一化）→ 屏幕坐标：手机怎么拿，叠层就跟着转多少。
    private func view(_ r: CGRect, _ size: CGSize) -> CGRect {
        let s = state.orientation.screen(r)
        return CGRect(x: s.minX * size.width, y: s.minY * size.height,
                      width: s.width * size.width, height: s.height * size.height)
    }

    private func blob(_ ctx: inout GraphicsContext, _ size: CGSize, _ b: Blob) {
        guard b.cells >= 80 else { return }
        let r = view(b.rect, size)
        ctx.stroke(Path(r), with: .color(.white.opacity(0.7)), style: StrokeStyle(lineWidth: 1, dash: [4, 3]))
        pill(&ctx, String(format: "S%d · %.2f m · ⌀%.0f cm", b.id, b.distance, b.width * 100),
             at: CGPoint(x: r.minX, y: max(r.minY, 18)), color: .black.opacity(0.65), small: true)
    }

    private func crosshair(_ ctx: inout GraphicsContext, _ size: CGSize, _ z: Float?) {
        let c = CGPoint(x: size.width / 2, y: size.height / 2)
        var p = Path()
        p.move(to: CGPoint(x: c.x - 12, y: c.y)); p.addLine(to: CGPoint(x: c.x + 12, y: c.y))
        p.move(to: CGPoint(x: c.x, y: c.y - 12)); p.addLine(to: CGPoint(x: c.x, y: c.y + 12))
        ctx.stroke(p, with: .color(.white.opacity(0.85)), lineWidth: 1.5)
        pill(&ctx, z.map { String(format: "%.2f m", $0) } ?? "— m",
             at: CGPoint(x: c.x + 14, y: c.y - 4), color: .black.opacity(0.6), small: true)
    }

    private func pill(_ ctx: inout GraphicsContext, _ s: String, at p: CGPoint, color: Color,
                      dark: Bool = false, small: Bool = false) {
        let t = ctx.resolve(Text(s)
            .font(.system(size: small ? 10 : 12, weight: .semibold, design: .rounded))
            .foregroundStyle(dark ? .black : .white))
        let m = t.measure(in: CGSize(width: 600, height: 40))
        let r = CGRect(x: p.x, y: p.y - m.height - 4, width: m.width + 10, height: m.height + 4)
        ctx.fill(Path(roundedRect: r, cornerRadius: 4), with: .color(color))
        ctx.draw(t, at: CGPoint(x: r.midX, y: r.midY))
    }
}
