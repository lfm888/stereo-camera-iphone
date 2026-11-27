import AVFoundation
import CoreImage
import ImageIO
import Observation
import Photos
import SwiftUI
import UniformTypeIdentifiers

@MainActor
@Observable
final class Capture {
    private(set) var isRecording = false
    private(set) var recordingStart: Date?
    private(set) var message: String?

    /// 有一次拍照请求在等下一帧。AppModel 据此决定要不要走拍照分支。
    @ObservationIgnored private(set) var photoPending = false
    @ObservationIgnored private var writer: AVAssetWriter?
    @ObservationIgnored private var input: AVAssetWriterInput?
    @ObservationIgnored private var adaptor: AVAssetWriterInputPixelBufferAdaptor?
    @ObservationIgnored private var t0: CFTimeInterval?
    @ObservationIgnored private var messageTask: Task<Void, Never>?

    func takePhoto() { photoPending = true }

    func toggleRecording(size: (w: Int, h: Int)) {
        isRecording ? stopRecording() : startRecording(size: size)
    }

    /// 拍一张照片：返回待渲染的缓冲，以及渲染完的收尾。
    ///
    /// 需要的东西全部由调用方传进来 —— Capture 不再反向持有 AppModel。
    func beginPhoto(frame: StereoFrame, analysis: Analysis, overlay: CaptureUIState,
                    metadata: [String: Any],
                    size: (w: Int, h: Int)) -> (CVPixelBuffer, @MainActor (CVPixelBuffer) -> Void)? {
        photoPending = false
        guard let pb = Self.buffer(size.w, size.h) else { return nil }
        // 深度图在共享 MTLBuffer 里、带行跨度：拷成紧凑数组才能跨帧安全使用（C++ 里拷）。
        var depth = [Float](repeating: 0, count: frame.width * frame.height)
        depth.withUnsafeMutableBufferPointer { out in
            _ = SVExporter.copyDepth(frame.depthPointer, srcStride: frame.depthStride,
                                    width: frame.width, height: frame.height,
                                    dst: out.baseAddress!)
        }
        let snapshot = Snapshot(frame: frame, depth: depth, analysis: analysis, meta: metadata)
        return (pb, { [weak self] in self?.finishPhoto($0, snapshot, overlay: overlay) })
    }

    /// 录一帧：还没开始录、或 writer 还没准备好就返回 nil。
    func beginRecordingFrame(overlay: CaptureUIState) -> (CVPixelBuffer, @MainActor (CVPixelBuffer) -> Void)? {
        guard isRecording, let adaptor, let input, input.isReadyForMoreMediaData,
              let pool = adaptor.pixelBufferPool else { return nil }
        var pb: CVPixelBuffer?
        CVPixelBufferPoolCreatePixelBuffer(nil, pool, &pb)
        guard let pb else { return nil }
        let t = CACurrentMediaTime()
        return (pb, { [weak self] in self?.append($0, t, overlay: overlay) })
    }

    private func startRecording(size: (w: Int, h: Int)) {
        let (w, h) = size
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("StereoVision-\(Self.stamp()).mp4")
        do {
            let wr = try AVAssetWriter(outputURL: url, fileType: .mp4)
            let inp = AVAssetWriterInput(mediaType: .video, outputSettings: [
                AVVideoCodecKey: AVVideoCodecType.hevc, AVVideoWidthKey: w, AVVideoHeightKey: h,
            ])
            inp.expectsMediaDataInRealTime = true
            let ad = AVAssetWriterInputPixelBufferAdaptor(assetWriterInput: inp, sourcePixelBufferAttributes: Self.attributes(w, h))
            wr.add(inp)
            guard wr.startWriting() else { throw wr.error ?? CocoaError(.fileWriteUnknown) }
            wr.startSession(atSourceTime: .zero)
            writer = wr
            input = inp
            adaptor = ad
            t0 = nil
            isRecording = true
            recordingStart = .now
        } catch {
            flash("Recording failed: \(error.localizedDescription)")
        }
    }

    private func stopRecording() {
        guard let writer, let input else { return }
        isRecording = false
        recordingStart = nil
        self.writer = nil
        self.input = nil
        adaptor = nil
        input.markAsFinished()
        let held = Unchecked(writer)
        writer.finishWriting { [weak self] in
            let ok = held.value.status == .completed
            let url = held.value.outputURL
            Task { @MainActor in
                guard let self else { return }
                guard ok else { return self.flash("Recording failed") }
                await self.save { PHAssetChangeRequest.creationRequestForAssetFromVideo(atFileURL: url) }
                try? FileManager.default.removeItem(at: url)
                self.flash("Video saved to Photos")
            }
        }
    }

    private func append(_ pb: CVPixelBuffer, _ t: CFTimeInterval, overlay: CaptureUIState) {
        guard isRecording, let adaptor, let input, input.isReadyForMoreMediaData else { return }
        drawOverlay(pb, overlay: overlay)
        let start = t0 ?? t
        t0 = start
        adaptor.append(pb, withPresentationTime: CMTime(seconds: t - start, preferredTimescale: 600))
    }

    private func finishPhoto(_ pb: CVPixelBuffer, _ snap: Snapshot, overlay: CaptureUIState) {
        drawOverlay(pb, overlay: overlay)
        guard let view = CIContext().createCGImage(CIImage(cvPixelBuffer: pb), from: CGRect(x: 0, y: 0, width: CVPixelBufferGetWidth(pb), height: CVPixelBufferGetHeight(pb))) else { return }
        let image = UIImage(cgImage: view)
        Task {
            await save { PHAssetChangeRequest.creationRequestForAsset(from: image) }
            let dir = await Task.detached { try? Self.export(snap, view) }.value
            flash(dir.map { "Saved to Photos + Files › StereoVision › \($0.lastPathComponent)" } ?? "Saved to Photos (raw export failed)")
        }
    }

    private func save(_ change: @escaping () -> Void) async {
        guard await PHPhotoLibrary.requestAuthorization(for: .addOnly) == .authorized else {
            return flash("Photo library access denied")
        }
        do {
            try await PHPhotoLibrary.shared().performChanges(change)
        } catch {
            flash("Save failed: \(error.localizedDescription)")
        }
    }

    private func drawOverlay(_ pb: CVPixelBuffer, overlay state: CaptureUIState) {
        CVPixelBufferLockBaseAddress(pb, [])
        defer { CVPixelBufferUnlockBaseAddress(pb, []) }
        let w = CVPixelBufferGetWidth(pb), h = CVPixelBufferGetHeight(pb)
        guard let ctx = CGContext(data: CVPixelBufferGetBaseAddress(pb), width: w, height: h, bitsPerComponent: 8,
                                  bytesPerRow: CVPixelBufferGetBytesPerRow(pb), space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                  bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue) else { return }
        let lw: CGFloat = 720, lh = lw * CGFloat(h) / CGFloat(w)
        ctx.scaleBy(x: CGFloat(w) / lw, y: CGFloat(h) / lh)
        ImageRenderer(content: OverlayView(state: state, hints: false).frame(width: lw, height: lh)).render { _, draw in draw(ctx) }
    }

    private func flash(_ text: String) {
        message = text
        messageTask?.cancel()
        messageTask = Task {
            try? await Task.sleep(for: .seconds(3))
            if !Task.isCancelled { message = nil }
        }
    }

    nonisolated static func stamp() -> String {
        let f = DateFormatter()
        f.dateFormat = "yyyyMMdd-HHmmss"
        return f.string(from: .now)
    }

    nonisolated private static func attributes(_ w: Int, _ h: Int) -> [String: Any] {
        [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
            kCVPixelBufferWidthKey as String: w,
            kCVPixelBufferHeightKey as String: h,
            kCVPixelBufferMetalCompatibilityKey as String: true,
            kCVPixelBufferIOSurfacePropertiesKey as String: [:] as [String: Any],
        ]
    }

    nonisolated private static func buffer(_ w: Int, _ h: Int) -> CVPixelBuffer? {
        var pb: CVPixelBuffer?
        CVPixelBufferCreate(nil, w, h, kCVPixelFormatType_32BGRA, attributes(w, h) as CFDictionary, &pb)
        return pb
    }
}

struct Snapshot: @unchecked Sendable {
    let frame: StereoFrame
    let depth: [Float]
    let analysis: Analysis
    let meta: [String: Any]
}

extension Capture {
    nonisolated static func export(_ s: Snapshot, _ view: CGImage) throws -> URL {
        let f = s.frame
        let docs = try FileManager.default.url(for: .documentDirectory, in: .userDomainMask, appropriateFor: nil, create: true)
        let dir = docs.appendingPathComponent(stamp(), isDirectory: true)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)

        try png(view, dir.appendingPathComponent("view.png"))
        if let rgb = CIContext().createCGImage(CIImage(cvPixelBuffer: f.image), from: CGRect(x: 0, y: 0, width: CVPixelBufferGetWidth(f.image), height: CVPixelBufferGetHeight(f.image))) {
            try png(rgb, dir.appendingPathComponent("rgb.png"))
        }

        var mm = [UInt16](repeating: 0, count: f.width * f.height)
        mm.withUnsafeMutableBufferPointer { out in
            _ = SVExporter.depth(toMillimetres: s.depth, stride: f.width, width: f.width,
                                 height: f.height, out: out.baseAddress!)
        }
        let depthData = mm.withUnsafeBytes { Data($0) }
        if let provider = CGDataProvider(data: depthData as CFData),
           let img = CGImage(width: f.width, height: f.height, bitsPerComponent: 16, bitsPerPixel: 16, bytesPerRow: f.width * 2,
                             space: CGColorSpaceCreateDeviceGray(), bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.none.rawValue | CGBitmapInfo.byteOrder16Little.rawValue),
                             provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent) {
            try png(img, dir.appendingPathComponent("depth_mm.png"))
        }

        try ply(s, dir.appendingPathComponent("cloud.ply"))

        var meta = s.meta
        meta["depth"] = ["width": f.width, "height": f.height, "unit": "mm", "file": "depth_mm.png", "invalid": 0]
        meta["intrinsics"] = ["fx": f.K.fx, "fy": f.K.fy, "cx": f.K.cx, "cy": f.K.cy, "note": "pixel units of the depth map"]
        meta["baseline_mm"] = f.baselineMM
        // 分割模型已删（见 test.md §15）：不再有语义物体，objects 保持空数组以兼容旧导出格式；
        // 现在用的是纯传统的深度连通域面片块。
        meta["objects"] = [] as [[String: Any]]
        meta["blobs"] = s.analysis.blobs.map { b -> [String: Any] in
            ["id": b.id, "distance_m": b.distance, "x_m": b.x, "width_m": b.width, "cells": b.cells,
             "bbox_norm": [b.rect.minX, b.rect.minY, b.rect.width, b.rect.height]]
        }
        meta["point_cloud"] = "cloud.ply, camera frame: x right, y down, z forward, meters"
        try JSONSerialization.data(withJSONObject: meta, options: [.prettyPrinted, .sortedKeys]).write(to: dir.appendingPathComponent("meta.json"))
        return dir
    }

    private nonisolated static func png(_ img: CGImage, _ url: URL) throws {
        guard let dst = CGImageDestinationCreateWithURL(url as CFURL, UTType.png.identifier as CFString, 1, nil) else { throw CocoaError(.fileWriteUnknown) }
        CGImageDestinationAddImage(dst, img, nil)
        guard CGImageDestinationFinalize(dst) else { throw CocoaError(.fileWriteUnknown) }
    }

    /// 点云 PLY 全在 C++（core/src/cloud.cpp）：反投影、颜色取样、%.4f 格式化、
    /// 头部顶点数统计一次做完，这里只负责把字节写进文件。
    private nonisolated static func ply(_ s: Snapshot, _ url: URL) throws {
        let f = s.frame, pb = f.image
        CVPixelBufferLockBaseAddress(pb, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(pb, .readOnly) }
        guard let base = CVPixelBufferGetBaseAddress(pb) else { throw CocoaError(.fileWriteUnknown) }
        let k = f.K
        guard let data = SVExporter.pointCloudPLY(s.depth,
                                                  depthStride: f.width,
                                                  width: f.width, height: f.height,
                                                  intrinsics: SIMD4(k.fx, k.fy, k.cx, k.cy),
                                                  rgba: base.assumingMemoryBound(to: UInt8.self),
                                                  rgbaStride: CVPixelBufferGetBytesPerRow(pb),
                                                  rgbaWidth: CVPixelBufferGetWidth(pb),
                                                  rgbaHeight: CVPixelBufferGetHeight(pb)) else {
            throw CocoaError(.fileWriteUnknown)
        }
        try data.write(to: url)
    }
}
