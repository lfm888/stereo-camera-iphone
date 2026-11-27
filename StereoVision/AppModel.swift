import AVFoundation
import CoreVideo
import Metal
import Observation
import QuartzCore
import UIKit
import os

enum ViewMode: Int, CaseIterable, Identifiable {
    case standard, depth, overlay, disparity, stereoPair, anaglyph, segmentation, pointCloud
    var id: Self { self }

    var title: String {
        switch self {
        case .standard: "Standard"
        case .depth: "Depth map"
        case .overlay: "Overlay"
        case .disparity: "Disparity"
        case .stereoPair: "Rectified pair"
        case .anaglyph: "Anaglyph"
        case .segmentation: "3D segmentation"
        case .pointCloud: "Point cloud"
        }
    }

    var icon: String {
        switch self {
        case .standard: "camera"
        case .depth: "flame"
        case .overlay: "square.2.layers.3d"
        case .disparity: "circle.lefthalf.filled"
        case .stereoPair: "rectangle.split.2x1"
        case .anaglyph: "eyeglasses"
        case .segmentation: "square.grid.3x3.fill"
        case .pointCloud: "cube.transparent"
        }
    }

    var needsStereoPair: Bool { self == .stereoPair || self == .anaglyph }

    /// 显式映射，不用 rawValue：删过枚举项之后 rawValue 会前移，而着色器里的
    /// mode 是固定编号（Shaders.metal 的 displayFragment）。之前用
    /// `min(rawValue, 7)` 靠"Bird's-eye 恰好是 7"凑数，删掉它就错位了。
    var shaderIndex: Int32 {
        switch self {
        case .standard: 0
        case .depth: 1
        case .overlay: 2
        case .disparity: 3
        case .stereoPair: 4
        case .anaglyph: 5
        case .segmentation: 6
        case .pointCloud: 7   // 不走 displayFragment，Renderer 里单独分支
        }
    }
}

enum Matcher: String, CaseIterable, Identifiable, Sendable {
    case sgm, block
    var id: Self { self }
    var title: String { self == .sgm ? "SGM 4 paths" : "Block matching 5×5" }
}

/// 显示方向：手机怎么拿，屏幕上就把画面转多少。
///
/// 采集和匹配永远跑在传感器的横屏坐标系里（buffer 640×360），所以转屏不会重开 session、
/// 不丢帧、不用重新标定，深度/掩码/外参也都不变；只有“怎么显示”在变：
///   0 landscapeRight     buffer 原样贴屏（原来的唯一姿势）
///   1 portrait           内容顺时针转 90°
///   2 landscapeLeft      180°
///   3 portraitUpsideDown 270°
enum DisplayOrientation: Int, Sendable, CaseIterable {
    case landscapeRight = 0, portrait = 1, landscapeLeft = 2, portraitUpsideDown = 3

    var isPortrait: Bool { self == .portrait || self == .portraitUpsideDown }

    /// 反方向：用来把屏幕坐标系的东西转回相机 buffer 坐标系。
    var inverse: DisplayOrientation {
        switch self {
        case .landscapeRight: .landscapeRight
        case .portrait: .portraitUpsideDown
        case .landscapeLeft: .landscapeLeft
        case .portraitUpsideDown: .portrait
        }
    }

    var key: String {
        switch self {
        case .landscapeRight: "landscape_right"
        case .portrait: "portrait"
        case .landscapeLeft: "landscape_left"
        case .portraitUpsideDown: "portrait_upside_down"
        }
    }

    var label: String {
        switch self {
        case .landscapeRight: "landscape →"
        case .portrait: "portrait"
        case .landscapeLeft: "← landscape"
        case .portraitUpsideDown: "portrait ⇅"
        }
    }

    /// 着色器里要把屏幕 uv 换成 buffer uv，这跟这里的 screen/buffer 是同一对互逆变换。
    func screen(_ p: CGPoint) -> CGPoint {
        switch self {
        case .landscapeRight: CGPoint(x: p.x, y: p.y)
        case .portrait: CGPoint(x: 1 - p.y, y: p.x)
        case .landscapeLeft: CGPoint(x: 1 - p.x, y: 1 - p.y)
        case .portraitUpsideDown: CGPoint(x: p.y, y: 1 - p.x)
        }
    }

    func buffer(_ p: CGPoint) -> CGPoint {
        switch self {
        case .landscapeRight: CGPoint(x: p.x, y: p.y)
        case .portrait: CGPoint(x: p.y, y: 1 - p.x)
        case .landscapeLeft: CGPoint(x: 1 - p.x, y: 1 - p.y)
        case .portraitUpsideDown: CGPoint(x: 1 - p.y, y: p.x)
        }
    }

    func screen(_ r: CGRect) -> CGRect {
        let a = screen(CGPoint(x: r.minX, y: r.minY)), b = screen(CGPoint(x: r.maxX, y: r.maxY))
        return CGRect(x: min(a.x, b.x), y: min(a.y, b.y), width: abs(a.x - b.x), height: abs(a.y - b.y))
    }

    func buffer(_ r: CGRect) -> CGRect {
        let a = buffer(CGPoint(x: r.minX, y: r.minY)), b = buffer(CGPoint(x: r.maxX, y: r.maxY))
        return CGRect(x: min(a.x, b.x), y: min(a.y, b.y), width: abs(a.x - b.x), height: abs(a.y - b.y))
    }
}

struct PipelineSettings: Sendable {
    var matcher = Matcher.sgm
    var lrCheck = true
    var p1 = 8
    var p2 = 96
    var minZ: Float = 0.12
    var maxZ: Float = 3
    var uniqueness: Float = 0.1
    var maxDisparity = 64
    var yOffset: Float = 0
    var flip = false
    var orient = DisplayOrientation.landscapeRight
    var calibrate = true
    /// true = 匹配交给 C++ 核心（CPU）；false = 走 Metal 的 SGM。
    /// 由 AppModel 依据 DepthSource 推入，不放进设置页的 picker。
    var cpuMatcher = false
}

struct FrameReport: Sendable {
    var analysis: Analysis
    var fps: Double
    var gpuMs: Double
    /// CPU 一段的总和（读回 + 匹配 + 深度）；Metal 路径为 0。
    var cpuMs: Double = 0
    var matcherName: String = "—"
    var stages: [StageDiag] = []
    var baselineMM: Float
    var baselineDeg: Float
    var hasExtrinsics: Bool
    var K: Intrinsics
    var depthSize: CGSize
    var aspect: CGFloat
    var calibration: (flip: Bool, dy: Float)?
}

/// 这一帧走的是哪条管线（决定诊断页上那张表）。
enum PipelineKind {
    case metalStereo
    case cpuStereo
    case systemDepth
}

// MARK: - 界面与状态之间的契约
//
// 三个界面只认这几样东西：
//   CaptureUIState / DiagnosticsSnapshot / DeviceInfo  —— 只读入参（值类型，界面改不了状态）
//   CaptureIntents / SettingsIntents                    —— 唯一可写入口
// 于是“诊断页不会偷改状态”是编译器保证的，不靠约定。

/// 设置页编辑的**引擎参数草稿**。
///
/// 改动在点 Apply 之前不进管线：换 maxDisparity 会让 C++ context 重新分配缓冲区，
/// 所以不能拖一下滑杆就生效。
struct SettingsDraft {
    var minZ: Float = 0.12
    var maxZ: Float = 3
    var uniqueness: Float = 0.1
    var maxDisparity = 64
    var matcher = Matcher.sgm
    var lrCheck = true
    var p1: Float = 8
    var p2: Float = 96
    var yOffset: Float = 0
    var flip = false
}

/// 只读的设备/标定信息。
struct DeviceInfo {
    var source: DepthSource
    var isMultiCam: Bool
    var baselineMM: Float
    var baselineDeg: Float
    var intrinsics: Intrinsics
    var depthSize: CGSize
}

/// 主界面的只读状态。
struct CaptureUIState {
    var mode: ViewMode
    var source: DepthSource
    var aspect: CGFloat
    var orientation: DisplayOrientation
    var fps: Double
    var device: DeviceInfo
    var hasExtrinsics: Bool
    var calibrating: Bool
    var status: String?
    var draft: SettingsDraft
    var overlayAlpha: Float
    var yaw: Float
    var pitch: Float
    var orbit: Float
    var gpuSummary: String
    var analysis: Analysis
    var tap: CGPoint?
    var tapDistance: Float?
    var isRecording: Bool
    var recordingStart: Date?
    var captureMessage: String?
}

/// 主拍摄界面能做的事。
@MainActor
protocol CaptureIntents {
    func startCapture() async
    func selectMode(_ mode: ViewMode)
    func selectNextSource()
    func autoCalibrate()
    func takePhoto()
    func toggleRecording()
    func measureTap(at normalized: CGPoint)
    func setOrbit(yaw: Float, pitch: Float)
    func setOrbitDistance(_ orbit: Float)
    func resetOrbit()
}

/// 设置界面能做的事。
@MainActor
protocol SettingsIntents {
    var draft: SettingsDraft { get }
    var device: DeviceInfo { get }
    var overlayAlpha: Float { get }
    func apply(_ draft: SettingsDraft)
    func setOverlayAlpha(_ value: Float)
    func setSource(_ source: DepthSource)
    func autoCalibrate()
    func runCoreSelfTest()
}

final class FrameStore: @unchecked Sendable {
    private let lock = NSLock()
    private var frame: StereoFrame?
    var latest: StereoFrame? {
        get { lock.withLock { frame } }
        set { lock.withLock { frame = newValue } }
    }
}

final class Pipeline: @unchecked Sendable {
    let engine: StereoEngine
    let store = FrameStore()
    let settings = OSAllocatedUnfairLock(initialState: PipelineSettings())
    var publish: ((FrameReport) -> Void)?
    private let analyzer = Analyzer()
    private var stamps: [Double] = []

    init(engine: StereoEngine) { self.engine = engine }

    func stereo(_ wide: CMSampleBuffer, _ ultra: CMSampleBuffer) {
        var s = settings.withLock { st in
            defer { st.calibrate = false }
            return st
        }
        var cal: (flip: Bool, dy: Float)?
        if s.calibrate {
            let c = engine.calibrate(wide, ultra, s)
            s.flip = c.flip
            s.yOffset = c.dy
            settings.withLock { $0.flip = c.flip; $0.yOffset = c.dy }
            cal = c
        }
        guard let f = s.cpuMatcher ? engine.stereoCPU(wide, ultra, s) : engine.stereo(wide, ultra, s) else { return }
        finish(f, cal, s.cpuMatcher ? .cpuStereo : .metalStereo)
    }

    func depthData(_ video: CMSampleBuffer, _ depth: AVDepthData) {
        let s = settings.withLock { $0 }
        guard let f = engine.depthData(video, depth, s) else { return }
        finish(f, nil, .systemDepth)
    }

    /// 哪个阶段在 GPU / CPU。这张表是诊断页的数据源。
    private static func diagnostics(_ kind: PipelineKind, _ f: StereoFrame) -> (String, [StageDiag]) {
        switch kind {
        case .metalStereo:
            return ("Metal SGM-4（全 GPU）", [
                StageDiag(id: 0, name: "rectify → census → costVolume → sgmPath×4 → wta → toDepth → unrectify",
                          device: .gpu, ms: f.gpuMs, note: "整帧一条 command buffer，未细分"),
                StageDiag(id: 1, name: "显示 / 点云渲染", device: .gpu, ms: nil, note: "MTKView，每帧一次"),
            ])
        case .cpuStereo:
            return ("C++ core（CPU 匹配 + CPU 深度）", [
                StageDiag(id: 0, name: "rectify（两路单应重采样）",
                          device: .gpu, ms: f.gpuRectifyMs, note: "Metal kernel"),
                StageDiag(id: 1, name: "像素读回",
                          device: .cpu, ms: f.readbackMs,
                          note: "CVPixelBuffer 锁定 + MTLTexture.getBytes（右图 0.9 MB）+ 每帧两次临时分配"),
                StageDiag(id: 2, name: "census → costVolume → sgmPath×4 → wta",
                          device: .cpu, ms: f.matchMs, note: "C++ 匹配：core/src/sgm.cpp"),
                StageDiag(id: 3, name: "toDepth → unrectify",
                          device: .cpu, ms: f.depthMs,
                          note: "C++ 深度：core/src/sgm.cpp + core/src/rectify.cpp，直接写进共享 MTLBuffer"),
                StageDiag(id: 4, name: "显示 / 点云渲染", device: .gpu, ms: nil, note: "MTKView，每帧一次"),
            ])
        case .systemDepth:
            return ("系统深度（Apple / LiDAR）", [
                StageDiag(id: 0, name: "系统深度图", device: .system, ms: nil, note: "由系统计算，App 拿不到耗时"),
                StageDiag(id: 1, name: "显示 / 点云渲染", device: .gpu, ms: nil, note: "MTKView"),
            ])
        }
    }

    private func finish(_ f: StereoFrame, _ cal: (flip: Bool, dy: Float)?, _ kind: PipelineKind) {
        store.latest = f
        let now = CACurrentMediaTime()
        stamps.append(now)
        stamps.removeAll { now - $0 > 1 }
        // 深度连通域分析在 C++（core/src/analyze.cpp），这里只拿结果。
        let a = analyzer.analyze(f, using: engine.core)
        let (name, stages) = Self.diagnostics(kind, f)
        publish?(FrameReport(analysis: a, fps: Double(stamps.count), gpuMs: f.gpuMs, cpuMs: f.cpuMs,
                             matcherName: name, stages: stages,
                             baselineMM: f.baselineMM, baselineDeg: f.baselineDeg,
                             hasExtrinsics: engine.hasExtrinsics, K: f.K,
                             depthSize: CGSize(width: f.width, height: f.height),
                             aspect: CGFloat(f.color.width) / CGFloat(f.color.height), calibration: cal))
    }
}

@MainActor
@Observable
final class AppModel {
    private(set) var source: DepthSource = .stereo
    private(set) var mode: ViewMode = .standard

    // ---- 引擎参数：只能经由 apply(_ draft:) 或自动标定修改 ----
    // 不再用 didSet 逐字段 push —— 11 个字段要是一次提交就只推一次，
    // 否则 Settings 拖一下会往管线里塞 11 次。
    private(set) var minZ: Float = 0.12
    private(set) var maxZ: Float = 3
    private(set) var uniqueness: Float = 0.1
    private(set) var maxDisparity = 64
    private(set) var matcher = Matcher.sgm
    private(set) var lrCheck = true
    private(set) var p1: Float = 8
    private(set) var p2: Float = 96
    private(set) var yOffset: Float = 0
    private(set) var flip = false

    // ---- 纯显示状态：即时生效，不进管线 ----
    private(set) var overlayAlpha: Float = 0.55
    private(set) var yaw: Float = 0
    private(set) var pitch: Float = 0
    private(set) var orbit: Float = 1.2
    private(set) var calibrating = false

    /// HUD 用的一行摘要：GPU / CPU 各占多少。
    var gpuStagesSummary: String {
        if gpuReport.cpuMs > 0 {
            return String(format: "GPU %.1f ms · CPU %.1f ms", gpuReport.gpuMs, gpuReport.cpuMs)
        }
        return String(format: "GPU %.1f ms", gpuReport.gpuMs)
    }

    private(set) var analysis = Analysis()
    private(set) var fps = 0.0
    private(set) var gpuMs = 0.0
    private(set) var gpuReport = GpuReport()
    private(set) var baselineMM: Float = 0
    private(set) var baselineDeg: Float = 0
    private(set) var hasExtrinsics = true
    private(set) var K = Intrinsics(fx: 1, fy: 1, cx: 0, cy: 0)
    private(set) var depthSize = CGSize(width: 1, height: 1)
    /// 相机画面自身的宽高比（永远是横屏的 16:9）。
    private(set) var frameAspect: CGFloat = 16 / 9
    /// 屏幕方向：手机怎么拿就转多少（见 DisplayOrientation）。
    private(set) var orientation = DisplayOrientation.landscapeRight
    /// 给视图用的宽高比：竖屏时是 frameAspect 的倒数，画面才不会被拉伸。
    var aspect: CGFloat { orientation.isPortrait ? 1 / frameAspect : frameAspect }
    private(set) var status: String?
    private(set) var tap: CGPoint?
    private(set) var tapDistance: Float?

    // MARK: - 给界面的只读视图（界面不再直接拿 AppModel 读一堆字段）

    /// 当前的引擎参数，作为设置页草稿的初值。
    var draft: SettingsDraft {
        var d = SettingsDraft()
        d.minZ = minZ
        d.maxZ = maxZ
        d.uniqueness = uniqueness
        d.maxDisparity = maxDisparity
        d.matcher = matcher
        d.lrCheck = lrCheck
        d.p1 = p1
        d.p2 = p2
        d.yOffset = yOffset
        d.flip = flip
        return d
    }

    /// 只读的设备/标定信息。
    var device: DeviceInfo {
        DeviceInfo(source: source, isMultiCam: source.isMultiCam, baselineMM: baselineMM,
                   baselineDeg: baselineDeg, intrinsics: K, depthSize: depthSize)
    }

    /// 主界面的只读状态（HUD / 侧栏 / 叠层全从它渲染）。
    var ui: CaptureUIState {
        CaptureUIState(mode: mode, source: source, aspect: aspect, orientation: orientation,
                       fps: fps, device: device, hasExtrinsics: hasExtrinsics,
                       calibrating: calibrating, status: status, draft: draft,
                       overlayAlpha: overlayAlpha, yaw: yaw, pitch: pitch, orbit: orbit,
                       gpuSummary: gpuStagesSummary, analysis: analysis,
                       tap: tap, tapDistance: tapDistance,
                       isRecording: capture.isRecording, recordingStart: capture.recordingStart,
                       captureMessage: capture.message)
    }

    /// 诊断页的只读快照。
    var diagnostics: DiagnosticsSnapshot {
        DiagnosticsSnapshot(sourceTitle: source.title, matcherName: gpuReport.matcherName,
                            fps: fps, gpuMs: gpuReport.gpuMs, cpuMs: gpuReport.cpuMs,
                            allocatedMB: gpuReport.allocatedMB, workingSetMB: gpuReport.workingSetMB,
                            depthSize: depthSize, stages: gpuReport.stages)
    }

    @ObservationIgnored let pipeline: Pipeline
    @ObservationIgnored let renderer: Renderer
    /// 主线程专用的 C++ context：点选测距在主线程跑，而 pipeline 的 context 在相机数据队列上跑。
    /// 两个分开是为了不并发访问同一个 stereo_context（它不是线程安全的）。
    @ObservationIgnored private let queryCore = SVStereoCore(maxDisparity: 64, p1: 8, p2: 96,
                                                           uniqueness: 0.1, lrCheck: true)
    @ObservationIgnored private var orientationObservers: [NSObjectProtocol] = []
    let capture = Capture()
    @ObservationIgnored private let camera = CameraController()

    init() {
        let device = MTLCreateSystemDefaultDevice()!
        let engine = StereoEngine(device: device)
        pipeline = Pipeline(engine: engine)
        renderer = Renderer(device: device, store: pipeline.store)
        renderer.model = self
        capture.model = self

        let p = pipeline
        camera.onStereo = { p.stereo($0, $1) }
        camera.onDepth = { p.depthData($0, $1) }
        camera.onDevices = { engine.configure(wide: $0, ultra: $1) }
        camera.onError = { [weak self] msg in DispatchQueue.main.async { self?.status = msg } }
        pipeline.publish = { [weak self] r in DispatchQueue.main.async { self?.apply(r) } }
        watchOrientation()
    }

    /// 用 UIWindowScene.interfaceOrientation 而不是 UIDevice.orientation：前者等于是“界面真正转到哪边了”，
    /// 系统方向锁打开时它不会变，与屏幕上看到的一致。UIDevice 的通知只用来触发重读。
    private func watchOrientation() {
        UIDevice.current.beginGeneratingDeviceOrientationNotifications()
        let c = NotificationCenter.default
        for name in [UIDevice.orientationDidChangeNotification,
                     UIApplication.didBecomeActiveNotification,
                     UIApplication.willEnterForegroundNotification] {
            orientationObservers.append(c.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated { self?.refreshOrientation() }
            })
        }
        refreshOrientation()
    }

    func refreshOrientation() {
        let scenes = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
        guard let scene = scenes.first(where: { $0.activationState == .foregroundActive }) ?? scenes.first else { return }
        let o: DisplayOrientation
        switch scene.interfaceOrientation {
        case .portrait: o = .portrait
        case .portraitUpsideDown: o = .portraitUpsideDown
        case .landscapeLeft: o = .landscapeLeft
        case .landscapeRight: o = .landscapeRight
        default: return
        }
        guard o != orientation else { return }
        orientation = o
        push { $0.orient = o }
    }

    func start() async {
        guard await AVCaptureDevice.requestAccess(for: .video) else {
            status = "Camera access denied: enable it in Settings"
            return
        }
        refreshOrientation()          // 启动时界面方向可能已经定了，补读一次
        UIApplication.shared.isIdleTimerDisabled = true
        setSource(source)
    }

    func setSource(_ s: DepthSource) {
        source = s
        status = nil
        push { $0.cpuMatcher = (s == .cpuCore) }
        if !s.providesRectifiedPair && mode.needsStereoPair { mode = .standard }
        pipeline.store.latest = nil
        analysis = Analysis()
        if s.isMultiCam { calibrate() }
        camera.start(s)
    }

    func calibrate() {
        calibrating = true
        push { $0.calibrate = true }
    }

    func measure(at p: CGPoint) {
        if tap.map({ hypot($0.x - p.x, $0.y - p.y) < 0.03 }) == true {
            tap = nil
            tapDistance = nil
            return
        }
        tap = p
        tapDistance = sampleTap()
    }
    func metadata() -> [String: Any] {
        [
            "app": "StereoVision",
            "date": ISO8601DateFormatter().string(from: .now),
            "source": source.rawValue,
            "mode": mode.title,
            "matcher": source.isMultiCam ? (source == .cpuCore ? "c++ core (cpu)" : matcher.rawValue) : "n/a",
            "max_disparity": maxDisparity,
            "lr_check": lrCheck,
            "p1": Int(p1), "p2": Int(p2),
            "uniqueness": uniqueness,
            "range_m": [minZ, maxZ],
            "rectification": ["flip": flip, "dy_px": yOffset, "baseline_angle_deg": baselineDeg, "camera_extrinsics": hasExtrinsics],
            "orientation": orientation.key,
        ]
    }

    // MARK: - 三层框架自检（Swift → ObjC++ → C API → C++）

    /// 在最新一帧**已矫正**的左右图上跑一次 CPU SGM，把耗时与有效率写进 HUD。
    ///
    /// 这条路径经过三层：
    ///   Swift（这里，负责从 CVPixelBuffer / MTLTexture 取像素）
    ///     → SVStereoCore（Bridge/StereoCoreBridge.mm，负责生命周期与格式）
    ///     → stereo_match（core/include/stereo/stereo.h 的纯 C 接口）
    ///     → C++ 算法（core/src/sgm.cpp）
    ///
    /// 真接成第四个深度来源时，应该把 SVStereoCore 挂到 Pipeline 上复用
    /// （它的内部缓冲区按尺寸复用，第一帧之后零分配）。
    func runCoreSelfTest() {
        status = "C++ core: 正在跑…"
        let frame = pipeline.store.latest
        // 两段任务：后台跑算法（不捕获 self），回到 MainActor 再写 status。
        // 这样不会出现 Swift 6 的 “reference to captured var 'self' in
        // concurrently-executing code” 错误。
        Task { @MainActor in
            let msg = await Task.detached(priority: .userInitiated) {
                Self.coreSelfTest(frame)
            }.value
            status = msg
        }
    }

    nonisolated private static func coreSelfTest(_ f: StereoFrame?) -> String {
        guard let f, let pb = f.pairLeftPB, let right = f.right else { return "C++ core: 还没有可用帧" }
        guard let core = SVStereoCore(maxDisparity: 64, p1: 8, p2: 96, uniqueness: 0.1, lrCheck: true) else {
            return "C++ core: 初始化失败"
        }

        let w = f.width, h = f.height
        CVPixelBufferLockBaseAddress(pb, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(pb, .readOnly) }
        guard let base = CVPixelBufferGetBaseAddress(pb) else { return "C++ core: 拿不到左图" }

        // 左图是 32BGRA 的 CVPixelBuffer（rectify 用 bgra8Unorm 纹理写进去的），
        // 右图是 .rgba8Unorm 的共享纹理 —— 两幅图的字节序不同，必须分开声明格式。
        let leftStride = CVPixelBufferGetBytesPerRow(pb)
        let rightStride = w * 4
        let rightBytes = UnsafeMutablePointer<UInt8>.allocate(capacity: rightStride * h)
        defer { rightBytes.deallocate() }
        right.getBytes(rightBytes, bytesPerRow: rightStride,
                       from: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0)

        let disp = UnsafeMutablePointer<Float>.allocate(capacity: w * h)
        defer { disp.deallocate() }

        let t0 = CACurrentMediaTime()
        let ok = core.matchLeft(base.assumingMemoryBound(to: UInt8.self),
                                leftStride: leftStride, leftFormat: .bgra8,
                                right: UnsafePointer(rightBytes),
                                rightStride: rightStride, rightFormat: .rgba8,
                                width: w, height: h, direction: Int(f.dir),
                                disparity: disp)
        let ms = (CACurrentMediaTime() - t0) * 1000
        guard ok else { return "C++ core: \(core.lastError)" }

        var valid = 0
        for i in 0 ..< (w * h) where disp[i] > 0 { valid += 1 }
        return String(format: "C++ core: %.1f ms · 有效 %.1f%% · %d×%d D=%d",
                      ms, 100.0 * Double(valid) / Double(w * h), w, h, core.maxDisparity)
    }

    func resetOrbit() {
        yaw = 0
        pitch = 0
        orbit = 1.2
    }

    /// 导出（拍照 / 录像 / 叠加画面）的像素尺寸：长边跟着屏幕的长边走。
    func outputSize(long: Int) -> (w: Int, h: Int) {
        let l = max(2, long & ~1)
        let s = max(2, Int((Double(l) / Double(frameAspect)).rounded()) & ~1)
        return orientation.isPortrait ? (s, l) : (l, s)
    }

    private func sampleTap() -> Float? {
        guard let tap, let f = pipeline.store.latest, let core = queryCore else { return nil }
        let p = orientation.buffer(tap)          // 屏幕坐标 -> 相机 buffer 坐标（深度图在 buffer 坐标系里）
        var v: Float = 0
        let ok = core.sampleDepth(f.depthPointer,
                                  stride: f.depthStride,
                                  width: f.width,
                                  height: f.height,
                                  rect: SIMD4(Float(p.x - 0.012), Float(p.y - 0.02),
                                              Float(p.x + 0.012), Float(p.y + 0.02)),
                                  q: 0.5,
                                  value: &v)
        return ok ? v : nil
    }

    private func push(_ change: (inout PipelineSettings) -> Void) {
        pipeline.settings.withLockUnchecked { change(&$0) }
    }

    private func apply(_ r: FrameReport) {
        analysis = r.analysis
        fps = r.fps
        gpuMs = r.gpuMs
        let dev = renderer.device
        gpuReport = GpuReport(stages: r.stages, matcherName: r.matcherName,
                              gpuMs: r.gpuMs, cpuMs: r.cpuMs,
                              allocatedMB: Double(dev.currentAllocatedSize) / 1_048_576.0,
                              workingSetMB: Double(dev.recommendedMaxWorkingSetSize) / 1_048_576.0)
        baselineMM = r.baselineMM
        baselineDeg = r.baselineDeg
        hasExtrinsics = r.hasExtrinsics
        K = r.K
        depthSize = r.depthSize
        if abs(frameAspect - r.aspect) > 0.01 { frameAspect = r.aspect }
        if let c = r.calibration {
            flip = c.flip
            yOffset = c.dy
            calibrating = false
            // 自动标定的结果也要进管线。以前靠 didSet 推送，现在集中在这里。
            push { $0.flip = c.flip; $0.yOffset = c.dy }
        }
        if tap != nil { tapDistance = sampleTap() }
    }
}

// MARK: - 意图实现（界面唯一可写入口）

extension AppModel: CaptureIntents {
    func startCapture() async { await start() }

    func selectMode(_ mode: ViewMode) { self.mode = mode }

    func selectNextSource() {
        let all = DepthSource.available
        guard !all.isEmpty else { return }
        setSource(all[((all.firstIndex(of: source) ?? 0) + 1) % all.count])
    }

    func autoCalibrate() { calibrate() }

    func takePhoto() { capture.takePhoto() }

    func toggleRecording() { capture.toggleRecording() }

    func measureTap(at normalized: CGPoint) { measure(at: normalized) }

    func setOrbit(yaw: Float, pitch: Float) {
        self.yaw = yaw
        self.pitch = min(1.4, max(-1.4, pitch))
    }

    func setOrbitDistance(_ orbit: Float) {
        self.orbit = min(8, max(0.2, orbit))
    }
}

extension AppModel: SettingsIntents {
    /// 草稿一次性提交：10 个字段只往管线里推一遍。
    func apply(_ d: SettingsDraft) {
        minZ = d.minZ
        maxZ = d.maxZ
        uniqueness = d.uniqueness
        maxDisparity = d.maxDisparity
        matcher = d.matcher
        lrCheck = d.lrCheck
        p1 = d.p1
        p2 = d.p2
        yOffset = d.yOffset
        flip = d.flip
        push {
            $0.minZ = d.minZ
            $0.maxZ = d.maxZ
            $0.uniqueness = d.uniqueness
            $0.maxDisparity = d.maxDisparity
            $0.matcher = d.matcher
            $0.lrCheck = d.lrCheck
            $0.p1 = Int(d.p1)
            $0.p2 = Int(d.p2)
            $0.yOffset = d.yOffset
            $0.flip = d.flip
        }
    }

    func setOverlayAlpha(_ value: Float) {
        overlayAlpha = min(1, max(0, value))
    }
}
