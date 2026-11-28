//
//  NPY.swift
//  StereoVisionTests
//
//  极简 .npy 读取（只服务测试；对应 C++ 侧的 core/tests/npy.hpp）。
//
//  支持 numpy.save() 的 v1/v2/v3、小端、C 顺序、非 pickle；
//  descr 只认 '<f4' / '<u4' / '<u2' / '|u1' / '<u8'（golden 只产出这几种）。
//  遇到 fortran_order == True 直接报错：测试向量必须是 C 顺序。
//

import Foundation

enum NPYError: LocalizedError {
    case badFile(String)

    var errorDescription: String? {
        switch self {
        case .badFile(let msg): return "NPY: \(msg)"
        }
    }
}

struct NPYArray {
    /// 去掉字节序前缀的 dtype，例如 "f4" / "u1" / "u8"。
    let dtype: String
    let shape: [Int]
    let payload: Data

    var count: Int { shape.reduce(1, *) }

    var elementSize: Int {
        switch dtype {
        case "u1": return 1
        case "u2": return 2
        case "u4", "f4": return 4
        case "u8": return 8
        default: return 0
        }
    }
}

enum NPY {
    static func load(_ url: URL) throws -> NPYArray {
        let bytes = [UInt8](try Data(contentsOf: url))
        let magic: [UInt8] = [0x93] + Array("NUMPY".utf8)
        guard bytes.count >= 10, Array(bytes[0..<6]) == magic else {
            throw NPYError.badFile("not a .npy: \(url.lastPathComponent)")
        }
        let major = bytes[6]
        let headerLength: Int
        let headerOffset: Int
        if major == 1 {
            headerLength = Int(bytes[8]) | (Int(bytes[9]) << 8)
            headerOffset = 10
        } else {
            var n = 0
            for i in 0..<4 { n |= Int(bytes[8 + i]) << (8 * i) }
            headerLength = n
            headerOffset = 12
        }
        guard bytes.count >= headerOffset + headerLength else {
            throw NPYError.badFile("truncated header: \(url.lastPathComponent)")
        }
        let header = String(decoding: bytes[headerOffset..<(headerOffset + headerLength)], as: UTF8.self)

        guard header.contains("'fortran_order': False") else {
            throw NPYError.badFile("fortran_order must be False: \(url.lastPathComponent)")
        }

        // descr
        guard let descrLabel = header.range(of: "'descr':") else {
            throw NPYError.badFile("missing descr: \(url.lastPathComponent)")
        }
        guard let quote1 = header[descrLabel.upperBound...].firstIndex(of: "'") else {
            throw NPYError.badFile("bad descr: \(url.lastPathComponent)")
        }
        let afterQuote = header.index(after: quote1)
        guard let quote2 = header[afterQuote...].firstIndex(of: "'") else {
            throw NPYError.badFile("bad descr: \(url.lastPathComponent)")
        }
        let descr = String(header[afterQuote..<quote2])          // 例如 "<f4"
        let dtype = String(descr.suffix(2))

        // shape
        guard let parenStart = header.range(of: "("),
              let parenEnd = header.range(of: ")", range: parenStart.upperBound..<header.endIndex)
        else {
            throw NPYError.badFile("missing shape: \(url.lastPathComponent)")
        }
        let shapeText = header[parenStart.upperBound..<parenEnd.lowerBound]
        let shape = shapeText
            .split(separator: ",")
            .compactMap { Int($0.trimmingCharacters(in: .whitespaces)) }

        let elementSize = NPYArray(dtype: dtype, shape: shape, payload: Data()).elementSize
        guard elementSize > 0 else {
            throw NPYError.badFile("unsupported dtype \(descr): \(url.lastPathComponent)")
        }
        let payload = Data(bytes[(headerOffset + headerLength)...])
        let expected = shape.reduce(1, *) * elementSize
        guard payload.count == expected else {
            throw NPYError.badFile(
                "payload \(payload.count) != expected \(expected): \(url.lastPathComponent)")
        }
        return NPYArray(dtype: dtype, shape: shape, payload: payload)
    }

    /// float32 数组 → [Float]（逐元素 unaligned load，Data 不保证 4 字节对齐）。
    static func floats(_ array: NPYArray) throws -> [Float] {
        guard array.dtype == "f4" else {
            throw NPYError.badFile("expected f4, got \(array.dtype)")
        }
        var out = [Float](repeating: 0, count: array.count)
        out.withUnsafeMutableBufferPointer { dst in
            array.payload.withUnsafeBytes { raw in
                for i in 0..<array.count {
                    dst[i] = raw.loadUnaligned(fromByteOffset: i * 4, as: Float.self)
                }
            }
        }
        return out
    }

    /// uint8 数组 → [UInt8]。
    static func bytes(_ array: NPYArray) throws -> [UInt8] {
        guard array.dtype == "u1" else {
            throw NPYError.badFile("expected u1, got \(array.dtype)")
        }
        return [UInt8](array.payload)
    }
}
