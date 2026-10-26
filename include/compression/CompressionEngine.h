#pragma once

// ============================================================================
// CompressionEngine.h — Nén/giải nén data với LZ4 và Zstandard
//
// Hai thuật toán, hai use cases:
//
// LZ4 (https://github.com/lz4/lz4):
//   - Cực nhanh (GB/s compression speed)
//   - Tỉ lệ nén vừa phải
//   - Dùng cho: data nhỏ, cần latency thấp
//   - VD: compress script trước khi gửi qua WebSocket
//
// Zstandard (https://github.com/facebook/zstd):
//   - Nhanh VÀ tỉ lệ nén cao
//   - Tốt hơn gzip/deflate ở cùng tốc độ
//   - Dùng cho: resource files, offsets data
//   - VD: decompress offsets data tải từ server
//
// So sánh:
//   | Thuật toán | Compress Speed | Decompress Speed | Ratio |
//   |------------|---------------|------------------|-------|
//   | LZ4        | 780 MB/s      | 4970 MB/s        | 2.1   |
//   | Zstd (1)   | 500 MB/s      | 1700 MB/s        | 2.9   |
//   | Zstd (19)  | 6 MB/s        | 1700 MB/s        | 3.7   |
//   | gzip (6)   | 36 MB/s       | 400 MB/s         | 3.1   |
// ============================================================================

#include <string>
#include <vector>
#include <cstdint>
#include <optional>

namespace xeno {

/// Engine nén/giải nén
///
/// Tất cả methods là static — không cần instance.
/// Thread-safe (không có shared state).
///
/// Ví dụ:
///   // LZ4
///   auto compressed = CompressionEngine::lz4Compress(data);
///   auto original   = CompressionEngine::lz4Decompress(compressed, originalSize);
///
///   // Zstd
///   auto compressed = CompressionEngine::zstdCompress(data);
///   auto original   = CompressionEngine::zstdDecompress(compressed);
///
class CompressionEngine {
public:
    // ---- LZ4 ----

    /// Nén data bằng LZ4
    ///
    /// LZ4 nén theo block — không streaming.
    /// Output format: [4 bytes original size][compressed data]
    ///
    /// @param data  Data cần nén
    /// @param size  Kích thước data
    /// @return Compressed bytes, nullopt nếu lỗi
    static std::optional<std::vector<uint8_t>> lz4Compress(
        const void* data, size_t size);

    /// Overload cho string
    static std::optional<std::vector<uint8_t>> lz4Compress(const std::string& data);

    /// Giải nén LZ4 data
    ///
    /// @param compressedData  Data đã nén
    /// @param compressedSize  Kích thước data nén
    /// @param originalSize    Kích thước data gốc (phải biết trước)
    /// @return Decompressed bytes, nullopt nếu lỗi
    static std::optional<std::vector<uint8_t>> lz4Decompress(
        const void* compressedData, size_t compressedSize,
        size_t originalSize);

    // ---- Zstandard ----

    /// Nén data bằng Zstd
    ///
    /// @param data              Data cần nén
    /// @param size              Kích thước
    /// @param compressionLevel  Mức nén (1-22, mặc định 3)
    ///   Level 1-3:   Nhanh, tỉ lệ thấp
    ///   Level 4-9:   Cân bằng
    ///   Level 10-19: Chậm, tỉ lệ cao
    ///   Level 20-22: Ultra (rất chậm, tỉ lệ cực cao)
    ///
    /// @return Compressed bytes, nullopt nếu lỗi
    static std::optional<std::vector<uint8_t>> zstdCompress(
        const void* data, size_t size, int compressionLevel = 3);

    /// Overload cho string
    static std::optional<std::vector<uint8_t>> zstdCompress(
        const std::string& data, int compressionLevel = 3);

    /// Giải nén Zstd data
    ///
    /// Zstd frame format chứa original size trong header,
    /// nên không cần truyền original size riêng.
    ///
    /// @param compressedData  Data đã nén
    /// @param compressedSize  Kích thước data nén
    /// @return Decompressed bytes, nullopt nếu lỗi
    static std::optional<std::vector<uint8_t>> zstdDecompress(
        const void* compressedData, size_t compressedSize);

    /// Overload cho vector
    static std::optional<std::vector<uint8_t>> zstdDecompress(
        const std::vector<uint8_t>& compressedData);

    /// Overload khi biết trước original size
    static std::optional<std::vector<uint8_t>> zstdDecompress(
        const void* compressedData, size_t compressedSize,
        size_t originalSize);

    // ---- Version Info ----

    /// Lấy version string của LZ4
    static std::string lz4Version();

    /// Lấy version string của Zstandard
    static std::string zstdVersion();
};

} // namespace xeno
