//
//  StereoVisionApp.swift
//  StereoVision
//
//  **App 入口**，只做一件事：把窗口接到根视图。
//
//  这里刻意不持有状态、不建视图层级：
//    UI/RootView.swift        根视图（AppModel 的持有者、两个 sheet）
//    UI/CaptureScreen.swift   主拍摄界面（HUD + 右侧功能条）
//    UI/SettingsScreen.swift  参数设置（草稿 + Apply）
//    UI/DiagnosticsScreen.swift  GPU/CPU 诊断（只读快照）
//

import SwiftUI

@main
struct StereoVisionApp: App {
    var body: some Scene {
        WindowGroup { RootView() }
    }
}
