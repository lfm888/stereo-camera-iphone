//
//  UI/RootView.swift
//  StereoVision
//
//  连线根：持有一个 `AppModel`，把只读快照发给界面、把意图传下去。
//
//  界面本身在 UI/ 目录里：
//    CaptureScreen.swift     主拍摄界面（含 HUD 与右侧功能条）
//    SettingsScreen.swift    参数设置（草稿 + Apply）
//    DiagnosticsScreen.swift GPU/CPU 诊断（和设置一样是整屏 sheet，内容只读）
//    OverlayView.swift       叠层（准星读数、面块框、点选气泡）
//
//  红线：本文件只 import SwiftUI。平台对象（MTKView 的 target）由
//  Rendering/MetalView.swift 包着，所以 UI/ 里不会出现 MetalKit / AVFoundation。
//

import SwiftUI

struct RootView: View {
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
                // 草稿从这里进：设置界面自己不会再读 AppModel。
                SettingsScreen(draft: model.draft,
                               device: model.device,
                               overlayAlpha: model.overlayAlpha,
                               intents: model,
                               onDismiss: { showSettings = false })
            }
            .sheet(isPresented: $showDiagnostics) {
                DiagnosticsScreen(snapshot: model.diagnostics,
                                  onDismiss: { showDiagnostics = false })
            }
    }
}
