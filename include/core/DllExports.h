#pragma once

// ============================================================================
// DllExports.h — 6 hàm mà DLL export ra ngoài
//
// Khi một chương trình (VD: Xeno UI viết bằng C#/.NET) load Xeno.dll,
// nó sẽ gọi các hàm này thông qua P/Invoke hoặc LoadLibrary + GetProcAddress.
//
// Macro XENO_API:
//   - Khi BUILD DLL:  __declspec(dllexport) → đánh dấu hàm để export
//   - Khi DÙNG DLL:   __declspec(dllimport) → import hàm từ DLL
//
// Tất cả hàm dùng extern "C" để tránh C++ name mangling
// (VD: "Initialize" thay vì "?Initialize@@YAHXZ")
// ============================================================================

#ifdef _WIN32
    #ifdef XENO_BUILD_DLL
        #define XENO_API __declspec(dllexport)
    #else
        #define XENO_API __declspec(dllimport)
    #endif
#else
    // Trên Linux/macOS — dùng visibility attribute
    #define XENO_API __attribute__((visibility("default")))
#endif

extern "C" {

/// Khởi tạo toàn bộ hệ thống Xeno
///
/// Flow:
///   1. Load cấu hình
///   2. Tải offsets data từ remote server
///   3. Giải nén (Zstd) và parse offsets
///   4. Quét Roblox processes đang chạy
///   5. Khởi động HTTP server trên 127.0.0.1:{port}
///   6. Khởi động WebSocket server
///
/// @return true nếu thành công, false nếu lỗi
XENO_API bool Initialize();

/// Gắn (attach) vào một Roblox client process
///
/// Flow:
///   1. OpenProcess với PROCESS_ALL_ACCESS
///   2. Tìm RenderView address trong memory
///   3. Từ RenderView → tìm DataModel
///   4. Áp dụng offsets theo version
///   5. Inject code vào process
///   6. Đợi ping xác nhận thành công
///
/// @param pid  Process ID của Roblox client (từ GetClients)
/// @return true nếu attach thành công
XENO_API bool Attach(unsigned long pid);

/// Thực thi Luau script trên một client đã attach
///
/// Script được gửi qua WebSocket đến client, client compile
/// và chạy trong Roblox Luau VM.
///
/// @param pid     PID của client đã attach
/// @param script  Luau source code (UTF-8)
/// @return true nếu gửi thành công (không đảm bảo chạy thành công)
XENO_API bool Execute(unsigned long pid, const char* script);

/// Lấy danh sách Roblox client processes đang chạy
///
/// Quét tất cả processes, lọc ra RobloxPlayerBeta.exe,
/// trả về JSON array chứa thông tin mỗi client.
///
/// @return JSON string: [{"pid": 1234, "version": "...", "attached": false}, ...]
///         Caller phải free string bằng cách gọi hàm C runtime free()
XENO_API const char* GetClients();

/// Thay đổi một setting
///
/// @param key    Tên setting (VD: "Port", "AutoExecDir")
/// @param value  Giá trị mới
/// @return true nếu setting hợp lệ và được áp dụng
XENO_API bool SetSetting(const char* key, const char* value);

/// Lấy version string của Xeno
///
/// @return Chuỗi version (VD: "1.0.0") — static, không cần free
XENO_API const char* Version();

} // extern "C"
