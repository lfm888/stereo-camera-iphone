import SwiftUI

/// **设置界面**：参数编辑。
///
/// 控件是**从 C++ 的参数表生成的**（`SVParameterTable` → core/src/params.cpp）：
/// 名称、分组、类型、范围、步长、单位、枚举选项全部来自那张表，
/// 所以 Swift 里不再各写一份滑杆区间。
///
/// 改的是**草稿**（`@State draft`），点 Apply 才通过 `SettingsIntents.apply` 提交；
/// Cancel 直接丢掉。理由：`maxDisparity` 一变，C++ context 就要重新分配缓冲区，
/// 不能拖一下滑杆就生效。
///
/// 唯一的例外是 `overlay_alpha`：它是纯显示参数，即时生效（不进草稿）。
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
                ForEach(sections, id: \.title) { section in
                    Section(section.title) {
                        ForEach(section.params, id: \.identifier) { p in
                            if isApplicable(p.key) {
                                control(p)
                            }
                        }
                        if section.group == "matching" {
                            Text(rectificationNote).font(.caption).foregroundStyle(.secondary)
                            Button("Auto-calibrate rectification") {
                                intents.autoCalibrate()
                                onDismiss()
                            }
                        }
                    }
                    .disabled(section.group == "matching" && !device.isMultiCam)
                }

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

    // MARK: - 从参数表生成

    private var sections: [(group: String, title: String, params: [SVParameter])] {
        let titles = ["depth": "Depth range", "matching": "Stereo matching", "display": "Display"]
        var order: [String] = []
        var byGroup: [String: [SVParameter]] = [:]
        for p in SVParameterTable.allParameters() {
            if byGroup[p.group] == nil {
                order.append(p.group)
                byGroup[p.group] = []
            }
            byGroup[p.group]?.append(p)
        }
        return order.map { ($0, titles[$0] ?? $0, byGroup[$0] ?? []) }
    }

    /// P1/P2/左右检查只对 SGM 有意义。这是唯一按参数做的显示策略。
    private func isApplicable(_ key: String) -> Bool {
        switch key {
        case "p1", "p2", "lr_check": return draft.matcher == .sgm
        default: return true
        }
    }

    private var rectificationNote: String {
        device.baselineDeg > 20 || device.baselineDeg < -20
            ? "This phone has the two lenses on a diagonal, so the images are rectified with a roll and the depth map is then rotated back to keep the picture upright. The frame corners can lack depth."
            : "The two lenses sit side by side, so rectification only needs a small correction."
    }

    /// 一条参数 → 它的值绑定。`overlay_alpha` 即时生效，其它走草稿。
    private func binding(_ p: SVParameter) -> Binding<Float> {
        if p.key == "overlay_alpha" {
            return Binding(get: { overlayAlpha },
                           set: { overlayAlpha = $0; intents.setOverlayAlpha($0) })
        }
        return Binding(get: { draft[key: p.key] ?? p.defaultValue },
                       set: { draft[key: p.key] = $0 })
    }

    @ViewBuilder
    private func control(_ p: SVParameter) -> some View {
        switch p.kind {
        case .flag:
            let b = binding(p)
            Toggle(p.label, isOn: Binding(get: { b.wrappedValue != 0 },
                                          set: { b.wrappedValue = $0 ? 1 : 0 }))
        case .choice:
            let b = binding(p)
            Picker(p.label, selection: Binding(get: { Int(b.wrappedValue) },
                                               set: { b.wrappedValue = Float($0) })) {
                ForEach(Array(p.enumLabels.enumerated()), id: \.offset) { index, name in
                    Text(name).tag(index)
                }
            }
            .pickerStyle(.segmented)
        default:
            sliderRow(p)
        }
    }

    private func sliderRow(_ p: SVParameter) -> some View {
        let b = binding(p)
        let range = p.minValue...max(p.maxValue, p.minValue + p.step)
        return VStack(alignment: .leading, spacing: 2) {
            HStack {
                Text(p.label)
                Spacer()
                Text(formatted(p, b.wrappedValue))
                    .monospacedDigit().foregroundStyle(.secondary)
            }
            Slider(value: b, in: range, step: p.step)
        }
    }

    /// 小数位跟着步长走：步长 >= 1 取整，否则看步长是不是 0.1 的整数倍。
    private func formatted(_ p: SVParameter, _ value: Float) -> String {
        let digits: Int
        if p.step >= 1 {
            digits = 0
        } else {
            let tenths = p.step * 10
            digits = abs(tenths - tenths.rounded()) < 1e-6 ? 1 : 2
        }
        let text = String(format: "%.\(digits)f", value)
        return p.unit.isEmpty ? text : "\(text) \(p.unit)"
    }
}
