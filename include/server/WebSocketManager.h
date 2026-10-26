#pragma once

// ============================================================================
// WebSocketManager.h — Quản lý WebSocket connections
//
// Xeno dùng IXWebSocket library cho WebSocket server.
// Mỗi Roblox client đã được inject sẽ mở 1 WebSocket connection
// về Xeno server để giao tiếp real-time.
//
// WebSocket được dùng vì:
//   1. Bidirectional — server có thể push data xuống client
//   2. Persistent connection — không cần mở/đóng liên tục như HTTP
//   3. Low latency — quan trọng cho script execution
//
// IXWebSocket: https://github.com/machinezone/IXWebSocket
// ============================================================================

#include <string>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>
#include <memory>

// Forward declare IXWebSocket
namespace ix { class WebSocket; }

namespace xeno {

/// Thông tin về 1 WebSocket connection
struct WebSocketConnection {
    std::string uid;                              ///< Unique ID (thường là PID dạng string)
    std::shared_ptr<ix::WebSocket> wsPtr;         ///< IXWebSocket object
    bool connected = false;                       ///< Đang kết nối?
};

/// Callback khi nhận message từ WebSocket client
using WsMessageCallback = std::function<void(const std::string& uid,
                                              const std::string& message)>;

/// Quản lý tất cả WebSocket connections
///
/// Kiến trúc:
///   - Mỗi connection được identify bằng UID (thường = PID)
///   - Có thread cleaner chạy định kỳ dọn dẹp dead connections
///   - Giới hạn số connections tối đa (tránh resource exhaustion)
///   - Thread-safe: mọi thao tác đều dùng mutex
///
class WebSocketManager {
public:
    WebSocketManager();
    ~WebSocketManager();

    // Không cho copy
    WebSocketManager(const WebSocketManager&) = delete;
    WebSocketManager& operator=(const WebSocketManager&) = delete;

    /// Khởi tạo WebSocket system (init network library)
    /// Phải gọi trước khi dùng bất kỳ function nào khác
    bool initialize();

    /// Dọn dẹp toàn bộ
    void shutdown();

    // ---- Connection Management ----

    /// Mở một WebSocket connection mới
    /// @param uid  Unique identifier (VD: PID dạng string)
    /// @param url  WebSocket URL để connect (VD: "ws://127.0.0.1:28016")
    /// @return true nếu mở thành công
    bool openConnection(const std::string& uid, const std::string& url);

    /// Đóng connection theo UID
    void closeConnection(const std::string& uid);

    /// Đóng tất cả connections
    void closeAllConnections();

    // ---- Messaging ----

    /// Gửi message đến client qua WebSocket
    /// @param uid      UID của client
    /// @param message  Nội dung (thường là Luau script hoặc JSON command)
    /// @return true nếu gửi thành công
    bool sendMessage(const std::string& uid, const std::string& message);

    // ---- Queries ----

    /// Connection có tồn tại và đang active?
    bool isConnected(const std::string& uid) const;

    /// Số connections hiện tại
    size_t connectionCount() const;

    // ---- Callbacks ----

    /// Đăng ký callback khi nhận message
    void onMessage(WsMessageCallback callback);

private:
    /// Thread dọn dẹp dead connections
    /// Chạy mỗi vài giây, kiểm tra và xóa connections đã disconnect
    void cleanerThread();

    mutable std::mutex mutex_;

    /// Map: UID → Connection info
    std::unordered_map<std::string, WebSocketConnection> connections_;

    WsMessageCallback messageCallback_;

    std::thread       cleanerThread_;
    std::atomic<bool> cleanerRunning_{false};

    bool initialized_{false};
};

} // namespace xeno
