#pragma once

// ============================================================================
// CryptoEngine.h — Mã hóa / Giải mã sử dụng OpenSSL
//
// Xeno hỗ trợ nhiều cipher modes của AES-256:
//   GCM, CCM, CBC, CTR, ECB, OFB, OCB, CFB128, XTS
//
// OpenSSL EVP API (high-level API) được dùng thay vì low-level API
// vì nó linh hoạt và dễ chuyển đổi giữa các cipher modes.
//
// References:
//   - OpenSSL EVP: https://wiki.openssl.org/index.php/EVP
//   - AES-256-GCM: https://wiki.openssl.org/index.php/EVP_Authenticated_Encryption_and_Decryption
// ============================================================================

#include <string>
#include <vector>
#include <cstdint>
#include <optional>

namespace xeno {

/// Các cipher modes được hỗ trợ
///
/// Mỗi mode có đặc điểm riêng:
///   - GCM:  Authenticated encryption (có tag kiểm tra integrity)
///   - CBC:  Block cipher, cần padding, cần IV
///   - CTR:  Stream-like, không cần padding
///   - ECB:  Không dùng IV (KHÔNG an toàn cho data thực)
///   - ...
///
enum class CipherMode {
    AES_256_GCM,     ///< Galois/Counter Mode — khuyến nghị, có authentication
    AES_256_CCM,     ///< Counter with CBC-MAC
    AES_256_CBC,     ///< Cipher Block Chaining — phổ biến nhất
    AES_256_CTR,     ///< Counter mode — stream cipher behavior
    AES_256_ECB,     ///< Electronic Codebook — KHÔNG nên dùng cho data thực
    AES_256_OFB,     ///< Output Feedback
    AES_256_OCB,     ///< Offset Codebook Mode
    AES_256_CFB128,  ///< Cipher Feedback 128-bit
    AES_256_XTS,     ///< XEX-based Tweaked-codebook with Stealing
};

/// Convert string thành CipherMode enum
/// @param mode  VD: "gcm", "cbc", "ctr", ...
/// @return CipherMode tương ứng, nullopt nếu không hợp lệ
std::optional<CipherMode> parseCipherMode(const std::string& mode);

/// Engine mã hóa/giải mã
///
/// Sử dụng OpenSSL EVP API. Thread-safe (mỗi call tạo context riêng).
///
/// Ví dụ:
///   CryptoEngine crypto;
///   auto key = CryptoEngine::generateRandomBytes(32);  // 256-bit key
///   auto iv  = CryptoEngine::generateRandomBytes(12);  // 96-bit IV cho GCM
///   auto encrypted = crypto.encrypt("Hello", key, iv, CipherMode::AES_256_GCM);
///   auto decrypted = crypto.decrypt(encrypted.value(), key, iv, CipherMode::AES_256_GCM);
///
class CryptoEngine {
public:
    CryptoEngine() = default;
    ~CryptoEngine() = default;

    // ---- Encryption ----

    /// Mã hóa data
    ///
    /// Flow (OpenSSL EVP):
    ///   1. EVP_CIPHER_CTX_new()      — tạo context
    ///   2. EVP_EncryptInit_ex()      — khởi tạo với cipher + key + IV
    ///   3. EVP_EncryptUpdate()       — mã hóa data (có thể gọi nhiều lần)
    ///   4. EVP_EncryptFinal_ex()     — hoàn tất (xử lý padding)
    ///   5. EVP_CIPHER_CTX_free()     — giải phóng context
    ///
    /// @param plaintext   Data cần mã hóa
    /// @param key         Key (phải đúng 32 bytes cho AES-256)
    /// @param iv          Initialization Vector (size tùy mode)
    /// @param mode        Cipher mode
    /// @return Encrypted bytes, hoặc nullopt nếu lỗi
    std::optional<std::vector<uint8_t>> encrypt(
        const std::string& plaintext,
        const std::vector<uint8_t>& key,
        const std::vector<uint8_t>& iv,
        CipherMode mode = CipherMode::AES_256_GCM
    ) const;

    /// Giải mã data
    ///
    /// Flow (OpenSSL EVP):
    ///   1. EVP_CIPHER_CTX_new()
    ///   2. EVP_DecryptInit_ex()
    ///   3. EVP_DecryptUpdate()
    ///   4. EVP_DecryptFinal_ex()   — kiểm tra padding / auth tag
    ///   5. EVP_CIPHER_CTX_free()
    ///
    /// @param ciphertext  Data đã mã hóa
    /// @param key         Key (phải giống key lúc encrypt)
    /// @param iv          IV (phải giống IV lúc encrypt)
    /// @param mode        Cipher mode (phải giống mode lúc encrypt)
    /// @return Decrypted string, hoặc nullopt nếu lỗi
    std::optional<std::string> decrypt(
        const std::vector<uint8_t>& ciphertext,
        const std::vector<uint8_t>& key,
        const std::vector<uint8_t>& iv,
        CipherMode mode = CipherMode::AES_256_GCM
    ) const;

    // ---- Utility ----

    /// Sinh random bytes an toàn (dùng OpenSSL RAND_bytes)
    /// Dùng cho: tạo key, tạo IV, tạo nonce
    ///
    /// RAND_bytes dùng CSPRNG (Cryptographically Secure PRNG)
    /// của OS (CryptGenRandom trên Windows, /dev/urandom trên Linux)
    ///
    /// @param count  Số bytes cần sinh
    /// @return Vector chứa random bytes
    static std::vector<uint8_t> generateRandomBytes(size_t count);

    /// Lấy block size của cipher mode
    /// @return Block size (bytes)
    static int getBlockSize(CipherMode mode);

    /// Lấy cipher mode number (cho OpenSSL)
    /// @return Mode constant (VD: EVP_CIPH_GCM_MODE)
    static int getCipherModeValue(CipherMode mode);
};

} // namespace xeno
