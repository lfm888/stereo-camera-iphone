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

@main  // 标记下面结构作为整个程序的起点
/* 
swift把“程序入口”做成了一个协议（App），只要某个类型遵循 App 协议，并在前面加上 @main，
编译器就会在后台自动生成真正的系统级 main() 启动逻辑，去实例化这个类型并读取它的 body
所以@main 后面定义了结构体和结构体内的计算属性 body就行，底层会自己调用 
*/
struct StereoVisionApp: App {
    /* 
    body是计算属性，底层读取app.body 时，临时跑 { ... } 里的代码算一个 Scene 交出去
    （类似 C++ 里写一个无参 get_body() 成员函数，只是语法上长得像属性）
    var body: some Scene  -- 可变计算属性 ：不透明返回类型 符合 Scene 协议
    之前学过常见的计算属性表达式 var color: Color -- 这里就有具体的返回类型
    */
    var body: some Scene { 
        /*
        { ... } 就是匿名函数（闭包），相当于 C++ 的 Lambda [&]() { return RootView(); }
        */
        WindowGroup { RootView() }
    }
}
