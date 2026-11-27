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

/// 一个阶段的诊断记录（纯显示用）。
struct StageDiag: Identifiable, Sendable {
    let id: Int
    let name: String
    let device: StageDevice
    /// nil = 这一阶段没有单独计时（例如显示、读回）
    let ms: Double?
    let note: String
}

// MARK: - 管线 → 诊断表

/// 每条管线在诊断页上叫什么、有哪几个阶段、各自跑在哪里。
///
/// 以前这张表由 Pipeline 每帧生成（文案散在后台管线里）；现在管线只交出数字
/// （`StageTimings`）和走了哪条路（`PipelineKind`），文案归显示层，统一放在这里。
/// 以后改某个阶段的叫法或备注，只需要改这一个文件。
extension PipelineKind {
    var title: String {
        switch self {
        case .metalStereo: "Metal SGM-4（全 GPU）"
        case .cpuStereo: "C++ core（CPU 匹配 + CPU 深度）"
        case .systemDepth: "系统深度（Apple / LiDAR）"
        }
    }

    func rows(_ t: StageTimings) -> [StageDiag] {
        switch self {
        case .metalStereo:
            return [
                StageDiag(id: 0, name: "rectify → census → costVolume → sgmPath×4 → wta → toDepth → unrectify",
                          device: .gpu, ms: t.gpuMs, note: "整帧一条 command buffer，未细分"),
                StageDiag(id: 1, name: "显示 / 点云渲染", device: .gpu, ms: nil, note: "MTKView，每帧一次"),
            ]
        case .cpuStereo:
            return [
                StageDiag(id: 0, name: "rectify（两路单应重采样）",
                          device: .gpu, ms: t.gpuRectifyMs, note: "Metal kernel"),
                StageDiag(id: 1, name: "像素读回",
                          device: .cpu, ms: t.readbackMs,
                          note: "CVPixelBuffer 锁定 + MTLTexture.getBytes（右图 0.9 MB）+ 每帧两次临时分配"),
                StageDiag(id: 2, name: "census → costVolume → sgmPath×4 → wta",
                          device: .cpu, ms: t.matchMs, note: "C++ 匹配：core/src/sgm.cpp"),
                StageDiag(id: 3, name: "toDepth → unrectify",
                          device: .cpu, ms: t.depthMs,
                          note: "C++ 深度：core/src/sgm.cpp + core/src/rectify.cpp，直接写进共享 MTLBuffer"),
                StageDiag(id: 4, name: "显示 / 点云渲染", device: .gpu, ms: nil, note: "MTKView，每帧一次"),
            ]
        case .systemDepth:
            return [
                StageDiag(id: 0, name: "系统深度图", device: .system, ms: nil, note: "由系统计算，App 拿不到耗时"),
                StageDiag(id: 1, name: "显示 / 点云渲染", device: .gpu, ms: nil, note: "MTKView"),
            ]
        }
    }
}

// GpuReport / DiagnosticsSnapshot 的定义在 Contracts.swift。

/// 后台诊断页：把「这一帧的每个阶段是谁算的」列出来。
///
/// 存在的理由很实际：把 SGM 从 Metal 换成 C++ 之后，GPU 上只剩矫正、视差转深度、
/// 深度回转与显示；光看帧率看不出是哪个阶段变慢、GPU 有没有真的闲下来。
struct DiagnosticsScreen: View {
    let snapshot: DiagnosticsSnapshot
    /// 关掉自己。诊断内容本身只读，这个回调只负责收起 sheet。
    let onDismiss: () -> Void

    private var r: DiagnosticsSnapshot { snapshot }

    /// 当前管线的阶段表；还没收到第一帧时为空。
    private var rows: [StageDiag] { r.kind?.rows(r.timings) ?? [] }

    var body: some View {
        NavigationStack {
            List {
                Section("概览") {
                    LabeledContent("深度来源", value: r.sourceTitle)
                    LabeledContent("匹配器", value: r.kind?.title ?? "—")
                    LabeledContent("帧率", value: String(format: "%.0f fps", r.fps))
                    LabeledContent("GPU 合计", value: String(format: "%.2f ms/帧", r.timings.gpuMs))
                    LabeledContent("CPU 合计", value: String(format: "%.2f ms/帧", r.timings.cpuMs))
                }

                Section("GPU 内存") {
                    LabeledContent("已分配", value: String(format: "%.0f MB", r.allocatedMB))
                    LabeledContent("推荐工作集上限", value: String(format: "%.0f MB", r.workingSetMB))
                    LabeledContent("深度图", value: "\(Int(r.depthSize.width))×\(Int(r.depthSize.height))")
                }

                Section("各阶段：谁在算") {
                    ForEach(rows) { s in
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
            .navigationTitle("GPU / CPU 诊断")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { Button("Done", action: onDismiss) }
        }
    }
}
