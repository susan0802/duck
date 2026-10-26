// ============================================================================
// HashEngine.cpp — OpenSSL EVP hashing
//
// Hash = hàm một chiều, biến data bất kỳ thành fingerprint cố định.
//   Input: bất kỳ kích thước → Output: fixed size (VD: 32 bytes cho SHA256)
//   Tính chất: deterministic, one-way, collision-resistant
//
// Xeno dùng hashing cho:
//   - Verify integrity của downloaded data
//   - Script checksums
//   - API cho Luau scripts (game.hash, crypt.hash)
// ============================================================================

#include "crypto/HashEngine.h"
#include <spdlog/spdlog.h>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <memory>

#include <openssl/evp.h>
#include <openssl/err.h>

namespace xeno {

// ============================================================================
// parseHashAlgorithm — String → enum
// ============================================================================

std::optional<HashAlgorithm> parseHashAlgorithm(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (lower == "md5")        return HashAlgorithm::MD5;
    if (lower == "sha1")       return HashAlgorithm::SHA1;
    if (lower == "sha224")     return HashAlgorithm::SHA224;
    if (lower == "sha256")     return HashAlgorithm::SHA256;
    if (lower == "sha384")     return HashAlgorithm::SHA384;
    if (lower == "sha512")     return HashAlgorithm::SHA512;
    if (lower == "sha512/224" || lower == "sha512-224") return HashAlgorithm::SHA512_224;
    if (lower == "sha512/256" || lower == "sha512-256") return HashAlgorithm::SHA512_256;
    if (lower == "sha3-224")   return HashAlgorithm::SHA3_224;
    if (lower == "sha3-256")   return HashAlgorithm::SHA3_256;
    if (lower == "sha3-384")   return HashAlgorithm::SHA3_384;
    if (lower == "sha3-512")   return HashAlgorithm::SHA3_512;

    return std::nullopt;
}

// ============================================================================
// Helper: Lấy EVP_MD* cho algorithm
// ============================================================================

static const EVP_MD* getDigest(HashAlgorithm algo) {
    switch (algo) {
        case HashAlgorithm::MD5:        return EVP_md5();
        case HashAlgorithm::SHA1:       return EVP_sha1();
        case HashAlgorithm::SHA224:     return EVP_sha224();
        case HashAlgorithm::SHA256:     return EVP_sha256();
        case HashAlgorithm::SHA384:     return EVP_sha384();
        case HashAlgorithm::SHA512:     return EVP_sha512();
        case HashAlgorithm::SHA512_224: return EVP_sha512_224();
        case HashAlgorithm::SHA512_256: return EVP_sha512_256();
        case HashAlgorithm::SHA3_224:   return EVP_sha3_224();
        case HashAlgorithm::SHA3_256:   return EVP_sha3_256();
        case HashAlgorithm::SHA3_384:   return EVP_sha3_384();
        case HashAlgorithm::SHA3_512:   return EVP_sha3_512();
        default: return nullptr;
    }
}

// RAII deleter cho EVP_MD_CTX
struct MdCtxDeleter {
    void operator()(EVP_MD_CTX* ctx) { EVP_MD_CTX_free(ctx); }
};
using MdCtxPtr = std::unique_ptr<EVP_MD_CTX, MdCtxDeleter>;

// ============================================================================
// hash — Thực hiện hashing
//
// EVP Digest flow:
//   1. EVP_MD_CTX_new()      — allocate context
//   2. EVP_DigestInit_ex()   — chọn algorithm
//   3. EVP_DigestUpdate()    — feed data (có thể nhiều lần cho large data)
//   4. EVP_DigestFinal_ex()  — lấy kết quả
//   5. EVP_MD_CTX_free()     — free context
// ============================================================================

std::optional<std::vector<uint8_t>> HashEngine::hash(
    const void* data,
    size_t dataSize,
    HashAlgorithm algorithm
) const {
    const EVP_MD* md = getDigest(algorithm);
    if (!md) {
        spdlog::error("Invalid hash algorithm");
        return std::nullopt;
    }

    // 1. Tạo context (RAII)
    MdCtxPtr ctx(EVP_MD_CTX_new());
    if (!ctx) return std::nullopt;

    // 2. Init với algorithm đã chọn
    if (EVP_DigestInit_ex(ctx.get(), md, nullptr) != 1) {
        spdlog::error("EVP_DigestInit_ex failed");
        return std::nullopt;
    }

    // 3. Feed data — có thể gọi nhiều lần khi hash file lớn theo chunks
    if (EVP_DigestUpdate(ctx.get(), data, dataSize) != 1) {
        spdlog::error("EVP_DigestUpdate failed");
        return std::nullopt;
    }

    // 4. Finalize — lấy hash bytes
    unsigned int digestLen = 0;
    std::vector<uint8_t> digest(EVP_MD_get_size(md));
    if (EVP_DigestFinal_ex(ctx.get(), digest.data(), &digestLen) != 1) {
        spdlog::error("EVP_DigestFinal_ex failed");
        return std::nullopt;
    }

    digest.resize(digestLen);
    return digest;
}

std::optional<std::vector<uint8_t>> HashEngine::hash(
    const std::string& data,
    HashAlgorithm algorithm
) const {
    return hash(data.data(), data.size(), algorithm);
}

// ============================================================================
// hashToHex — Hash và convert sang hex string
// ============================================================================

std::string HashEngine::hashToHex(
    const std::string& data,
    HashAlgorithm algorithm
) const {
    auto result = hash(data, algorithm);
    if (!result) return "";

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (uint8_t byte : *result) {
        oss << std::setw(2) << static_cast<int>(byte);
    }
    return oss.str();
}

// ============================================================================
// digestSize — Output size cho mỗi algorithm
// ============================================================================

size_t HashEngine::digestSize(HashAlgorithm algorithm) {
    const EVP_MD* md = getDigest(algorithm);
    if (md) return static_cast<size_t>(EVP_MD_get_size(md));
    return 0;
}

// ============================================================================
// algorithmName — Tên hiển thị
// ============================================================================

const char* HashEngine::algorithmName(HashAlgorithm algorithm) {
    switch (algorithm) {
        case HashAlgorithm::MD5:        return "MD5";
        case HashAlgorithm::SHA1:       return "SHA1";
        case HashAlgorithm::SHA224:     return "SHA-224";
        case HashAlgorithm::SHA256:     return "SHA-256";
        case HashAlgorithm::SHA384:     return "SHA-384";
        case HashAlgorithm::SHA512:     return "SHA-512";
        case HashAlgorithm::SHA512_224: return "SHA-512/224";
        case HashAlgorithm::SHA512_256: return "SHA-512/256";
        case HashAlgorithm::SHA3_224:   return "SHA3-224";
        case HashAlgorithm::SHA3_256:   return "SHA3-256";
        case HashAlgorithm::SHA3_384:   return "SHA3-384";
        case HashAlgorithm::SHA3_512:   return "SHA3-512";
        default: return "Unknown";
    }
}

} // namespace xeno
