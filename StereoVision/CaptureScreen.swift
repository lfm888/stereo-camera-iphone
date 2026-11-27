import SwiftUI

/// **主拍摄界面**：预览 + 手势 + HUD + 右侧功能条。
///
/// 它只拿两样东西：
///   `CaptureUIState`   只读快照（值类型，改不了状态）
///   `CaptureIntents`   唯一可写入口
/// 平台对象（`Renderer`）单独传，因为它不是"状态"而是 MTKView 的 target。
struct CaptureScreen: View {
    let state: CaptureUIState
    let intents: CaptureIntents
    let renderer: Renderer
    @Binding var showSettings: Bool
    @Binding var showDiagnostics: Bool

    @State private var dragStart: (yaw: Float, pitch: Float)?
    @State private var zoomStart: Float?

    var body: some View {
        HStack(spacing: 0) {
            ZStack {
                Color.black
                GeometryReader { geo in
                    ZStack {
                        MetalView(renderer: renderer)
                        OverlayView(state: state)
                    }
                    .contentShape(Rectangle())
                    .gesture(SpatialTapGesture().onEnded { v in
                        if state.mode == .pointCloud { intents.resetOrbit(); return }
                        intents.measureTap(at: CGPoint(x: v.location.x / geo.size.width,
                                                       y: v.location.y / geo.size.height))
                    })
                    .simultaneousGesture(orbitDrag, including: state.mode == .pointCloud ? .all : .none)
                    .simultaneousGesture(orbitZoom, including: state.mode == .pointCloud ? .all : .none)
                }
                .aspectRatio(state.aspect, contentMode: .fit)
                .overlay(alignment: .topLeading) { HUD(state: state).padding(8) }
            }
            SidePanel(state: state, intents: intents,
                      showSettings: $showSettings, showDiagnostics: $showDiagnostics)
        }
    }

    /// 手势自身保留起点（SwiftUI 的 translation 是累计量），落点交给意图做绝对设置，
    /// 这样 onChanged 被调多少次都不会累积错误。
    private var orbitDrag: some Gesture {
        DragGesture(minimumDistance: 4)
            .onChanged { v in
                let s = dragStart ?? (state.yaw, state.pitch)
                dragStart = s
                intents.setOrbit(yaw: s.yaw + Float(v.translation.width) * 0.008,
                                 pitch: s.pitch + Float(v.translation.height) * 0.008)
            }
            .onEnded { _ in dragStart = nil }
    }

    private var orbitZoom: some Gesture {
        MagnifyGesture()
            .onChanged { v in
                let s = zoomStart ?? state.orbit
                zoomStart = s
                intents.setOrbitDistance(s / Float(v.magnification))
            }
            .onEnded { _ in zoomStart = nil }
    }
}

// MARK: - HUD

/// 左上角读数。纯展示，无输入。
struct HUD: View {
    let state: CaptureUIState

    private var d: SettingsDraft { state.draft }

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(state.mode.title).font(.headline)
            Text("\(Int(state.fps)) fps · \(state.orientation.label)")
            if state.source.isMultiCam {
                Text(String(format: "B %.1f mm · β %+.0f° · f %.0f px",
                            state.device.baselineMM, state.device.baselineDeg,
                            state.device.intrinsics.fx))
                if state.source == .cpuCore {
                    Text("C++ core · D \(d.maxDisparity) · " + (d.lrCheck ? "LR" : "no-LR"))
                } else {
                    Text(String(format: "Δy %.2f px%@", d.yOffset, d.flip ? " · ⇄" : "")
                         + (d.matcher == .sgm
                            ? " · SGM-4 · D \(d.maxDisparity)" + (d.lrCheck ? " · LR" : "")
                            : " · Block · D \(d.maxDisparity)"))
                }
                Text(state.gpuSummary).foregroundStyle(.white.opacity(0.75))
            } else {
                Text("\(state.source.title) · \(Int(state.device.depthSize.width))×\(Int(state.device.depthSize.height)) · f \(Int(state.device.intrinsics.fx)) px")
            }
            if state.source.isMultiCam && !state.hasExtrinsics {
                Text("No lens extrinsics: assuming a side-by-side pair").foregroundStyle(.yellow)
            }
            if state.calibrating { Text("Epipolar auto-calibration…").foregroundStyle(.yellow) }
            if let start = state.recordingStart {
                TimelineView(.periodic(from: start, by: 1)) { ctx in
                    let t = Int(ctx.date.timeIntervalSince(start))
                    Text(String(format: "● REC %02d:%02d", t / 60, t % 60))
                        .foregroundStyle(.red).bold()
                }
            }
            if let m = state.captureMessage { Text(m).foregroundStyle(.green) }
            if let s = state.status { Text(s).foregroundStyle(.red) }
        }
        .font(.caption.monospacedDigit())
        .foregroundStyle(.white)
        .padding(6)
        .background(.black.opacity(0.55), in: RoundedRectangle(cornerRadius: 8))
        .allowsHitTesting(false)
    }
}

// MARK: - 右侧功能条

/// 视图模式、深度来源、标定、拍照/录像、两个入口。
struct SidePanel: View {
    let state: CaptureUIState
    let intents: CaptureIntents
    @Binding var showSettings: Bool
    @Binding var showDiagnostics: Bool

    var body: some View {
        ScrollView(showsIndicators: false) {
            VStack(spacing: 6) {
                ForEach(ViewMode.allCases) { m in
                    let disabled = !state.source.providesRectifiedPair && m.needsStereoPair
                    button(m.icon, m.title, selected: state.mode == m) { intents.selectMode(m) }
                        .disabled(disabled)
                        .opacity(disabled ? 0.3 : 1)
                }
                Divider().background(.white.opacity(0.3)).padding(.vertical, 4)
                Button {
                    intents.selectNextSource()
                } label: {
                    Text(state.source.short)
                        .font(.system(size: 10, weight: .heavy, design: .monospaced))
                        .frame(width: 50, height: 32)
                        .background(sourceColor, in: RoundedRectangle(cornerRadius: 8))
                        .foregroundStyle(.white)
                }
                if state.source.isMultiCam {
                    button("wand.and.stars", "Calibrate", selected: state.calibrating) {
                        intents.autoCalibrate()
                    }
                }
                button("camera.aperture", "Photo", selected: false) { intents.takePhoto() }
                Button {
                    intents.toggleRecording()
                } label: {
                    Image(systemName: state.isRecording ? "stop.circle.fill" : "record.circle")
                        .font(.system(size: 22, weight: .semibold))
                        .frame(width: 50, height: 40)
                        .foregroundStyle(.red)
                }
                .accessibilityLabel(state.isRecording ? "Stop recording" : "Record video")
                button("waveform.path.ecg", "GPU / CPU diagnostics", selected: showDiagnostics) {
                    showDiagnostics = true
                }
                button("slider.horizontal.3", "Settings", selected: false) { showSettings = true }
            }
            .padding(.vertical, 10)
        }
        .frame(width: 62)
        .background(Color(white: 0.08))
    }

    private var sourceColor: Color {
        switch state.source {
        case .stereo: .orange
        case .cpuCore: .purple
        case .lidar: .green
        case .apple: .blue
        }
    }

    private func button(_ icon: String, _ label: String, selected: Bool,
                        action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: icon)
                .font(.system(size: 18, weight: .semibold))
                .frame(width: 50, height: 40)
                .background(selected ? Color.white.opacity(0.22) : .clear,
                            in: RoundedRectangle(cornerRadius: 8))
                .foregroundStyle(selected ? .yellow : .white)
        }
        .accessibilityLabel(label)
    }
}
