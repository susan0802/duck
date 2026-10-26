// ============================================================================
// WebSocketManager.cpp — Quản lý WebSocket connections
//
// IXWebSocket flow:
//   1. ix::initNetSystem()    — init Winsock (Windows) / no-op (Unix)
//   2. ix::WebSocket ws;
//   3. ws.setUrl("ws://...");
//   4. ws.setOnMessageCallback([](const ix::WebSocketMessagePtr& msg) { ... });
//   5. ws.start();            — connect async
//   6. ws.send("...");        — send message
//   7. ws.stop();             — disconnect
//   8. ix::uninitNetSystem(); — cleanup
//
// Xeno sử dụng IXWebSocket cho:
//   - Communication giữa DLL và injected code trong Roblox
//   - Real-time script execution results
//   - Game signals (PlayerRemoving, etc.)
// ============================================================================

#include "server/WebSocketManager.h"
#include <spdlog/spdlog.h>
#include <chrono>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

namespace xeno {

WebSocketManager::WebSocketManager() = default;

WebSocketManager::~WebSocketManager() {
    shutdown();
}

// ============================================================================
// initialize — Init network subsystem
// ============================================================================

bool WebSocketManager::initialize() {
    // Trên Windows, ix::initNetSystem() gọi WSAStartup()
    if (!ix::initNetSystem()) {
        spdlog::error("ix::initNetSystem failed");
        return false;
    }

    initialized_ = true;

    // Khởi động cleaner thread — dọn dẹp dead connections mỗi 5 giây
    cleanerRunning_ = true;
    cleanerThread_ = std::thread(&WebSocketManager::cleanerThread, this);

    spdlog::info("WebSocket system initialized");
    return true;
}

// ============================================================================
// shutdown — Dọn dẹp toàn bộ
// ============================================================================

void WebSocketManager::shutdown() {
    if (!initialized_) return;

    cleanerRunning_ = false;
    if (cleanerThread_.joinable()) {
        cleanerThread_.join();
    }

    closeAllConnections();

    ix::uninitNetSystem();

    initialized_ = false;
    spdlog::info("WebSocket system shut down");
}

// ============================================================================
// openConnection — Mở WebSocket connection mới
// ============================================================================

bool WebSocketManager::openConnection(const std::string& uid, const std::string& url) {
    if (!initialized_) {
        spdlog::error("WebSocket system not initialized");
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    int maxConn = 50;
    if (static_cast<int>(connections_.size()) >= maxConn) {
        spdlog::error("Max WebSocket Connections reached. Please close other connections");
        return false;
    }

    if (connections_.count(uid)) {
        spdlog::warn("WebSocket connection for UID {} already exists", uid);
        return false;
    }

    // Tạo IXWebSocket connection
    auto ws = std::make_shared<ix::WebSocket>();
    ws->setUrl(url);

    // Callback khi nhận message
    ws->setOnMessageCallback([this, uid](const ix::WebSocketMessagePtr& msg) {
        switch (msg->type) {
            case ix::WebSocketMessageType::Message:
                if (messageCallback_) {
                    messageCallback_(uid, msg->str);
                }
                break;

            case ix::WebSocketMessageType::Open:
                spdlog::info("[{}]: WebSocket connected", uid);
                break;

            case ix::WebSocketMessageType::Close:
                spdlog::info("[{}]: WebSocket Closed for url '{}'",
                             uid, msg->closeInfo.reason);
                break;

            case ix::WebSocketMessageType::Error:
                spdlog::error("[{}]: WebSocket returned error \"({})\" for url '{}'",
                              uid, msg->errorInfo.reason,
                              msg->errorInfo.http_status);
                break;

            case ix::WebSocketMessageType::Ping:
            case ix::WebSocketMessageType::Pong:
            case ix::WebSocketMessageType::Fragment:
                break;
        }
    });

    ws->start();

    WebSocketConnection conn;
    conn.uid = uid;
    conn.wsPtr = ws;
    conn.connected = true;
    connections_[uid] = conn;

    spdlog::info("WebSocket Connections: {}", connections_.size());
    return true;
}

// ============================================================================
// closeConnection — Đóng 1 connection
// ============================================================================

void WebSocketManager::closeConnection(const std::string& uid) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = connections_.find(uid);
    if (it == connections_.end()) return;

    if (it->second.wsPtr) {
        it->second.wsPtr->stop();
    }
    connections_.erase(it);

    spdlog::info("Closed WebSocket for UID: {}", uid);
}

// ============================================================================
// closeAllConnections
// ============================================================================

void WebSocketManager::closeAllConnections() {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& [uid, conn] : connections_) {
        if (conn.wsPtr) conn.wsPtr->stop();
        conn.connected = false;
    }
    connections_.clear();

    spdlog::info("All WebSocket connections closed");
}

// ============================================================================
// sendMessage — Gửi message qua WebSocket
// ============================================================================

bool WebSocketManager::sendMessage(const std::string& uid, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = connections_.find(uid);
    if (it == connections_.end() || !it->second.connected) {
        spdlog::error("Cannot send message: connection {} not found or disconnected", uid);
        return false;
    }

    if (!it->second.wsPtr) return false;

    auto sendInfo = it->second.wsPtr->send(message);
    return sendInfo.success;
}

// ============================================================================
// Queries
// ============================================================================

bool WebSocketManager::isConnected(const std::string& uid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = connections_.find(uid);
    return it != connections_.end() && it->second.connected;
}

size_t WebSocketManager::connectionCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return connections_.size();
}

void WebSocketManager::onMessage(WsMessageCallback callback) {
    messageCallback_ = std::move(callback);
}

// ============================================================================
// cleanerThread — Dọn dẹp dead connections mỗi 5 giây
// ============================================================================

void WebSocketManager::cleanerThread() {
    spdlog::info("WebSockets Cleaner thread: {:#x}",
                 std::hash<std::thread::id>{}(std::this_thread::get_id()));

    while (cleanerRunning_) {
        for (int i = 0; i < 50 && cleanerRunning_; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (!cleanerRunning_) break;

        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> toRemove;

        for (const auto& [uid, conn] : connections_) {
            if (conn.wsPtr &&
                conn.wsPtr->getReadyState() != ix::ReadyState::Open) {
                toRemove.push_back(uid);
            }
        }

        for (const auto& uid : toRemove) {
            spdlog::info("Cleaned up '{}' from InitializedClients", uid);
            if (connections_[uid].wsPtr) connections_[uid].wsPtr->stop();
            connections_.erase(uid);
        }
    }
}

} // namespace xeno
