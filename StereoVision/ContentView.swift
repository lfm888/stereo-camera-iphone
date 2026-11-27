import MetalKit
import SwiftUI

@main
struct StereoVisionApp: App {
    var body: some Scene {
        WindowGroup { ContentView() }
    }
}

/// 主拍摄界面 + 三个界面之间的连线。
///
/// 这里只做三件事：持有状态、把只读快照发给界面、把意图传下去。
/// 真正的界面在 CaptureScreen / SettingsView（下一步拆到独立文件）/ DiagnosticsView 里。
struct ContentView: View {
    @State private var model = AppModel()
    @State private var showSettings = false
    @State private var showDiagnostics = false

    var body: some View {
        CaptureScreen(state: model.ui,
                      intents: model,
                      renderer: model.renderer,
                      showSettings: $showSettings,
                      showDiagnostics: $showDiagnostics)
            .background(Color.black.ignoresSafeArea())
            .statusBarHidden()
            .persistentSystemOverlays(.hidden)
            .task { await model.startCapture() }
            .sheet(isPresented: $showSettings) {
                // 草稿从这里进：界面自己不会再读 AppModel。
                SettingsView(draft: model.draft,
                             device: model.device,
                             overlayAlpha: model.overlayAlpha,
                             intents: model,
                             onDismiss: { showSettings = false })
            }
            .sheet(isPresented: $showDiagnostics) {
                DiagnosticsView(snapshot: model.diagnostics,
                                onDismiss: { showDiagnostics = false })
            }
    }
}

// MARK: - 主拍摄界面

/// 预览 + 手势 + HUD + 侧栏。
///
/// 只拿 `CaptureUIState`（值类型，改不了状态）和 `CaptureIntents`（唯一可写入口）。
struct CaptureScreen: View {
    let state: CaptureUIState
    let intents: CaptureIntents
    /// 平台对象：MTKView 的 delegate/target 必须真身传进来，它不是"状态"。
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
                            state.device.baselineMM, state.device.baselineDeg, state.device.intrinsics.fx))
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
                    Text(String(format: "● REC %02d:%02d", t / 60, t % 60)).foregroundStyle(.red).bold()
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

/// 右侧功能条：视图模式、深度来源、标定、拍照/录像、两个入口。
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
                    button("wand.and.stars", "Calibrate", selected: state.calibrating) { intents.autoCalibrate() }
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
                button("waveform.path.ecg", "GPU / CPU diagnostics", selected: showDiagnostics) { showDiagnostics = true }
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

    private func button(_ icon: String, _ label: String, selected: Bool, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: icon)
                .font(.system(size: 18, weight: .semibold))
                .frame(width: 50, height: 40)
                .background(selected ? Color.white.opacity(0.22) : .clear, in: RoundedRectangle(cornerRadius: 8))
                .foregroundStyle(selected ? .yellow : .white)
        }
        .accessibilityLabel(label)
    }
}

// MARK: - 设置界面

/// 参数编辑：改的是**草稿**，点 Apply 才进管线。
///
/// 唯一可写入口是 `SettingsIntents`；`draft`/`device`/`overlayAlpha` 都是值。
struct SettingsView: View {
    let intents: SettingsIntents
    let device: DeviceInfo
    let onDismiss: () -> Void

    @State private var draft: SettingsDraft
    @State private var overlayAlpha: Float

    init(draft: SettingsDraft, device: DeviceInfo, overlayAlpha: Float,
         intents: SettingsIntents, onDismiss: @escaping () -> Void) {
        self.intents = intents
        self.device = device
        self.onDismiss = onDismiss
        _draft = State(initialValue: draft)
        _overlayAlpha = State(initialValue: overlayAlpha)
    }

    var body: some View {
        NavigationStack {
            Form {
                Section("Depth range") {
                    slider("Min", $draft.minZ, 0.05...1, "%.2f m")
                    slider("Max", $draft.maxZ, 0.5...8, "%.1f m")
                    VStack(alignment: .leading, spacing: 2) {
                        HStack {
                            Text("Overlay opacity")
                            Spacer()
                            Text(String(format: "%.2f", overlayAlpha)).monospacedDigit().foregroundStyle(.secondary)
                        }
                        Slider(value: $overlayAlpha, in: 0...1)
                            .onChange(of: overlayAlpha) { _, v in intents.setOverlayAlpha(v) }
                    }
                }
                Section("Stereo matching") {
                    Picker("Algorithm", selection: $draft.matcher) {
                        ForEach(Matcher.allCases) { Text($0.title).tag($0) }
                    }
                    Picker("Max disparity", selection: $draft.maxDisparity) {
                        ForEach([32, 64, 96], id: \.self) { Text("\($0) px").tag($0) }
                    }
                    .pickerStyle(.segmented)
                    if draft.matcher == .sgm {
                        Toggle("Left-right consistency check", isOn: $draft.lrCheck)
                        slider("P1 (1 px jumps)", $draft.p1, 1...40, "%.0f")
                        slider("P2 (discontinuities)", $draft.p2, 20...200, "%.0f")
                    }
                    slider("Uniqueness", $draft.uniqueness, 0...0.5, "%.2f")
                    slider("Vertical offset Δy", $draft.yOffset, -12...12, "%.2f px")
                    Toggle("Flip baseline (⇄)", isOn: $draft.flip)
                    Text(device.baselineDeg > 20 || device.baselineDeg < -20
                         ? "This phone has the two lenses on a diagonal, so the images are rectified with a roll and the depth map is then rotated back to keep the picture upright. The frame corners can lack depth."
                         : "The two lenses sit side by side, so rectification only needs a small correction.")
                        .font(.caption).foregroundStyle(.secondary)
                    Button("Auto-calibrate rectification") { intents.autoCalibrate(); onDismiss() }
                }
                .disabled(!device.isMultiCam)
                Section("Source") {
                    Picker("Depth", selection: Binding(get: { device.source },
                                                       set: { intents.setSource($0) })) {
                        ForEach(DepthSource.available) { Text($0.title).tag($0) }
                    }
                    LabeledContent("Baseline", value: String(format: "%.2f mm", device.baselineMM))
                    LabeledContent("Baseline angle", value: String(format: "%+.1f°", device.baselineDeg))
                    LabeledContent("Focal length", value: String(format: "%.1f px", device.intrinsics.fx))
                    LabeledContent("Principal point",
                                   value: String(format: "%.1f, %.1f", device.intrinsics.cx, device.intrinsics.cy))
                    LabeledContent("Depth map",
                                   value: "\(Int(device.depthSize.width))×\(Int(device.depthSize.height))")
                }
                Section("C++ core") {
                    Button("在最新一帧上跑一次 CPU SGM") { intents.runCoreSelfTest() }
                    Text("Swift → ObjC++ 中间层 → 纯 C 接口 → C++ 算法。结果会显示在 HUD 左上角。")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
            .navigationTitle("Settings")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel", action: onDismiss)
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Apply") { intents.apply(draft); onDismiss() }
                }
            }
        }
    }

    private func slider(_ title: String, _ v: Binding<Float>, _ r: ClosedRange<Float>, _ fmt: String) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack {
                Text(title)
                Spacer()
                Text(String(format: fmt, v.wrappedValue)).monospacedDigit().foregroundStyle(.secondary)
            }
            Slider(value: v, in: r)
        }
    }
}

// MARK: - 平台桥

struct MetalView: UIViewRepresentable {
    let renderer: Renderer
    func makeUIView(context: Context) -> MTKView {
        let v = MTKView()
        renderer.configure(v)
        return v
    }
    func updateUIView(_ v: MTKView, context: Context) {}
}
