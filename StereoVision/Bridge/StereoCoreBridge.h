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
#import <simd/simd.h>

NS_ASSUME_NONNULL_BEGIN

/// 分析结果容器（定义在本文件后面）。这里先前置声明，因为 SVStereoCore 的方法要用它。
@class SVAnalysis;
@class SVCalibration;

/// 像素字节顺序，与 core/include/stereo/stereo_c_api.h 的 enum stereo_format 一一对应。
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

// ---------------------------------------------------------------------------
// 第 4 步：视差 → 深度，以及深度图回转
// ---------------------------------------------------------------------------

/// 视差 → 深度：3x3 中值（在视差域做）+ 范围截断，无效像素写 0。
///
/// fB 的单位是「像素·米」（= K.fx * baseline_m），所以输出 Z 是米。
/// disp/depth 的行跨度以**像素**为单位；depth 可以直接指向
/// `MTLBuffer` 背后的共享内存（按字节跨度换算），不需要额外拷贝。
///
/// API 2.00 起无状态（core 的 `stereo_disparity_to_depth` 不再需要 ctx）；
/// 要连回转一起做，用 `+processDisparity:...`（一次调用、与 process_frame 同一条实现）。
- (BOOL)depthFromDisparity:(const float *)disp
                dispStride:(NSInteger)dispStride
                     width:(NSInteger)width
                    height:(NSInteger)height
                        fB:(float)fB
                      minZ:(float)minZ
                      maxZ:(float)maxZ
                     depth:(float *)depth
               depthStride:(NSInteger)depthStride;

/// 视差 → 深度 → 回转（第 4 步的全部）：替代 `depthFromDisparity` + `unrectifyDepth`
/// 两次调用，与 core 的 `stereo_process_disparity` 对应。无状态，所以是类方法。
///
/// `depthRectScratch` 必填（width*height 个 float），由调用方持有：实时路径里不应该
/// 由库隐式分配。`depth` 可以直接指向共享 MTLBuffer（跨度以像素传）。
+ (BOOL)processDisparity:(const float *)disp
              dispStride:(NSInteger)dispStride
                   width:(NSInteger)width
                  height:(NSInteger)height
                      fB:(float)fB
                    minZ:(float)minZ
                    maxZ:(float)maxZ
               transform:(simd_float3x3)transform
                   depth:(float *)depth
             depthStride:(NSInteger)depthStride
         depthRectScratch:(float *)depthRectScratch;

/// 深度图回转：把**矫正后**坐标系的深度映回**宽相机**坐标系。
///
/// 与 Shaders.metal 的 unrectifyDepth 逐条一致（目标像素 +0.5 过单应、向下取整读源、
/// 越界写 0）。无状态，所以是类方法。
+ (BOOL)unrectifyDepth:(const float *)src
             srcStride:(NSInteger)srcStride
                 width:(NSInteger)width
                height:(NSInteger)height
             transform:(simd_float3x3)transform
                   dst:(float *)dst
             dstStride:(NSInteger)dstStride
               dstWidth:(NSInteger)dstWidth
              dstHeight:(NSInteger)dstHeight;

// ---------------------------------------------------------------------------
// 深度图查询与分析（准星读数 / 点选测距 / 3D segmentation）
// ---------------------------------------------------------------------------

/// 在归一化矩形里取 q 分位数。
///
/// API 2.00 起无状态（不需要 context），所以是**类方法**：
/// 返回 YES = 调用成功；`hasValue` 表示有效样本够不够（0 → UI 显示 "—"）。
/// 参数非法时返回 NO。
+ (BOOL)sampleDepth:(const float *)depth
             stride:(NSInteger)stride
              width:(NSInteger)width
             height:(NSInteger)height
               rect:(simd_float4)rect
                  q:(float)q
              value:(float *)value
           hasValue:(int *)hasValue;

/// 自动标定：候选序列 / 细化 / 比较在 C++（core/src/calibrate.cpp），
/// 每一次打分回到 `score` —— iOS 上就是 Metal 的 rectify + census + cost。
///
/// `score` 返回该候选的平均匹配代价（越小越对齐）；返回 INFINITY 表示这次打分失败。
/// 返回 nil 表示 54 次打分全部失败。
+ (nullable SVCalibration *)calibrate:(float (^)(BOOL flipped, float yOffset))score
                                flip0:(BOOL)flip0
                                  dy0:(float)dy0;

/// 连通域分析：降采样成网格 → 4 邻域连通域 → 保留最大的 maxBlobs 个。
///
/// 结果写进 `out`（复用同一个对象就不每帧分配）。返回 NO 表示参数/内存问题。
- (BOOL)analyzeDepth:(const float *)depth
              stride:(NSInteger)stride
               width:(NSInteger)width
              height:(NSInteger)height
          intrinsics:(simd_float4)intrinsics
          centerRect:(simd_float4)centerRect
             centerQ:(float)centerQ
            maxBlobs:(NSInteger)maxBlobs
            minCells:(NSInteger)minCells
                into:(SVAnalysis *)out;

@end

/// 自动标定的结果。
@interface SVCalibration : NSObject
/// YES = 基线方向反了（外参要取逆）。
@property (nonatomic, readonly) BOOL flipped;
/// 行偏移（工作分辨率像素，可亚像素）。
@property (nonatomic, readonly) float yOffset;
/// 胜出候选的平均匹配代价（越小越对齐）。
@property (nonatomic, readonly) float score;
@end

/// 一个深度面块。字段与 core 的 `stereo_blob` 一一对应（Bridge 里逐字段翻译）。
typedef struct SVBlobRecord {
    NSInteger identifier;   ///< 1..blobCount，按格子数从大到小
    NSInteger cells;        ///< 占多少网格格子
    float distance;         ///< 平均深度（米）
    float x;                ///< 面块中心在相机坐标系里的 X（米）
    float width;            ///< 物理宽度（米）
    float rectX, rectY, rectW, rectH;   ///< 归一化包围盒
} SVBlobRecord;

/// 分析结果的容器（由一个可复用的实例持有，见 `analyzeDepth:...into:`）。
///
/// `blobs` / `labels` 返回的是**借用指针**，活到下一次 `analyzeDepth:...into:`
/// 或对象释放为止。这样每帧不产生数组分配 —— 调用方在这一帧内读完就行。
@interface SVAnalysis : NSObject

@property (nonatomic, readonly) NSInteger blobCount;
@property (nonatomic, readonly) NSInteger gridWidth;
@property (nonatomic, readonly) NSInteger gridHeight;
@property (nonatomic, readonly) BOOL hasCenter;
@property (nonatomic, readonly) float center;

/// `SVBlobRecord[blobCount]`
- (const void *)blobs;
/// `gridWidth * gridHeight` 个字节；0 = 未保留（含被 minCells 过滤掉的块）
- (const void *)labels;

@end

// ---------------------------------------------------------------------------
// 导出用的像素运算（点云 PLY / 深度打包 / 裁剪 / 拷贝）
//
// 原来这四件事都在 Swift 里逐像素跑（拍一张 640×360 照片要格式化约 14 万行字符串）。
// 现在全是 C++，这里只做指针与 NSData 的搬运。全部无状态，所以是类方法。
// ---------------------------------------------------------------------------

@interface SVExporter : NSObject

/// 深度图 + 彩色图（BGRA8）→ ASCII PLY 点云（相机坐标，单位米）。参数非法返回 nil。
///
/// intrinsics = (fx, fy, cx, cy)，单位是**深度图**的像素。
+ (nullable NSData *)pointCloudPLY:(const float *)depth
                       depthStride:(NSInteger)depthStride
                             width:(NSInteger)width
                            height:(NSInteger)height
                        intrinsics:(simd_float4)intrinsics
                              rgba:(const uint8_t *)rgba
                        rgbaStride:(NSInteger)rgbaStride
                         rgbaWidth:(NSInteger)rgbaWidth
                        rgbaHeight:(NSInteger)rgbaHeight;

/// 深度 → 16 位毫米（写 depth_mm.png 用）。out 必须是 width*height 个 uint16。
+ (BOOL)depthToMillimetres:(const float *)depth
                    stride:(NSInteger)stride
                     width:(NSInteger)width
                    height:(NSInteger)height
                       out:(uint16_t *)out;

/// 深度裁剪：非有限值或超出 [minZ, maxZ] 的写 0。src/dst 允许不同跨度。
+ (BOOL)filterDepth:(const float *)src
           srcStride:(NSInteger)srcStride
                 dst:(float *)dst
           dstStride:(NSInteger)dstStride
               width:(NSInteger)width
              height:(NSInteger)height
                minZ:(float)minZ
                maxZ:(float)maxZ;

/// 对 `values[0..count)` 里 **>= 0** 的元素求平均（自动标定读回代价图时用）。
/// 没有这样的元素时返回 NaN。
///
/// 放在这一层是因为它只是"把 GPU 输出归约成标量"，不是算法；
/// 但它必须在这里，Swift 侧才不用写逐元素循环。
+ (float)meanNonNegative:(const float *)values count:(NSInteger)count;

/// 带跨度 → 紧凑的 float 拷贝（把 MTLBuffer 的行跨度拆掉）。
+ (BOOL)copyDepth:(const float *)src
        srcStride:(NSInteger)srcStride
            width:(NSInteger)width
           height:(NSInteger)height
              dst:(float *)dst;

@end

// ---------------------------------------------------------------------------
// 第 2 步：极线矫正（几何）
//
// Swift 不直接调 stereo_rectify_geometry：那个结构体里有 float[9] / float[12] / float[16]，
// Swift 会把它们导成巨大的元组，写起来全是 withUnsafePointer。这一层负责换乘 simd。
// ---------------------------------------------------------------------------

/// 极线矫正的输入：第 1 步（Calibration.swift）读到的内参 + 外参，原样搬进来。
@interface SVRectifyInput : NSObject

/// (fx, fy, cx, cy)，单位是**原始图像像素**。
@property (nonatomic) simd_float4 wideIntrinsics;
@property (nonatomic) simd_float4 ultraIntrinsics;

/// 外参：`X_wide = rotation * X_ultra + translation`，translation 单位毫米。
@property (nonatomic) simd_float3x3 rotation;
@property (nonatomic) simd_float3 translation;

/// 原始图尺寸（内参是按它给的，工作分辨率由 C++ 按比例缩放）。
/// `sourceHeight` 只在 `stereo_process_frame` 里用得到——那条路要把原始图的重采样交给 C++。
@property (nonatomic) float sourceWidth;
@property (nonatomic) float sourceHeight;
@property (nonatomic) NSInteger workWidth;
@property (nonatomic) NSInteger workHeight;
/// 交换基线方向（外参取逆）。
@property (nonatomic) BOOL flip;
/// 矫正后的行偏移（工作分辨率像素，可亚像素），自动标定会给出非零值。
@property (nonatomic) float yOffset;

@end

/// 极线矫正的结果。三种矩阵都是齐次像素坐标之间的单应（`p_dst ∝ M * p_src`）。
@interface SVRectification : NSObject

/// 解矫正。参数非法（内参/外参为零、尺寸非法）返回 nil。不做任何 GPU 工作。
+ (nullable SVRectification *)solve:(SVRectifyInput *)input;

/// 矫正后左图像素 → 宽相机原始像素（MW，给 Metal 的 rectify kernel）。
@property (nonatomic, readonly) simd_float3x3 rectifiedToWide;
/// 矫正后左图像素 → 超广角原始像素（MU）。
@property (nonatomic, readonly) simd_float3x3 rectifiedToUltra;
/// 宽相机像素（**工作分辨率、K 坐标系**）→ 矫正后图像素（MUp，深度回转用）。
@property (nonatomic, readonly) simd_float3x3 wideToRectified;

/// 矫正后的共同内参 (fx, fy, cx, cy)。
@property (nonatomic, readonly) simd_float4 intrinsics;
/// +1 / -1：同名点在左图的哪一侧。
@property (nonatomic, readonly) NSInteger direction;
/// fx * baseline(米)：直接传给 toDepth 的 fB。
@property (nonatomic, readonly) float focalBaseline;
@property (nonatomic, readonly) float baselineMM;
/// 基线相对宽相机 x 轴的角度（度）。对角双摄约 ±45°。
@property (nonatomic, readonly) float baselineDegrees;

@end


// ---------------------------------------------------------------------------
// 参数描述表（core/src/params.cpp 的那张表，界面据此生成滑杆）
// ---------------------------------------------------------------------------

/// 控件类型。命名避开 Swift 关键字（enum/switch 之类会被导入成反引号名字）。
typedef NS_ENUM(NSInteger, SVParamKind) {
    SVParamKindNumber  = 0,   ///< 连续浮点 → 滑杆
    SVParamKindInteger = 1,   ///< 整数 → 滑杆（step 通常是 1 或 32）
    SVParamKindFlag    = 2,   ///< 开关 → Toggle
    SVParamKindChoice  = 3,   ///< 枚举 → Picker（选项见 enumLabels）
};

/// 一条参数描述。数值与字符串直接来自 C++ 的参数表，Swift 侧不再各写一份。
@interface SVParameter : NSObject

@property (nonatomic, readonly) NSInteger identifier;
/// 稳定标识（"max_disparity"、"min_z"…）。界面用它读写 `SettingsDraft`。
@property (nonatomic, readonly, copy) NSString *key;
@property (nonatomic, readonly, copy) NSString *label;
/// 分组："depth" / "matching" / "display"
@property (nonatomic, readonly, copy) NSString *group;
@property (nonatomic, readonly) SVParamKind kind;
@property (nonatomic, readonly) float minValue;
@property (nonatomic, readonly) float maxValue;
@property (nonatomic, readonly) float step;
@property (nonatomic, readonly) float defaultValue;
@property (nonatomic, readonly, copy) NSString *unit;
/// kind == Choice 时的选项标签（已按 '|' 拆开）；其它类型是空数组。
@property (nonatomic, readonly, copy) NSArray<NSString *> *enumLabels;

@end

/// 参数表的访问入口（结果缓存，只在第一次调用时构造）。
@interface SVParameterTable : NSObject

/// 全部 11 条，顺序就是希望界面上出现的顺序。
+ (NSArray<SVParameter *> *)allParameters;

/// 按 key 查；找不到返回 nil。
+ (nullable SVParameter *)lookup:(NSString *)key;

@end

NS_ASSUME_NONNULL_END
