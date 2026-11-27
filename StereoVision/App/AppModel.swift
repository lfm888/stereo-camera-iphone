import AVFoundation
import CoreVideo
import Metal
import Observation
import UIKit
import os

// 状态聚合 + 接线 + 意图实现。本文件只做三件事：
//   1. 持有界面看得到的全部状态（@Observable，主线程）
//   2. 把相机 / 管线 / 渲染器连起来
//   3. 实现 CaptureIntents / SettingsIntents（界面唯一可写入口）
//
// 类型定义分在 Contracts/：
//   Contracts/UIContracts.swift       ViewMode 以外的界面快照 + 意图（CaptureUIState / SettingsDraft / …）
//   Contracts/PipelineContracts.swift 共享词汇 + 管线数据结构（PipelineSettings / FrameReport / FrameStore）
// 每帧在后台跑的调度类在 App/FramePipeline.swift。

@MainActor
@Observable
final class AppModel {
    private(set) var source: DepthSource = .stereo
    private(set) var mode: ViewMode = .standard

    // ---- 引擎参数：只能经由 apply(_ draft:) 或自动标定修改 ----
    // 初值在 init 里从 **C++ 参数表**取的 SettingsDraft() 灌进来，这里不再写字面量。
    // 也不再逐个 didSet→push：一次提交只推一次。
    private(set) var minZ: Float
    private(set) var maxZ: Float
    private(set) var uniqueness: Float
    private(set) var maxDisparity: Int
    private(set) var matcher: Matcher
    private(set) var lrCheck: Bool
    private(set) var p1: Float
    private(set) var p2: Float
    private(set) var yOffset: Float
    private(set) var flip: Bool

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

    /// 诊断页的只读快照（只有数字和枚举；文案由 DiagnosticsScreen 生成）。
    var diagnostics: DiagnosticsSnapshot {
        DiagnosticsSnapshot(sourceTitle: source.title, kind: gpuReport.kind,
                            fps: fps, timings: gpuReport.timings,
                            allocatedMB: gpuReport.allocatedMB, workingSetMB: gpuReport.workingSetMB,
                            depthSize: depthSize)
    }

    @ObservationIgnored let pipeline: FramePipeline
    @ObservationIgnored let renderer: MetalSceneRenderer
    /// 主线程专用的 C++ context：点选测距在主线程跑，而 pipeline 的 context 在相机数据队列上跑。
    /// 两个分开是为了不并发访问同一个 stereo_context（它不是线程安全的）。
    @ObservationIgnored private let queryCore = SVStereoCore(maxDisparity: 64, p1: 8, p2: 96,
                                                           uniqueness: 0.1, lrCheck: true)
    @ObservationIgnored private var orientationObservers: [NSObjectProtocol] = []
    let capture = MediaExportManager()
    @ObservationIgnored private let camera = CameraSessionController()

    init() {
        // 引擎参数的初值来自 C++ 的参数表（与设置页同一份）
        let initial = SettingsDraft()
        minZ = initial.minZ
        maxZ = initial.maxZ
        uniqueness = initial.uniqueness
        maxDisparity = initial.maxDisparity
        matcher = initial.matcher
        lrCheck = initial.lrCheck
        p1 = initial.p1
        p2 = initial.p2
        yOffset = initial.yOffset
        flip = initial.flip

        let device = MTLCreateSystemDefaultDevice()!
        let engine = StereoEngine(device: device)
        pipeline = FramePipeline(engine: engine)
        renderer = MetalSceneRenderer(device: device, store: pipeline.store)
        renderer.model = self

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

    /// MetalSceneRenderer 每帧问一次：这一帧要不要额外烧一张照片 / 录一帧？
    ///
    /// 需要的数据由这里组装好传下去 —— MediaExportManager 不再反向持有 AppModel。
    /// 空闲时（没拍照、没录像）直接返回 nil，连快照都不构造。
    func captureTarget() -> (CVPixelBuffer, @MainActor (CVPixelBuffer) -> Void)? {
        if capture.photoPending, let frame = pipeline.store.latest {
            return capture.beginPhoto(frame: frame, analysis: analysis, overlay: ui,
                                      metadata: metadata(), size: outputSize(long: 1920))
        }
        guard capture.isRecording else { return nil }
        return capture.beginRecordingFrame(overlay: ui)
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

    /// 管线每帧发来的报告（已经 dispatch 到主线程）。
    private func apply(_ r: FrameReport) {
        analysis = r.analysis
        fps = r.fps
        gpuMs = r.timings.gpuMs
        let dev = renderer.device
        gpuReport = GpuReport(kind: r.kind, timings: r.timings,
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

    func toggleRecording() { capture.toggleRecording(size: outputSize(long: 1280)) }

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
