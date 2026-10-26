// ============================================================================
// CompressionEngine.cpp — LZ4 & Zstandard compression
//
// Xeno dùng 2 thuật toán nén:
//
// 1. LZ4 — Nén/giải nén CỰC NHANH
//    - Tốc độ nén: ~780 MB/s, giải nén: ~4000 MB/s
//    - Tỉ lệ nén: trung bình (~2.1x)
//    - Dùng cho: real-time data (WebSocket messages, script transfer)
//
// 2. Zstandard (Zstd) — Nén HIỆU QUẢ CAO
//    - Tốc độ nén: ~500 MB/s, giải nén: ~1700 MB/s
//    - Tỉ lệ nén: cao (~2.8x, tuỳ level)
//    - Dùng cho: offsets data download (ep.zst.enc)
// ============================================================================

#include "compression/CompressionEngine.h"
#include <spdlog/spdlog.h>
#include <stdexcept>

#include <lz4.h>
#include <zstd.h>

namespace xeno {

// ============================================================================
// LZ4 Compress
//
// LZ4_compressBound(inputSize):
//   Tính worst-case output size.
//   Formula: inputSize + inputSize/255 + 16
//
// LZ4_compress_default(src, dst, srcSize, dstCapacity):
//   Nén với tốc độ tối đa.
//   Return: số bytes compressed, hoặc 0 nếu fail.
// ============================================================================

std::optional<std::vector<uint8_t>> CompressionEngine::lz4Compress(
    const void* data, size_t size
) {
    if (!data || size == 0) return std::vector<uint8_t>{};

    // 1. Tính maximum output size
    int maxCompressedSize = LZ4_compressBound(static_cast<int>(size));
    if (maxCompressedSize <= 0) {
        spdlog::error("LZ4_compressBound failed for size {}", size);
        return std::nullopt;
    }

    // 2. Allocate output buffer
    std::vector<uint8_t> compressed(maxCompressedSize);

    // 3. Compress
    int compressedSize = LZ4_compress_default(
        static_cast<const char*>(data),
        reinterpret_cast<char*>(compressed.data()),
        static_cast<int>(size),
        maxCompressedSize
    );

    if (compressedSize <= 0) {
        spdlog::error("LZ4_compress_default failed");
        return std::nullopt;
    }

    // 4. Resize về đúng kích thước
    compressed.resize(compressedSize);
    return compressed;
}

std::optional<std::vector<uint8_t>> CompressionEngine::lz4Compress(
    const std::string& data
) {
    return lz4Compress(data.data(), data.size());
}

// ============================================================================
// LZ4 Decompress
//
// LZ4_decompress_safe(src, dst, compressedSize, maxDecompressedSize):
//   Giải nén AN TOÀN — kiểm tra bounds, không bao giờ ghi quá buffer.
//   Return: số bytes decompressed, hoặc negative nếu fail.
//
// QUAN TRỌNG: Cần biết trước original size.
// Luôn dùng LZ4_decompress_safe(), KHÔNG dùng LZ4_decompress_fast() (deprecated).
// ============================================================================

std::optional<std::vector<uint8_t>> CompressionEngine::lz4Decompress(
    const void* compressedData, size_t compressedSize, size_t originalSize
) {
    if (!compressedData || compressedSize == 0) return std::vector<uint8_t>{};

    std::vector<uint8_t> decompressed(originalSize);

    int result = LZ4_decompress_safe(
        static_cast<const char*>(compressedData),
        reinterpret_cast<char*>(decompressed.data()),
        static_cast<int>(compressedSize),
        static_cast<int>(originalSize)
    );

    if (result < 0) {
        spdlog::error("LZ4_decompress_safe failed: {}", result);
        return std::nullopt;
    }

    decompressed.resize(result);
    return decompressed;
}

// ============================================================================
// Zstd Compress
//
// ZSTD_compressBound(srcSize): Maximum output size
// ZSTD_compress(dst, dstCap, src, srcSize, level): Compress
//   compressionLevel: 1 (nhanh) → 22 (nén tối đa), default 3
//   Return: compressed size, check with ZSTD_isError()
// ============================================================================

std::optional<std::vector<uint8_t>> CompressionEngine::zstdCompress(
    const void* data, size_t size, int compressionLevel
) {
    if (!data || size == 0) return std::vector<uint8_t>{};

    // 1. Tính max output size
    size_t maxSize = ZSTD_compressBound(size);
    std::vector<uint8_t> compressed(maxSize);

    // 2. Compress
    size_t compressedSize = ZSTD_compress(
        compressed.data(), maxSize,
        data, size,
        compressionLevel
    );

    // 3. Kiểm tra lỗi
    if (ZSTD_isError(compressedSize)) {
        spdlog::error("ZSTD_compress failed: {}",
                      ZSTD_getErrorName(compressedSize));
        return std::nullopt;
    }

    compressed.resize(compressedSize);
    return compressed;
}

std::optional<std::vector<uint8_t>> CompressionEngine::zstdCompress(
    const std::string& data, int compressionLevel
) {
    return zstdCompress(data.data(), data.size(), compressionLevel);
}

// ============================================================================
// Zstd Decompress
//
// ZSTD_getFrameContentSize(src, srcSize):
//   Đọc original size từ frame header.
//   Return: original size, ZSTD_CONTENTSIZE_UNKNOWN, hoặc _ERROR
//
// Đây là hàm quan trọng nhất — dùng để giải nén offsets data:
//   Download ep.zst.enc → decrypt (AES-256) → Zstd decompress → JSON
// ============================================================================

std::optional<std::vector<uint8_t>> CompressionEngine::zstdDecompress(
    const void* compressedData, size_t compressedSize
) {
    if (!compressedData || compressedSize == 0) return std::vector<uint8_t>{};

    // 1. Lấy original size từ header
    unsigned long long originalSize = ZSTD_getFrameContentSize(
        compressedData, compressedSize
    );

    if (originalSize == ZSTD_CONTENTSIZE_ERROR) {
        spdlog::error("ZSTD: not a valid compressed frame");
        return std::nullopt;
    }

    if (originalSize == ZSTD_CONTENTSIZE_UNKNOWN) {
        // Streaming decompression khi không biết size
        // Dùng conservative estimate (input * 4 hoặc lớn hơn)
        originalSize = compressedSize * 8;
    }

    // 2. Allocate và decompress
    std::vector<uint8_t> decompressed(originalSize);
    size_t result = ZSTD_decompress(
        decompressed.data(), originalSize,
        compressedData, compressedSize
    );

    if (ZSTD_isError(result)) {
        spdlog::error("ZSTD_decompress failed: {}",
                      ZSTD_getErrorName(result));
        return std::nullopt;
    }

    decompressed.resize(result);
    return decompressed;
}

std::optional<std::vector<uint8_t>> CompressionEngine::zstdDecompress(
    const std::vector<uint8_t>& compressedData
) {
    return zstdDecompress(compressedData.data(), compressedData.size());
}

// ============================================================================
// Version info
// ============================================================================

std::string CompressionEngine::lz4Version() {
    return LZ4_versionString();
}

std::string CompressionEngine::zstdVersion() {
    return ZSTD_versionString();
}

} // namespace xeno
