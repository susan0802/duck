// ============================================================================
// CryptoEngine.cpp — OpenSSL EVP encryption/decryption
//
// OpenSSL EVP (Envelope) API là high-level API cho crypto operations.
// Nó abstract hóa các cipher algorithms, cho phép chuyển đổi giữa
// AES-GCM, AES-CBC, AES-CTR, ... chỉ bằng thay đổi tham số.
//
// EVP API flow cho encryption:
//   ctx = EVP_CIPHER_CTX_new()           // Allocate context
//   EVP_EncryptInit_ex(ctx, cipher,      // Chọn algorithm
//                      NULL, key, iv)    // Set key & IV
//   EVP_EncryptUpdate(ctx, out, &outLen, // Encrypt data
//                     in, inLen)         // (có thể gọi nhiều lần)
//   EVP_EncryptFinal_ex(ctx, out, &len)  // Finalize (padding)
//   EVP_CIPHER_CTX_free(ctx)             // Free context
//
// Mỗi cipher mode cần setup khác nhau:
//   - GCM: cần set IV length, lấy auth tag sau encrypt
//   - CBC: cần IV = block size (16 bytes), tự thêm padding
//   - CTR: cần nonce, không cần padding (stream mode)
//   - ECB: không dùng IV (KHÔNG an toàn!)
// ============================================================================

#include "crypto/CryptoEngine.h"
#include <spdlog/spdlog.h>
#include <memory>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/err.h>

namespace xeno {

// ============================================================================
// parseCipherMode — String → enum
// ============================================================================

std::optional<CipherMode> parseCipherMode(const std::string& mode) {
    if (mode == "gcm")    return CipherMode::AES_256_GCM;
    if (mode == "ccm")    return CipherMode::AES_256_CCM;
    if (mode == "cbc")    return CipherMode::AES_256_CBC;
    if (mode == "ctr")    return CipherMode::AES_256_CTR;
    if (mode == "ecb")    return CipherMode::AES_256_ECB;
    if (mode == "ofb")    return CipherMode::AES_256_OFB;
    if (mode == "ocb")    return CipherMode::AES_256_OCB;
    if (mode == "cfb128") return CipherMode::AES_256_CFB128;
    if (mode == "xts")    return CipherMode::AES_256_XTS;
    return std::nullopt;
}

// ============================================================================
// Helper: Lấy OpenSSL cipher function cho mode
//
// Mỗi mode có một hàm EVP_aes_256_xxx() tương ứng:
//   GCM    → EVP_aes_256_gcm()
//   CBC    → EVP_aes_256_cbc()
//   CTR    → EVP_aes_256_ctr()
//   ...
//
// Trả về const EVP_CIPHER* (pointer đến cipher descriptor)
// ============================================================================

static const EVP_CIPHER* getCipher(CipherMode mode) {
    switch (mode) {
        case CipherMode::AES_256_GCM:    return EVP_aes_256_gcm();
        case CipherMode::AES_256_CCM:    return EVP_aes_256_ccm();
        case CipherMode::AES_256_CBC:    return EVP_aes_256_cbc();
        case CipherMode::AES_256_CTR:    return EVP_aes_256_ctr();
        case CipherMode::AES_256_ECB:    return EVP_aes_256_ecb();
        case CipherMode::AES_256_OFB:    return EVP_aes_256_ofb();
        case CipherMode::AES_256_OCB:    return EVP_aes_256_ocb();
        case CipherMode::AES_256_CFB128: return EVP_aes_256_cfb128();
        case CipherMode::AES_256_XTS:    return EVP_aes_256_xts();
        default: return nullptr;
    }
}

// RAII deleter cho EVP_CIPHER_CTX
struct CipherCtxDeleter {
    void operator()(EVP_CIPHER_CTX* ctx) { EVP_CIPHER_CTX_free(ctx); }
};
using CipherCtxPtr = std::unique_ptr<EVP_CIPHER_CTX, CipherCtxDeleter>;

// ============================================================================
// encrypt — Mã hóa data
// ============================================================================

std::optional<std::vector<uint8_t>> CryptoEngine::encrypt(
    const std::string& plaintext,
    const std::vector<uint8_t>& key,
    const std::vector<uint8_t>& iv,
    CipherMode mode
) const {
    // Kiểm tra key size (AES-256 = 32 bytes, XTS cần 64)
    size_t expectedKeySize = (mode == CipherMode::AES_256_XTS) ? 64 : 32;
    if (key.size() != expectedKeySize) {
        spdlog::error("Crypt.encrypt: invalid key size (expected {}, got {})",
                      expectedKeySize, key.size());
        return std::nullopt;
    }

    // 1. Lấy cipher descriptor
    const EVP_CIPHER* cipher = getCipher(mode);
    if (!cipher) {
        spdlog::error("Crypt.encrypt: invalid cipher mode");
        return std::nullopt;
    }

    // 2. Tạo context (RAII)
    CipherCtxPtr ctx(EVP_CIPHER_CTX_new());
    if (!ctx) return std::nullopt;

    // 3. Init encryption
    if (EVP_EncryptInit_ex(ctx.get(), cipher, nullptr, nullptr, nullptr) != 1) {
        spdlog::error("EVP_EncryptInit_ex (cipher) failed");
        return std::nullopt;
    }

    // Cho GCM/CCM/OCB: set IV length nếu khác default
    if (mode == CipherMode::AES_256_GCM || mode == CipherMode::AES_256_OCB) {
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_SET_IVLEN,
                            static_cast<int>(iv.size()), nullptr);
    }

    // Set key & IV
    if (EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr,
                           key.data(), iv.data()) != 1) {
        spdlog::error("EVP_EncryptInit_ex (key/iv) failed");
        return std::nullopt;
    }

    // 4. Encrypt
    std::vector<uint8_t> ciphertext(plaintext.size() + EVP_CIPHER_get_block_size(cipher));
    int outLen = 0;

    if (EVP_EncryptUpdate(ctx.get(), ciphertext.data(), &outLen,
                          reinterpret_cast<const uint8_t*>(plaintext.data()),
                          static_cast<int>(plaintext.size())) != 1) {
        spdlog::error("EVP_EncryptUpdate failed");
        return std::nullopt;
    }
    int totalLen = outLen;

    // 5. Finalize (xử lý padding cho CBC, hoặc no-op cho CTR/GCM)
    if (EVP_EncryptFinal_ex(ctx.get(), ciphertext.data() + totalLen, &outLen) != 1) {
        spdlog::error("EVP_EncryptFinal_ex failed");
        return std::nullopt;
    }
    totalLen += outLen;
    ciphertext.resize(totalLen);

    // Cho GCM/CCM/OCB: append authentication tag (16 bytes)
    if (mode == CipherMode::AES_256_GCM || mode == CipherMode::AES_256_OCB) {
        uint8_t tag[16];
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_GET_TAG, 16, tag);
        ciphertext.insert(ciphertext.end(), tag, tag + 16);
    }

    return ciphertext;
}

// ============================================================================
// decrypt — Giải mã data
// ============================================================================

std::optional<std::string> CryptoEngine::decrypt(
    const std::vector<uint8_t>& ciphertext,
    const std::vector<uint8_t>& key,
    const std::vector<uint8_t>& iv,
    CipherMode mode
) const {
    size_t expectedKeySize = (mode == CipherMode::AES_256_XTS) ? 64 : 32;
    if (key.size() != expectedKeySize) {
        spdlog::error("Crypt.decrypt: invalid key size");
        return std::nullopt;
    }

    const EVP_CIPHER* cipher = getCipher(mode);
    if (!cipher) return std::nullopt;

    CipherCtxPtr ctx(EVP_CIPHER_CTX_new());
    if (!ctx) return std::nullopt;

    // Cho GCM/OCB: tách auth tag từ cuối ciphertext (16 bytes)
    std::vector<uint8_t> data = ciphertext;
    uint8_t tag[16] = {};
    bool isAead = (mode == CipherMode::AES_256_GCM || mode == CipherMode::AES_256_OCB);

    if (isAead) {
        if (data.size() < 16) return std::nullopt;
        std::copy(data.end() - 16, data.end(), tag);
        data.resize(data.size() - 16);
    }

    // Init
    if (EVP_DecryptInit_ex(ctx.get(), cipher, nullptr, nullptr, nullptr) != 1)
        return std::nullopt;

    if (isAead) {
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_SET_IVLEN,
                            static_cast<int>(iv.size()), nullptr);
    }

    if (EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr,
                           key.data(), iv.data()) != 1)
        return std::nullopt;

    // Cho GCM/OCB: set expected tag
    if (isAead) {
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_SET_TAG, 16, tag);
    }

    // Decrypt
    std::vector<uint8_t> plaintext(data.size() + EVP_CIPHER_get_block_size(cipher));
    int outLen = 0;

    if (EVP_DecryptUpdate(ctx.get(), plaintext.data(), &outLen,
                          data.data(), static_cast<int>(data.size())) != 1)
        return std::nullopt;
    int totalLen = outLen;

    // Finalize — cho GCM, đây là lúc verify auth tag
    if (EVP_DecryptFinal_ex(ctx.get(), plaintext.data() + totalLen, &outLen) != 1) {
        // Auth tag mismatch → data bị tampered
        ERR_clear_error();
        spdlog::error("Crypt.decrypt: authentication failed or padding error");
        return std::nullopt;
    }
    totalLen += outLen;

    return std::string(plaintext.begin(), plaintext.begin() + totalLen);
}

// ============================================================================
// generateRandomBytes — Sinh random bytes an toàn (CSPRNG)
//
// RAND_bytes sử dụng:
//   - Windows: CryptGenRandom (hoặc BCryptGenRandom)
//   - Linux: getrandom() / /dev/urandom
//   - macOS: SecRandomCopyBytes
// ============================================================================

std::vector<uint8_t> CryptoEngine::generateRandomBytes(size_t count) {
    std::vector<uint8_t> bytes(count);
    if (RAND_bytes(bytes.data(), static_cast<int>(count)) != 1) {
        spdlog::error("RAND_bytes failed: {}",
                      ERR_error_string(ERR_get_error(), nullptr));
        throw std::runtime_error("Failed to generate random bytes");
    }
    return bytes;
}

// ============================================================================
// getBlockSize / getCipherModeValue
// ============================================================================

int CryptoEngine::getBlockSize(CipherMode mode) {
    switch (mode) {
        case CipherMode::AES_256_GCM:
        case CipherMode::AES_256_CCM:
        case CipherMode::AES_256_CTR:
        case CipherMode::AES_256_OFB:
        case CipherMode::AES_256_CFB128:
        case CipherMode::AES_256_OCB:
            return 1;  // Stream modes: no padding needed
        case CipherMode::AES_256_CBC:
        case CipherMode::AES_256_ECB:
        case CipherMode::AES_256_XTS:
            return 16; // Block modes: 16-byte blocks
        default:
            return 16;
    }
}

int CryptoEngine::getCipherModeValue(CipherMode mode) {
    const EVP_CIPHER* cipher = getCipher(mode);
    if (cipher) return EVP_CIPHER_get_mode(cipher);
    return 0;
}

} // namespace xeno
