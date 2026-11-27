import SwiftUI

/// 一个管线阶段跑在哪里。
enum StageDevice: String, Sendable {
    case gpu = "GPU"
    case cpu = "CPU"
    /// 由系统（Apple Dual-Wide / LiDAR）算的，App 既不加时也看不见
    case system = "系统"

    var color: Color {
        switch self {
        case .gpu: .green
        case .cpu: .orange
        case .system: .blue
        }
    }
}

/// 一个阶段的诊断记录。
struct StageDiag: Identifiable, Sendable {
    let id: Int
    let name: String
    let device: StageDevice
    /// nil = 这一阶段没有单独计时（例如显示、读回）
    let ms: Double?
    let note: String
}

/// 每帧的诊断快照：谁在用 GPU、用了多久、占了多少显存。
struct GpuReport: Sendable {
    var stages: [StageDiag] = []
    var matcherName: String = "—"
    var gpuMs: Double = 0
    var cpuMs: Double = 0
    var allocatedMB: Double = 0
    var workingSetMB: Double = 0
}

/// 诊断页的**只读快照**。
///
/// 诊断页只需要这些数字。给它一个值类型，而不是 AppModel —— 这样它在编译期就写不了状态，
/// "诊断界面只显示信息"这件事不靠自觉。
struct DiagnosticsSnapshot {
    var sourceTitle: String = "—"
    var matcherName: String = "—"
    var fps: Double = 0
    var gpuMs: Double = 0
    var cpuMs: Double = 0
    var allocatedMB: Double = 0
    var workingSetMB: Double = 0
    var depthSize: CGSize = CGSize(width: 1, height: 1)
    var stages: [StageDiag] = []
}

/// 后台诊断页：把「这一帧的每个阶段是谁算的」列出来。
///
/// 存在的理由很实际：把 SGM 从 Metal 换成 C++ 之后，GPU 上只剩矫正、视差转深度、
/// 深度回转与显示；光看帧率看不出是哪个阶段变慢、GPU 有没有真的闲下来。
struct DiagnosticsScreen: View {
    let snapshot: DiagnosticsSnapshot

    private var r: DiagnosticsSnapshot { snapshot }

    var body: some View {
        NavigationStack {
            List {
                Section("概览") {
                    LabeledContent("深度来源", value: r.sourceTitle)
                    LabeledContent("匹配器", value: r.matcherName)
                    LabeledContent("帧率", value: String(format: "%.0f fps", r.fps))
                    LabeledContent("GPU 合计", value: String(format: "%.2f ms/帧", r.gpuMs))
                    LabeledContent("CPU 合计", value: String(format: "%.2f ms/帧", r.cpuMs))
                }

                Section("GPU 内存") {
                    LabeledContent("已分配", value: String(format: "%.0f MB", r.allocatedMB))
                    LabeledContent("推荐工作集上限", value: String(format: "%.0f MB", r.workingSetMB))
                    LabeledContent("深度图", value: "\(Int(r.depthSize.width))×\(Int(r.depthSize.height))")
                }

                Section("各阶段：谁在算") {
                    ForEach(r.stages) { s in
                        HStack(alignment: .top, spacing: 8) {
                            Text(s.device.rawValue)
                                .font(.caption2.monospaced().bold())
                                .foregroundStyle(s.device.color)
                                .frame(width: 30, alignment: .leading)
                                .padding(.top, 1)
                            VStack(alignment: .leading, spacing: 2) {
                                Text(s.name).font(.footnote)
                                if !s.note.isEmpty {
                                    Text(s.note).font(.caption2).foregroundStyle(.secondary)
                                }
                            }
                            Spacer(minLength: 4)
                            Text(s.ms.map { String(format: "%.2f", $0) } ?? "—")
                                .font(.caption.monospacedDigit())
                                .foregroundStyle(.secondary)
                        }
                    }
                }

                Section {
                    Text("GPU = 走 Metal 的 compute / render kernel；CPU = C++ 核心（core/）。切换深度来源为 “C++ core” 后，匹配与深度都在 CPU（只剩读回与两次矫正的单应重采样在 GPU）。")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
            .scrollContentBackground(.hidden)
            .navigationTitle("GPU / CPU 诊断")
            .navigationBarTitleDisplayMode(.inline)
        }
        // 它是盖在预览上的叠加面板（不是 sheet），所以自己铺一层半透明底。
        .background(.black.opacity(0.88))
    }
}
