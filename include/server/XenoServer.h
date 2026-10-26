#pragma once

// ============================================================================
// XenoServer.h — HTTP Server chính của Xeno
//
// Sử dụng cpp-httplib (header-only library) để tạo HTTP server
// lắng nghe trên 127.0.0.1. Tất cả communication giữa UI và DLL
// đi qua server này.
//
// cpp-httplib là thư viện rất phổ biến cho embedded HTTP servers trong C++.
// Xem: https://github.com/yhirose/cpp-httplib
//
// Server nhận lệnh qua query parameters:
//   GET /?c=<command>&p=<pid>&...
// và trả về JSON response.
// ============================================================================

#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <memory>
#include <unordered_map>

// Forward declare httplib::Server thay vì include header nặng
namespace httplib {
    class Server;
    struct Request;
    struct Response;
}

namespace xeno {

// Forward declares
class ClientManager;
class WebSocketManager;

/// HTTP Server — tim đập của Xeno
///
/// Kiến trúc:
///   - Chạy trên thread riêng (không block main thread)
///   - Đăng ký route handlers cho từng nhóm chức năng
///   - CORS headers cho phép UI (thường là localhost app) truy cập
///   - JSON response format thống nhất
///
class XenoServer {
public:
    /// @param clientManager   Quản lý Roblox clients
    /// @param wsManager       Quản lý WebSocket connections
    XenoServer(ClientManager& clientManager, WebSocketManager& wsManager);
    ~XenoServer();

    // Không cho copy
    XenoServer(const XenoServer&) = delete;
    XenoServer& operator=(const XenoServer&) = delete;

    /// Khởi động server trên port chỉ định
    /// @param port  Port lắng nghe (thường 28015)
    /// @return true nếu bind thành công
    bool start(int port);

    /// Dừng server (thread-safe)
    void stop();

    /// Server có đang chạy không?
    bool isRunning() const;

    /// Port hiện tại
    int port() const { return port_; }

private:
    // ---- Route Registration ----
    // Mỗi hàm đăng ký một nhóm routes liên quan

    /// Routes cho script execution: Loadstring, compile, ...
    void registerScriptRoutes();

    /// Routes cho file system: ReadFile, WriteFile, MakeFolder, ...
    void registerFileSystemRoutes();

    /// Routes cho crypto: encrypt, decrypt, hash, ...
    void registerCryptoRoutes();

    /// Routes cho Base64: encode, decode
    void registerBase64Routes();

    /// Routes cho console: RConsole create/print/clear/...
    void registerConsoleRoutes();

    /// Routes cho process management: attach, detach, clients list, ...
    void registerProcessRoutes();

    /// Routes cho WebSocket management: open, send, close
    void registerWebSocketRoutes();

    /// Routes cho compression: LZ4 compress/decompress
    void registerCompressionRoutes();

    /// Routes cho instance manipulation: get/set properties, bytecode, ...
    void registerInstanceRoutes();

    /// Routes cho miscellaneous: version, settings, ...
    void registerMiscRoutes();

    // ---- Helper Methods ----

    /// Tạo JSON success response
    /// @param data  Dữ liệu trả về (string hoặc JSON)
    static std::string makeSuccessResponse(const std::string& data);

    /// Tạo JSON error response
    /// @param message  Thông báo lỗi
    static std::string makeErrorResponse(const std::string& message);

    /// Thêm CORS headers vào response
    /// UI chạy trên domain khác cần CORS để gọi được API
    static void addCorsHeaders(httplib::Response& res);

    /// Lấy query parameter, trả về empty string nếu không có
    static std::string getParam(const httplib::Request& req, const std::string& key);

    // ---- Members ----

    std::unique_ptr<httplib::Server> server_;     ///< httplib server instance
    std::thread                     serverThread_; ///< Thread chạy server
    std::atomic<bool>               running_{false};
    int                             port_{0};

    ClientManager&    clientManager_;  ///< Reference đến client manager
    WebSocketManager& wsManager_;      ///< Reference đến WebSocket manager
};

} // namespace xeno
