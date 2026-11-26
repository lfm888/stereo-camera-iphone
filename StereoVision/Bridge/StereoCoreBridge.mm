//
//  StereoCoreBridge.mm
//  StereoVision
//
//  ObjC++ 中间层：把 C++ 的 context / 缓冲区 / 错误串关在这个文件里，
//  对外只留 StereoCoreBridge.h 那个纯 ObjC 接口。
//
//  这一层的规矩：
//    * 任何 C++ 异常都不许越过这里（Swift 无法展开 C++ 异常）。
//      stereo.h 的全部函数都不抛异常、只返回错误码，所以这里也不需要 try/catch。
//    * 不碰 CoreVideo / Metal：像素怎么拿到是第三层（Swift）的事，
//      这里只认「指针 + 行跨度 + 格式」。
//

#import "StereoCoreBridge.h"

#include "stereo/stereo_c_api.h"

@implementation SVStereoCore {
    stereo_context *_ctx;  // C++ 侧的不透明句柄，唯一需要手工管理生命周期的东西
}

- (nullable instancetype)initWithMaxDisparity:(NSInteger)maxDisparity
                                           p1:(NSInteger)p1
                                           p2:(NSInteger)p2
                                   uniqueness:(float)uniqueness
                                      lrCheck:(BOOL)lrCheck {
    self = [super init];
    if (self == nil) return nil;

    _ctx = stereo_create();
    if (_ctx == NULL) return nil;

    stereo_config cfg;
    stereo_config_default(&cfg);
    cfg.max_disp = (int)maxDisparity;
    cfg.p1 = (int)p1;
    cfg.p2 = (int)p2;
    cfg.uniqueness = uniqueness;
    cfg.lr_check = lrCheck ? 1 : 0;

    if (stereo_configure(_ctx, &cfg) != 0) {
        _lastError = [NSString stringWithUTF8String:stereo_last_error(_ctx)];
        stereo_destroy(_ctx);
        _ctx = NULL;
        return nil;
    }

    _maxDisparity = maxDisparity;
    _backendName = @"C++ core (Census 7x7 + SGM 4 paths, scalar)";
    _lastError = @"";
    return self;
}

- (void)dealloc {
    if (_ctx != NULL) {
        stereo_destroy(_ctx);
        _ctx = NULL;
    }
    // ARC 下不要调用 [super dealloc]
}

- (BOOL)configureMaxDisparity:(NSInteger)maxDisparity
                           p1:(NSInteger)p1
                           p2:(NSInteger)p2
                   uniqueness:(float)uniqueness
                      lrCheck:(BOOL)lrCheck {
    if (_ctx == NULL) {
        _lastError = @"context 已经释放";
        return NO;
    }

    stereo_config cfg;
    stereo_config_default(&cfg);
    cfg.max_disp = (int)maxDisparity;
    cfg.p1 = (int)p1;
    cfg.p2 = (int)p2;
    cfg.uniqueness = uniqueness;
    cfg.lr_check = lrCheck ? 1 : 0;

    if (stereo_configure(_ctx, &cfg) != 0) {
        _lastError = [NSString stringWithUTF8String:stereo_last_error(_ctx)];
        return NO;
    }

    _maxDisparity = maxDisparity;
    _lastError = @"";
    return YES;
}

- (BOOL)matchLeft:(const uint8_t *)left
       leftStride:(NSInteger)leftStride
       leftFormat:(SVImageFormat)leftFormat
            right:(const uint8_t *)right
      rightStride:(NSInteger)rightStride
      rightFormat:(SVImageFormat)rightFormat
            width:(NSInteger)width
           height:(NSInteger)height
        direction:(NSInteger)direction
        disparity:(float *)disparity {
    if (_ctx == NULL) {
        _lastError = @"context 已经释放";
        return NO;
    }
    if (left == NULL || right == NULL || disparity == NULL) {
        _lastError = @"输入指针为空";
        return NO;
    }

    // 唯一一次「结构体翻译」：ObjC 的参数 → C 的 POD。
    stereo_image li;
    li.data = left;
    li.width = (int)width;
    li.height = (int)height;
    li.stride = (int)leftStride;
    li.format = (int)leftFormat;

    stereo_image ri;
    ri.data = right;
    ri.width = (int)width;
    ri.height = (int)height;
    ri.stride = (int)rightStride;
    ri.format = (int)rightFormat;

    if (stereo_match(_ctx, &li, &ri, (int)direction, disparity, (int)width) != 0) {
        _lastError = [NSString stringWithUTF8String:stereo_last_error(_ctx)];
        return NO;
    }

    _lastError = @"";
    return YES;
}

@end
