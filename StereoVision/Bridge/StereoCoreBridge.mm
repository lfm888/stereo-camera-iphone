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

namespace {

/// simd 是**列主序**（m.columns[c][r]），C++ 那边的 float[9] 是**行主序**（m[r*3+c]）。
/// 转换必须经过这一层，不能直接 memcpy —— 这俩的 bug 看起来永远“像对的”。
void copy_to_row_major(simd_float3x3 m, float out[9]) {
    for (int c = 0; c < 3; ++c) {
        for (int r = 0; r < 3; ++r) out[r * 3 + c] = m.columns[c][r];
    }
}

simd_float3x3 mat3_from_row_major(const float in[9]) {
    return simd_float3x3(simd_float3(in[0], in[3], in[6]),
                         simd_float3(in[1], in[4], in[7]),
                         simd_float3(in[2], in[5], in[8]));
}

stereo_intrinsics intrinsics_from(simd_float4 v) {
    stereo_intrinsics k;
    k.fx = v.x;
    k.fy = v.y;
    k.cx = v.z;
    k.cy = v.w;
    return k;
}

}  // namespace

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

// MARK: - 第 4 步：深度恢复 / 回转

- (BOOL)depthFromDisparity:(const float *)disp
                dispStride:(NSInteger)dispStride
                     width:(NSInteger)width
                    height:(NSInteger)height
                        fB:(float)fB
                      minZ:(float)minZ
                      maxZ:(float)maxZ
                     depth:(float *)depth
               depthStride:(NSInteger)depthStride {
    if (_ctx == NULL) {
        _lastError = @"context 已经释放";
        return NO;
    }
    if (stereo_disparity_to_depth(_ctx, disp, (int)dispStride, (int)width, (int)height, fB, minZ,
                                  maxZ, depth, (int)depthStride) != 0) {
        _lastError = [NSString stringWithUTF8String:stereo_last_error(_ctx)];
        return NO;
    }
    _lastError = @"";
    return YES;
}

+ (BOOL)unrectifyDepth:(const float *)src
             srcStride:(NSInteger)srcStride
                 width:(NSInteger)width
                height:(NSInteger)height
             transform:(simd_float3x3)transform
                   dst:(float *)dst
             dstStride:(NSInteger)dstStride
               dstWidth:(NSInteger)dstWidth
              dstHeight:(NSInteger)dstHeight {
    float m[9];
    copy_to_row_major(transform, m);
    return stereo_unrectify_depth(src, (int)srcStride, (int)width, (int)height, m, dst,
                                  (int)dstStride, (int)dstWidth, (int)dstHeight) == 0;
}

@end

// MARK: - 第 2 步：极线矫正

@implementation SVRectifyInput

- (instancetype)init {
    self = [super init];
    if (self == nil) return nil;
    _wideIntrinsics = simd_make_float4(1, 1, 0, 0);
    _ultraIntrinsics = simd_make_float4(1, 1, 0, 0);
    _rotation = matrix_identity_float3x3;
    _translation = simd_make_float3(0, 0, 0);
    _sourceWidth = 0;
    _workWidth = 0;
    _workHeight = 0;
    _flip = NO;
    _yOffset = 0;
    return self;
}

@end

@implementation SVRectification

+ (nullable SVRectification *)solve:(SVRectifyInput *)input {
    if (input == nil) return nil;

    stereo_rectify_input in{};
    in.wide = intrinsics_from(input.wideIntrinsics);
    in.ultra = intrinsics_from(input.ultraIntrinsics);
    copy_to_row_major(input.rotation, in.R);
    in.t[0] = input.translation.x;
    in.t[1] = input.translation.y;
    in.t[2] = input.translation.z;
    in.src_width = (int)input.sourceWidth;
    in.work_width = (int)input.workWidth;
    in.work_height = (int)input.workHeight;
    in.flip = input.flip ? 1 : 0;
    in.dy = input.yOffset;

    stereo_rectify_output out{};
    if (stereo_rectify_geometry(&in, &out) != 0) return nil;

    SVRectification *r = [[SVRectification alloc] init];
    r->_rectifiedToWide = mat3_from_row_major(out.to_wide_image);
    r->_rectifiedToUltra = mat3_from_row_major(out.to_ultra_image);
    r->_wideToRectified = mat3_from_row_major(out.to_rect_image);
    r->_intrinsics = simd_make_float4(out.k.fx, out.k.fy, out.k.cx, out.k.cy);
    r->_direction = out.dir;
    r->_focalBaseline = out.focal_baseline;
    r->_baselineMM = out.baseline_mm;
    r->_baselineDegrees = out.baseline_deg;
    return r;
}

@end
