// ============================================================================
// Base64.cpp — Base64 encoding/decoding
//
// Base64 biến binary data thành text an toàn cho truyền qua HTTP, JSON, etc.
//
// Nguyên lý:
//   - Input: 3 bytes (24 bits)
//   - Output: 4 characters (mỗi char = 6 bits = 64 giá trị)
//   - Bảng mã: A-Z(0-25), a-z(26-51), 0-9(52-61), +(62), /(63)
//   - Padding: '=' thêm vào cuối nếu input không chia hết cho 3
//
// Ví dụ: "Hi" (2 bytes = 16 bits)
//   H = 0x48 = 01001000
//   i = 0x69 = 01101001
//   Padding:   00000000
//   → 010010 000110 100100 000000
//   → S      G      k      =
//   → "SGk="
//
// Xeno dùng Base64 cho:
//   - Encode script content trước khi gửi qua HTTP
//   - Encode binary data trong JSON responses
//   - Encode/decode trong crypt.base64encode/crypt.base64decode API
// ============================================================================

#include "utils/Base64.h"

namespace xeno {

// ============================================================================
// Bảng mã Base64 (RFC 4648)
// ============================================================================

static constexpr char ENCODE_TABLE[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Bảng decode: ASCII value → 6-bit value (255 = invalid)
// VD: 'A' (65) → 0, 'B' (66) → 1, ..., '/' (47) → 63
static constexpr uint8_t DECODE_TABLE[256] = {
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255, // 0-15
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255, // 16-31
    255,255,255,255,255,255,255,255,255,255,255, 62,255,255,255, 63, // 32-47  (+, /)
     52, 53, 54, 55, 56, 57, 58, 59, 60, 61,255,255,255,  0,255,255, // 48-63  (0-9, =)
    255,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, // 64-79  (A-O)
     15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,255,255,255,255,255, // 80-95  (P-Z)
    255, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, // 96-111 (a-o)
     41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51,255,255,255,255,255, // 112-127(p-z)
    // 128-255: all invalid
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
};

// ============================================================================
// encode — Binary → Base64 text
//
// Xử lý 3 bytes mỗi lần:
//   byte[0]: bits 7-2 → char 1, bits 1-0 → char 2 (high)
//   byte[1]: bits 7-4 → char 2 (low), bits 3-0 → char 3 (high)
//   byte[2]: bits 7-6 → char 3 (low), bits 5-0 → char 4
//
// Khi input không chia hết cho 3:
//   - Dư 1 byte: output 2 chars + "=="
//   - Dư 2 bytes: output 3 chars + "="
// ============================================================================

std::string Base64::encode(const void* data, size_t size) {
    if (!data || size == 0) return "";

    auto input = static_cast<const uint8_t*>(data);

    // Output size: ceil(size/3) * 4
    std::string output;
    output.reserve(((size + 2) / 3) * 4);

    size_t i = 0;

    // Xử lý từng nhóm 3 bytes
    for (; i + 2 < size; i += 3) {
        // 3 bytes → 4 base64 chars
        uint32_t triple = (static_cast<uint32_t>(input[i])     << 16)
                        | (static_cast<uint32_t>(input[i + 1]) << 8)
                        |  static_cast<uint32_t>(input[i + 2]);

        output += ENCODE_TABLE[(triple >> 18) & 0x3F]; // Bits 23-18
        output += ENCODE_TABLE[(triple >> 12) & 0x3F]; // Bits 17-12
        output += ENCODE_TABLE[(triple >>  6) & 0x3F]; // Bits 11-6
        output += ENCODE_TABLE[(triple      ) & 0x3F]; // Bits 5-0
    }

    // Xử lý bytes dư
    if (i < size) {
        uint32_t triple = static_cast<uint32_t>(input[i]) << 16;
        if (i + 1 < size) {
            triple |= static_cast<uint32_t>(input[i + 1]) << 8;
        }

        output += ENCODE_TABLE[(triple >> 18) & 0x3F];
        output += ENCODE_TABLE[(triple >> 12) & 0x3F];

        if (i + 1 < size) {
            // Dư 2 bytes → 3 chars + 1 padding
            output += ENCODE_TABLE[(triple >> 6) & 0x3F];
        } else {
            // Dư 1 byte → 2 chars + 2 padding
            output += '=';
        }
        output += '=';
    }

    return output;
}

std::string Base64::encode(const std::string& data) {
    return encode(data.data(), data.size());
}

std::string Base64::encode(const std::vector<uint8_t>& data) {
    return encode(data.data(), data.size());
}

// ============================================================================
// decode — Base64 text → binary
//
// Ngược lại encode:
//   4 base64 chars → 3 bytes
//   Bỏ qua whitespace, xử lý padding '='
// ============================================================================

std::optional<std::vector<uint8_t>> Base64::decode(const std::string& encoded) {
    if (encoded.empty()) return std::vector<uint8_t>{};

    std::vector<uint8_t> output;
    output.reserve((encoded.size() / 4) * 3);

    uint32_t accumulator = 0;
    int bits = 0;

    for (char c : encoded) {
        // Bỏ qua whitespace (newlines, spaces trong formatted base64)
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;

        // Padding = kết thúc
        if (c == '=') break;

        uint8_t value = DECODE_TABLE[static_cast<uint8_t>(c)];
        if (value == 255) {
            // Ký tự không hợp lệ
            return std::nullopt;
        }

        // Tích luỹ 6 bits mỗi lần
        accumulator = (accumulator << 6) | value;
        bits += 6;

        // Khi đủ 8 bits → output 1 byte
        if (bits >= 8) {
            bits -= 8;
            output.push_back(static_cast<uint8_t>((accumulator >> bits) & 0xFF));
        }
    }

    return output;
}

std::optional<std::string> Base64::decodeToString(const std::string& encoded) {
    auto bytes = decode(encoded);
    if (!bytes) return std::nullopt;
    return std::string(bytes->begin(), bytes->end());
}

// ============================================================================
// isValid — Kiểm tra chuỗi có phải Base64 hợp lệ không
//
// Rules:
//   - Chỉ chứa A-Z, a-z, 0-9, +, /, =, whitespace
//   - Padding '=' chỉ ở cuối (tối đa 2 ký tự)
//   - Độ dài (trừ whitespace) chia hết cho 4
// ============================================================================

bool Base64::isValid(const std::string& encoded) {
    if (encoded.empty()) return true;

    size_t validChars = 0;
    size_t paddingCount = 0;
    bool seenPadding = false;

    for (char c : encoded) {
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;

        if (c == '=') {
            seenPadding = true;
            paddingCount++;
            if (paddingCount > 2) return false;
            continue;
        }

        // Ký tự sau padding → invalid
        if (seenPadding) return false;

        // Kiểm tra ký tự hợp lệ
        if (DECODE_TABLE[static_cast<uint8_t>(c)] == 255) return false;

        validChars++;
    }

    // Tổng (validChars + paddingCount) phải chia hết cho 4
    return (validChars + paddingCount) % 4 == 0;
}

} // namespace xeno
