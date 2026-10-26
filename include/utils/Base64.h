#pragma once

// ============================================================================
// Base64.h — Encode/decode Base64
//
// Base64 chuyển binary data thành text ASCII an toàn cho truyền tải.
// Mỗi 3 bytes input → 4 characters output (tăng 33% kích thước).
//
// Bảng ký tự: A-Z, a-z, 0-9, +, / (và = cho padding)
//
// VD: "Hello" → "SGVsbG8="
//   H=72=01001000, e=101=01100101, l=108=01101100
//   010010 000110 010101 101100 → S, G, V, s
//   ...
//
// Xeno dùng Base64 để:
//   - Encode script content khi gửi qua HTTP
//   - Encode binary data trong JSON responses
//   - Truyền encrypted data dạng text
// ============================================================================

#include <string>
#include <vector>
#include <cstdint>
#include <optional>

namespace xeno {

/// Encode/decode Base64
///
/// Tất cả methods static, thread-safe.
///
/// Ví dụ:
///   std::string encoded = Base64::encode("Hello World");
///   // → "SGVsbG8gV29ybGQ="
///
///   auto decoded = Base64::decode(encoded);
///   // → "Hello World"
///
class Base64 {
public:
    /// Encode string thành Base64
    static std::string encode(const std::string& input);

    /// Encode binary data thành Base64
    static std::string encode(const uint8_t* data, size_t size);

    /// Encode vector thành Base64
    static std::string encode(const std::vector<uint8_t>& data);

    /// Decode Base64 string thành bytes
    /// @return Decoded bytes, nullopt nếu input không phải Base64 hợp lệ
    static std::optional<std::vector<uint8_t>> decode(const std::string& input);

    /// Decode Base64 string thành string
    static std::optional<std::string> decodeToString(const std::string& input);

    /// Kiểm tra string có phải Base64 hợp lệ không
    ///
    /// Kiểm tra:
    ///   1. Độ dài chia hết cho 4
    ///   2. Chỉ chứa ký tự hợp lệ (A-Za-z0-9+/=)
    ///   3. Padding (=) chỉ ở cuối, tối đa 2 ký tự
    ///
    static bool isValid(const std::string& input);

private:
    /// Bảng encode: index 0-63 → character
    static constexpr char ENCODE_TABLE[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    /// Bảng decode: character → index (hoặc -1 nếu invalid)
    /// Được tạo tại compile-time
    static constexpr int DECODE_TABLE[256] = {
        // Chỉ hiển thị một phần — đầy đủ trong .cpp
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, // 0-15
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, // 16-31
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63, // 32-47 (+, /)
        52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1, // 48-63 (0-9)
        -1,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, // 64-79 (A-O)
        15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1, // 80-95 (P-Z)
        -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, // 96-111 (a-o)
        41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1, // 112-127 (p-z)
        // 128-255: tất cả -1
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    };
};

} // namespace xeno
