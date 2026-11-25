//
//  StereoVision-Bridging-Header.h
//  StereoVision
//
//  Swift 能看到的 C / Objective-C 面（三层架构里的第二层入口）。
//
//  规矩：
//    * 这里只 import 纯 C 头与 Objective-C 头；
//    * 绝不 import 任何 C++ 头（.hpp）或标准库 —— Swift 的桥接导入处理不了；
//    * C++ 的部分透过 SVStereoCore 这个 ObjC 类间接使用，实现在 Bridge/StereoCoreBridge.mm。
//

#import "Bridge/StereoCoreBridge.h"
