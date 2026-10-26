#pragma once

// ============================================================================
// HashEngine.h — Hashing sử dụng OpenSSL EVP
//
// Xeno hỗ trợ nhiều hash algorithms:
//   MD5, SHA1, SHA224, SHA256, SHA384, SHA512,
//   SHA512/224, SHA512/256, SHA3-224, SHA3-256, SHA3-384, SHA3-512
//
// OpenSSL EVP Digest API:
//   1. EVP_MD_CTX_new()      — tạo context
//   2. EVP_DigestInit_ex()   — chọn algorithm
//   3. EVP_DigestUpdate()    — feed data (gọi nhiều lần được)
//   4. EVP_DigestFinal_ex()  — lấy kết quả hash
//   5. EVP_MD_CTX_free()     — giải phóng
//
// Tất cả hàm hash đều thread-safe (tạo context riêng mỗi lần gọi).
// ============================================================================

#include <string>
#include <vector>
#include <cstdint>
#include <optional>

namespace xeno {

/// Các hash algorithms được hỗ trợ
enum class HashAlgorithm {
    MD5,          ///< 128-bit — KHÔNG an toàn, chỉ dùng cho checksum
    SHA1,         ///< 160-bit — deprecated cho security, vẫn dùng cho compatibility
    SHA224,       ///< 224-bit
    SHA256,       ///< 256-bit — khuyến nghị cho general purpose
    SHA384,       ///< 384-bit
    SHA512,       ///< 512-bit
    SHA512_224,   ///< SHA-512 truncated to 224-bit
    SHA512_256,   ///< SHA-512 truncated to 256-bit
    SHA3_224,     ///< SHA-3 224-bit (Keccak)
    SHA3_256,     ///< SHA-3 256-bit
    SHA3_384,     ///< SHA-3 384-bit
    SHA3_512,     ///< SHA-3 512-bit — mạnh nhất
};

/// Parse string thành HashAlgorithm
/// @param name  VD: "sha256", "md5", "sha3-512"
/// @return Algorithm tương ứng, nullopt nếu không hợp lệ
std::optional<HashAlgorithm> parseHashAlgorithm(const std::string& name);

/// Hash engine sử dụng OpenSSL EVP
///
/// Ví dụ:
///   HashEngine hasher;
///   std::string hex = hasher.hashToHex("Hello World", HashAlgorithm::SHA256);
///   // → "a591a6d40bf420404a011733cfb7b190d62c65bf0bcda32b57b277d9ad9f146e"
///
class HashEngine {
public:
    HashEngine() = default;
    ~HashEngine() = default;

    /// Hash data và trả về bytes
    ///
    /// @param data       Data cần hash
    /// @param dataSize   Kích thước data (bytes)
    /// @param algorithm  Hash algorithm
    /// @return Hash bytes, nullopt nếu lỗi
    std::optional<std::vector<uint8_t>> hash(
        const void* data,
        size_t dataSize,
        HashAlgorithm algorithm
    ) const;

    /// Hash string và trả về bytes
    std::optional<std::vector<uint8_t>> hash(
        const std::string& data,
        HashAlgorithm algorithm
    ) const;

    /// Hash và trả về hex string (lowercase)
    ///
    /// Đây là format thường dùng nhất:
    ///   SHA256("hello") → "2cf24dba5fb0a30e..."
    ///
    std::string hashToHex(
        const std::string& data,
        HashAlgorithm algorithm
    ) const;

    /// Lấy output size (bytes) của algorithm
    /// VD: SHA256 → 32, SHA512 → 64, MD5 → 16
    static size_t digestSize(HashAlgorithm algorithm);

    /// Lấy tên algorithm (cho logging/display)
    static const char* algorithmName(HashAlgorithm algorithm);
};

} // namespace xeno
