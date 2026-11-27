//
//  Rendering/MetalView.swift
//  StereoVision
//
//  MTKView 的 SwiftUI 包装。
//
//  为什么不放在 UI/：`import MetalKit` 会让 UI/ 目录带进平台依赖，
//  而 UI/ 只允许 `import SwiftUI`（见 stereovision_agent.md §0 的红线）。
//  这个文件是"界面 ↔ Metal 渲染器"的唯一桥，放在 Rendering/ 才对。
//

import MetalKit
import SwiftUI

struct MetalView: UIViewRepresentable {
    /// MTKView 的 target（每帧 draw 由它负责）。
    let renderer: MetalSceneRenderer

    func makeUIView(context: Context) -> MTKView {
        let v = MTKView()
        renderer.configure(v)
        return v
    }

    func updateUIView(_ v: MTKView, context: Context) {}
}
