import SwiftUI

/// **设置界面**：参数编辑。
///
/// 改的是**草稿**（`@State draft`），点 Apply 才通过 `SettingsIntents.apply` 提交；
/// Cancel 直接丢掉。理由：`maxDisparity` 一变，C++ context 就要重新分配缓冲区，
/// 不能拖一下滑杆就生效。
///
/// 只读的东西（设备信息、基线、内参）从 `DeviceInfo` 来，界面不持有 `AppModel`。
struct SettingsScreen: View {
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
                            Text(String(format: "%.2f", overlayAlpha))
                                .monospacedDigit().foregroundStyle(.secondary)
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
                    Button("Auto-calibrate rectification") {
                        intents.autoCalibrate()
                        onDismiss()
                    }
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
                                   value: String(format: "%.1f, %.1f",
                                                 device.intrinsics.cx, device.intrinsics.cy))
                    LabeledContent("Depth map",
                                   value: "\(Int(device.depthSize.width))×\(Int(device.depthSize.height))")
                }
            }
            .navigationTitle("Settings")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel", action: onDismiss)
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Apply") {
                        intents.apply(draft)
                        onDismiss()
                    }
                }
            }
        }
    }

    private func slider(_ title: String, _ v: Binding<Float>,
                        _ r: ClosedRange<Float>, _ fmt: String) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack {
                Text(title)
                Spacer()
                Text(String(format: fmt, v.wrappedValue))
                    .monospacedDigit().foregroundStyle(.secondary)
            }
            Slider(value: v, in: r)
        }
    }
}
