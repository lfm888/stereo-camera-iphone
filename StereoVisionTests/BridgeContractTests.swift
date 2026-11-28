//
//  BridgeContractTests.swift
//  StereoVisionTests
//
//  唯一一组 iOS 侧自动化测试：**只测桥接契约**，不测算法细节。
//
//  1) 跨语言 ABI：把 core/tests/golden 的 tiny 向量（64×48 RGB8, D=16）喂给
//     SVStereoCore.matchLeft，结果必须与 C++ golden 的 disp_f32 **逐位一致**。
//     这覆盖了 ObjC++ 的指针/跨度/格式/参数转换 —— 这些地方出错，真机上只会表现为"深度不对"。
//  2) 参数表自洽：界面靠它生成控件，所以 id/key 唯一、默认值在范围内、步长合法、
//     枚举标签条数与范围一致；默认值就是 SettingsDraft/PipelineSettings 的来源。
//
//  这两组测试不需要相机、不需要真机：CI 在模拟器上跑。
//

import XCTest

@testable import StereoVision

final class BridgeContractTests: XCTestCase {
    // MARK: - 夹具

    private func fixture(_ name: String) throws -> URL {
        guard let url = Bundle(for: Self.self).url(forResource: name, withExtension: "npy") else {
            throw NPYError.badFile("fixture \(name).npy 不在测试 bundle 里（检查 project.yml 的 resources）")
        }
        return url
    }

    // MARK: - 1) 跨语言 ABI：与 C++ golden 逐位一致

    func testTinyGoldenMatchesCppBitExact() throws {
        // golden/meta.json：W=64, H=48, D=16, dir=1, P1=8, P2=96, uniqueness=0.1, lrCheck=1
        let left = try NPY.load(try fixture("left_u8"))
        let right = try NPY.load(try fixture("right_u8"))
        let expected = try NPY.floats(try NPY.load(try fixture("disp_f32")))

        XCTAssertEqual(left.shape, [48, 64, 3])
        XCTAssertEqual(right.shape, [48, 64, 3])

        let W = 64, H = 48
        XCTAssertEqual(expected.count, W * H)

        guard let core = SVStereoCore(maxDisparity: 16, p1: 8, p2: 96,
                                      uniqueness: 0.1, lrCheck: true) else {
            return XCTFail("stereo_create 失败（内存不足？）")
        }

        let leftBytes = try NPY.bytes(left)
        let rightBytes = try NPY.bytes(right)
        var disparity = [Float](repeating: 0, count: W * H)

        let ok = leftBytes.withUnsafeBufferPointer { leftPtr in
            rightBytes.withUnsafeBufferPointer { rightPtr in
                disparity.withUnsafeMutableBufferPointer { dispPtr in
                    core.matchLeft(leftPtr.baseAddress!,
                                   leftStride: W * 3,
                                   leftFormat: .rgb8,
                                   right: rightPtr.baseAddress!,
                                   rightStride: W * 3,
                                   rightFormat: .rgb8,
                                   width: W, height: H,
                                   direction: 1,
                                   disparity: dispPtr.baseAddress!)
                }
            }
        }
        XCTAssertTrue(ok, "SVStereoCore.matchLeft 失败：\(core.lastError)")

        var mismatches = 0
        var firstMismatch = -1
        for i in 0..<expected.count where disparity[i].bitPattern != expected[i].bitPattern {
            if firstMismatch < 0 { firstMismatch = i }
            mismatches += 1
        }
        XCTAssertEqual(
            mismatches, 0,
            "跨语言 ABI 与 C++ golden 不一致：\(mismatches) 个像素，首个在 \(firstMismatch)"
        )
    }

    // MARK: - 2) 参数表自洽

    func testParameterTableIsSelfConsistent() {
        let params = SVParameterTable.allParameters()
        let keys = Set(params.map(\.key))
        XCTAssertEqual(
            keys,
            ["min_z", "max_z", "matcher", "max_disparity", "p1", "p2",
             "lr_check", "uniqueness", "y_offset", "flip_baseline", "overlay_alpha"]
        )
        XCTAssertEqual(Set(params.map { $0.identifier }).count, params.count, "id 必须唯一")

        for p in params {
            XCTAssertLessThanOrEqual(p.minValue, p.defaultValue, "\(p.key): min <= default")
            XCTAssertLessThanOrEqual(p.defaultValue, p.maxValue, "\(p.key): default <= max")
            XCTAssertGreaterThan(p.step, 0, "\(p.key): step > 0")
            XCTAssertLessThanOrEqual(p.minValue, p.maxValue, "\(p.key): min <= max")
            if p.kind == .choice {
                let n = Int((p.maxValue - p.minValue) / p.step) + 1
                XCTAssertEqual(p.enumLabels.count, n, "\(p.key): 枚举标签条数与范围不一致")
            }
        }

        // 默认值就是 SettingsDraft / PipelineSettings 的来源：抽查几条稳定的。
        XCTAssertEqual(SVParameterTable.lookup("max_disparity")?.defaultValue, 64)
        XCTAssertEqual(SVParameterTable.lookup("p1")?.defaultValue, 8)
        XCTAssertEqual(SVParameterTable.lookup("p2")?.defaultValue, 96)
        XCTAssertNil(SVParameterTable.lookup("no_such_key"))
    }

    // MARK: - 3) 无状态函数不再需要 context（API 2.00 的契约）

    func testStatelessSampleDepthNeedsNoContext() {
        // 8×8 全 1.0m：中位数必须 1.0，且 has_value = true。
        let depth = [Float](repeating: 1.0, count: 64)
        var value: Float = 0
        var has: Int32 = 0
        let ok = depth.withUnsafeBufferPointer { ptr in
            SVStereoCore.sampleDepth(ptr.baseAddress!, stride: 8, width: 8, height: 8,
                                     rect: SIMD4<Float>(0, 0, 1, 1), q: 0.5,
                                     value: &value, hasValue: &has)
        }
        XCTAssertTrue(ok)
        XCTAssertEqual(has, 1)
        XCTAssertEqual(value, 1.0, accuracy: 1e-6)

        // 反向矩形 = 参数非法：返回 false（不是 "样本不足"）。
        let bad = depth.withUnsafeBufferPointer { ptr in
            SVStereoCore.sampleDepth(ptr.baseAddress!, stride: 8, width: 8, height: 8,
                                     rect: SIMD4<Float>(0.5, 0, 0.4, 1), q: 0.5,
                                     value: &value, hasValue: &has)
        }
        XCTAssertFalse(bad)
    }
}
