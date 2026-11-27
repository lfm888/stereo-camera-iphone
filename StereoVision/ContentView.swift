import MetalKit
import SwiftUI

@main
struct StereoVisionApp: App {
    var body: some Scene {
        WindowGroup { ContentView() }
    }
}

/// 连线根：持有一个 `AppModel`，把只读快照发给界面、把意图传下去。
///
/// 界面本身在三个文件里：
///   CaptureScreen.swift     主拍摄界面（含 HUD 与右侧功能条）
///   SettingsScreen.swift    参数设置（草稿 + Apply）
///   DiagnosticsScreen.swift GPU/CPU 诊断（零输入，纯只读面板）
struct ContentView: View {
    @State private var model = AppModel()
    @State private var showSettings = false
    @State private var showDiagnostics = false

    var body: some View {
        CaptureScreen(state: model.ui,
                      intents: model,
                      renderer: model.renderer,
                      diagnostics: model.diagnostics,
                      showSettings: $showSettings,
                      showDiagnostics: $showDiagnostics)
            .background(Color.black.ignoresSafeArea())
            .statusBarHidden()
            .persistentSystemOverlays(.hidden)
            .task { await model.startCapture() }
            .sheet(isPresented: $showSettings) {
                // 草稿从这里进：设置界面自己不会再读 AppModel。
                SettingsScreen(draft: model.draft,
                               device: model.device,
                               overlayAlpha: model.overlayAlpha,
                               intents: model,
                               onDismiss: { showSettings = false })
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
