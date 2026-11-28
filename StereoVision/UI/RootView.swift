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

/*
@MainActor 意思是“这个类型/里面的成员默认都在主线程（UI 线程）上执行”。
在 C++ / 常规多线程里，你通常自己写 mutex 或把 UI 更新切回主线程；
在 Swift 并发模型里，@MainActor 是一种编译器级别的“线程亲和性/隔离保证”，
UI 视图（View）和 UI 状态一般都放在 @MainActor 下，避免跨线程改界面出问题。
*/
@MainActor
struct RootView: View { // 定义值类型结构体 RootView，遵循 View 协议（表示一个 UI 界面组件）

    /*
    @State 是 SwiftUI 的“属性包装器（Property Wrapper）”。
    普通 struct 的属性默认是不可变的（改属性相当于改整个 struct 值），
    但界面状态（比如弹窗开没开、模型对象）需要在运行时变化并触发界面刷新。
    @State 把存储托管给 SwiftUI 底层管理（类似一块受框架监控的状态槽），
    当 @State 里的状态变了，SwiftUI 会自动重新计算相关的 View。
    这里调用了类型方法 RootView.makeModel() 给 model 赋初始值。
    */
    @State private var model = RootView.makeModel()

    /*
    下面两个 @State Bool 是控制两个弹窗（sheet）是否显示的开关状态：
    showSettings 控制“设置页”弹不弹出，showDiagnostics 控制“诊断页”弹不弹出。
    */
    @State private var showSettings = false
    @State private var showDiagnostics = false

    /// 单元测试宿主启动时**不要**创建引擎。
    ///
    /// `StereoVisionTests` 是桥接契约测试，只用到 ObjC 桥接层；而 `AppModel.init`
    /// 会建 Metal 引擎 → 深度目标（共享 MTLBuffer 上的线性纹理）。
    /// 模拟器的 Metal 不允许这种组合（真机允许），宿主 App 会启动即崩，测试永远跑不到。
    /// 这不是产品路径：只有 XCTest 注入的 `XCTestConfigurationFilePath` 存在时才跳过。

    /*
    private static func makeModel() -> AppModel?
    - private：只在当前文件/作用域内可见（类似 C++ private/匿名命名空间隔离）。
    - static func：类型方法/静态方法，直接通过 RootView.makeModel() 调用，不需要先有一个 RootView 实例（类似 C++ static 成员函数）。
    - -> AppModel?：返回类型是“可选的 AppModel（Optional）”。问号 `?` 表示可能返回有效对象，也可能返回 nil（空），类似 C++ 中可能为空的指针或 std::optional<AppModel>。
    */
    private static func makeModel() -> AppModel? {
        /*
        let 声明常量（绑定后不可重新赋值，类似 C++ const）。
        ProcessInfo.processInfo.environment 拿到当前进程的环境变量字典（类似 C++ 读 getenv / envp）。
        */
        let env = ProcessInfo.processInfo.environment
        // XCTest 注入环境变量；Loaded XCTest.framework 是第二重保险（不同 Xcode 版本行为有差异）。
        /*
        != nil 表示“不为 nil（存在）”；|| 是逻辑或。
        NSClassFromString("XCTestCase") 动态查找运行时是否加载了 XCTestCase 类，没找到就是 nil。
        */
        let underTests = env["XCTestConfigurationFilePath"] != nil
            || NSClassFromString("XCTestCase") != nil

        /*
        三元运算符 `条件 ? 值为真时的结果 : 值为假时的结果`（和 C++ 的 `cond ? a : b` 一样）。
        如果在测试环境下，返回 nil；否则调用 AppModel() 构造一个 AppModel 实例返回。
        */
        return underTests ? nil : AppModel()
    }

    /*
    View 协议规定的计算属性 body：每次 SwiftUI 决定刷新界面时读取它，返回某种 View（some View）。
    */
    var body: some View {
        /*
        if let model 叫做“可选绑定（Optional Binding）”。
        因为上面的 model 类型是 AppModel?（可能是 nil），这里尝试把非空的 model 解包出来：
        如果 model 不是 nil，就进入 if 分支，且分支里拿到的 model 是解包后的非空值；
        如果是 nil，就走 else 分支。
        */
        if let model {
            content(model)
        } else {
            // 测试宿主：不渲染相机界面，也不需要相机权限。
            /*
            Color.black 是一块黑色视图；.ignoresSafeArea() 意思是铺满整个屏幕，
            连顶部刘海/状态栏、底部 Home 条的安全区域（Safe Area）也一起盖住。
            */
            Color.black.ignoresSafeArea()
        }
    }

    /*
    辅助私有方法 content(_ model: AppModel) -> some View：
    把主界面的组件拼装和修饰器（Modifier）抽出来，让 body 保持简短清晰。
    参数名前面的 `_` 表示调用时省略外部参数标签，直接写 content(model)。
    返回 some View 表示返回某种组合出来的具体 View 类型。
    */
    private func content(_ model: AppModel) -> some View {
        /*
        创建主界面 CaptureScreen，并把状态、动作接口、渲染器、两个弹窗开关传进去。
        注意 $showSettings 和 $showDiagnostics：
        前面带 `$` 是从 @State 属性取出对应的 @Binding（双向绑定句柄）。
        这样 CaptureScreen 内部如果改了 showSettings，外部 RootView 里的状态也会同步更新。
        */
        CaptureScreen(state: model.ui,
                      intents: model,
                      renderer: model.renderer,
                      showSettings: $showSettings,
                      showDiagnostics: $showDiagnostics)
            /*
            下面这一串 `.xxx(...)` 叫 View 修饰器（Modifier），采用链式调用（类似 builder 模式）：
            每次调用都在外面包一层新的视图行为/外观，最后整体作为 some View 返回。
            */
            .background(Color.black.ignoresSafeArea()) // 底层垫全屏黑背景
            .statusBarHidden()                         // 隐藏顶部状态栏
            .persistentSystemOverlays(.hidden)         // 隐藏系统覆盖层（如底部指示条等）
            /*
            .task { await model.startCapture() }：
            当这个 View 出现时，启动一个异步任务（Task），并在里面用 await 等待异步函数 model.startCapture() 执行。
            await 类似现代 C++ 协程里的 co_await，表示“这里可能挂起等异步完成，但不卡死主线程”。
            */
            .task { await model.startCapture() }
            /*
            .sheet(isPresented: $showSettings) { ... }：
            绑定一个模态弹出的半屏/整屏 Sheet 窗口。
            isPresented: $showSettings 表示“由 showSettings 这个布尔状态决定弹不弹出”；
            后面的 { SettingsScreen(...) } 是尾随闭包，决定弹出时里面显示什么界面。
            onDismiss: { showSettings = false } 则是传进去的一个回调闭包，点击关闭时把 showSettings 设回 false。
            */
            .sheet(isPresented: $showSettings) {
                // 草稿从这里进：设置界面自己不会再读 AppModel。
                SettingsScreen(draft: model.draft,
                               device: model.device,
                               overlayAlpha: model.overlayAlpha,
                               intents: model,
                               onDismiss: { showSettings = false })
            }
            /*
            第二个 .sheet，绑定 $showDiagnostics，弹出诊断界面 DiagnosticsScreen。
            */
            .sheet(isPresented: $showDiagnostics) {
                DiagnosticsScreen(snapshot: model.diagnostics,
                                  onDismiss: { showDiagnostics = false })
            }
    }
}