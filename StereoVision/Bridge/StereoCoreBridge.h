//
//  StereoCoreBridge.h
//  StereoVision
//
//  三层架构里的**第二层（中间层）**：Objective-C++ 门面。
//
//      Swift（第三层）  →  SVStereoCore（本文件，ObjC 接口）
//                              ↓  .mm 里把 C 调用翻译过去
//                        stereo.h（纯 C 接口）
//                              ↓
//                        C++ 算法（core/src/sgm.cpp）
//
//  这一层存在的理由：Swift 不该知道 stereo_context 的生命周期、临时缓冲区、
//  行跨度与像素格式；C++ 也不该知道 CoreVideo / Metal。两边只在这个 ObjC 类上碰头。
//
//  注意：本头文件**只允许** import Foundation，不允许出现任何 C++ 类型，
//  否则 Swift 桥接导入会失败。
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/// 像素字节顺序，与 core/include/stereo/stereo.h 的 enum stereo_format 一一对应。
typedef NS_ENUM(NSInteger, SVImageFormat) {
    SVImageFormatRGB8  NS_SWIFT_NAME(rgb8)  = 0,
    SVImageFormatBGR8  NS_SWIFT_NAME(bgr8)  = 1,
    SVImageFormatGray8 NS_SWIFT_NAME(gray8) = 2,
    SVImageFormatBGRA8 NS_SWIFT_NAME(bgra8) = 3,
    SVImageFormatRGBA8 NS_SWIFT_NAME(rgba8) = 4,
};

/// 立体匹配后端（C++ 核心的 ObjC 门面）。
///
/// 一个实例持有一份 C++ context，内部缓冲区在第一次匹配后按尺寸复用；
/// 所以**不要每帧 new 一个**，把它挂在管线上长期持有。
/// 实例不是线程安全的：同一个实例不要同时从两个线程调用。
@interface SVStereoCore : NSObject

/// 后端名称，写进 HUD / meta.json 用。
@property (nonatomic, readonly, copy) NSString *backendName;

/// 最大视差（32 / 64 / 96 是 App 的三档；库本身接受 4..96）。
@property (nonatomic, readonly) NSInteger maxDisparity;

/// 最近一次失败的原因；没有错误时是空串。
@property (nonatomic, readonly, copy) NSString *lastError;

/// 创建失败（内存不足或参数非法）返回 nil。
- (nullable instancetype)initWithMaxDisparity:(NSInteger)maxDisparity
                                           p1:(NSInteger)p1
                                           p2:(NSInteger)p2
                                   uniqueness:(float)uniqueness
                                      lrCheck:(BOOL)lrCheck NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;

/// 改参数（不动 context，只是重新配置）。改变 maxDisparity 后内部缓冲区会重新分配。
/// 返回 NO 时看 lastError。
///
/// NS_SWIFT_NAME 是必需的：否则桥接会把方法名导成 `configureMaxDisparity(...)`，
/// 而不是我们想要的 `configure(maxDisparity:...)`。
- (BOOL)configureMaxDisparity:(NSInteger)maxDisparity
                           p1:(NSInteger)p1
                           p2:(NSInteger)p2
                   uniqueness:(float)uniqueness
                      lrCheck:(BOOL)lrCheck
    NS_SWIFT_NAME(configure(maxDisparity:p1:p2:uniqueness:lrCheck:));

/// 在**已矫正**的一对图上跑一次 SGM（Census 7×7 + 4 路径 + 唯一性 + 左右一致性 + 亚像素）。
///
/// left/right 由调用方保证同尺寸；stride 是**字节**行跨度（CoreVideo 与 Metal 都给对齐过的跨度）。
/// direction = +1 表示右图里的同名点在左图右侧。disparity 需要 width*height 个 float，
/// 无效像素写 0。
- (BOOL)matchLeft:(const uint8_t *)left
       leftStride:(NSInteger)leftStride
       leftFormat:(SVImageFormat)leftFormat
            right:(const uint8_t *)right
      rightStride:(NSInteger)rightStride
      rightFormat:(SVImageFormat)rightFormat
            width:(NSInteger)width
           height:(NSInteger)height
        direction:(NSInteger)direction
        disparity:(float *)disparity;

@end

NS_ASSUME_NONNULL_END
