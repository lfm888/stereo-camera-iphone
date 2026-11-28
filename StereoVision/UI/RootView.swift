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

import Foundation
import SwiftUI

@MainActor
struct RootView: View {
    @State private var model = RootView.makeModel()
    @State private var showSettings = false
    @State private var showDiagnostics = false

    /// 单元测试宿主启动时**不要**创建引擎。
    ///
    /// `StereoVisionTests` 是桥接契约测试，只需要 `SVStereoCore`；而 `AppModel.init`
    /// 会建 `StereoEngine` → `DepthTarget`（共享 MTLBuffer 上的线性纹理）。
    /// 模拟器的 Metal 不允许这种组合（真机允许），宿主 App 会启动即崩，测试永远跑不到。
    /// 这不是产品路径：只有 XCTest 注入的 `XCTestConfigurationFilePath` 存在时才跳过。
    private static func makeModel() -> AppModel? {
        let env = ProcessInfo.processInfo.environment
        // XCTest 注入环境变量；Loaded XCTest.framework 是第二重保险（不同 Xcode 版本行为有差异）。
        let underTests = env["XCTestConfigurationFilePath"] != nil
            || NSClassFromString("XCTestCase") != nil
        return underTests ? nil : AppModel()
    }

    var body: some View {
        if let model {
            content(model)
        } else {
            // 测试宿主：不渲染相机界面，也不需要相机权限。
            Color.black.ignoresSafeArea()
        }
    }

    private func content(_ model: AppModel) -> some View {
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
