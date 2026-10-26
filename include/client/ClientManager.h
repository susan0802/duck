#pragma once

// ============================================================================
// ClientManager.h — Quản lý tất cả RBXClient instances
//
// Nhiệm vụ:
//   - Quét và phát hiện Roblox processes mới
//   - Tạo RBXClient cho mỗi process
//   - Dọn dẹp clients đã chết
//   - Cung cấp API lookup client theo PID
//   - Cache management (lưu/load state)
//
// Sử dụng Observer pattern: thông báo khi client mới xuất hiện hoặc biến mất.
// ============================================================================

#include "RBXClient.h"
#include <unordered_map>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>
#include <memory>

namespace xeno {

/// Callback khi phát hiện client mới
using ClientAddedCallback = std::function<void(uint32_t pid)>;

/// Callback khi client bị xóa (process chết)
using ClientRemovedCallback = std::function<void(uint32_t pid)>;

/// Quản lý collection các RBXClient
///
/// Thread-safe: tất cả operations đều dùng mutex.
/// Có background thread liên tục quét process mới.
///
class ClientManager {
public:
    ClientManager();
    ~ClientManager();

    // Không cho copy
    ClientManager(const ClientManager&) = delete;
    ClientManager& operator=(const ClientManager&) = delete;

    // ---- Lifecycle ----

    /// Bắt đầu quét processes
    /// Tạo background thread chạy scanForClients() định kỳ
    void startScanning();

    /// Dừng quét
    void stopScanning();

    // ---- Client Access ----

    /// Lấy client theo PID
    /// @return shared_ptr đến client, nullptr nếu không tìm thấy
    std::shared_ptr<RBXClient> getClient(uint32_t pid) const;

    /// Lấy tất cả clients
    std::vector<std::shared_ptr<RBXClient>> getAllClients() const;

    /// Lấy danh sách PIDs
    std::vector<uint32_t> getClientPids() const;

    /// Số lượng clients
    size_t clientCount() const;

    // ---- Operations ----

    /// Attach vào client theo PID
    /// @return true nếu attach thành công
    bool attachClient(uint32_t pid);

    /// Remove client (process đã chết)
    void removeClient(uint32_t pid);

    /// Xóa tất cả clients
    void removeAllClients();

    // ---- Offsets ----

    /// Áp dụng offsets data (tải từ endpoint)
    /// @param offsetsJson  JSON string chứa version → offsets mapping
    void loadOffsets(const std::string& offsetsJson);

    // ---- Callbacks ----

    void onClientAdded(ClientAddedCallback callback);
    void onClientRemoved(ClientRemovedCallback callback);

    // ---- Serialization ----

    /// Tạo JSON array string của tất cả clients
    /// Dùng cho GetClients() export
    std::string toJson() const;

    // ---- Cache ----

    /// Load cache từ file (khôi phục state sau restart)
    void loadCache();

    /// Save cache ra file
    void saveCache() const;

private:
    /// Quét system processes, tìm Roblox instances mới
    ///
    /// Flow:
    ///   1. CreateToolhelp32Snapshot
    ///   2. Process32FirstW / Process32NextW
    ///   3. Lọc theo tên process ("RobloxPlayerBeta.exe")
    ///   4. Nếu PID mới → tạo RBXClient
    ///   5. Nếu PID đã biết nhưng process chết → xóa
    ///
    void scanForClients();

    /// Thread chạy scanForClients() lặp lại
    void scannerThread();

    mutable std::mutex mutex_;

    /// Map: PID → RBXClient (shared_ptr vì client có thể được reference nhiều nơi)
    std::unordered_map<uint32_t, std::shared_ptr<RBXClient>> clients_;

    /// Offsets map: version → RBXOffsets
    std::unordered_map<std::string, RBXOffsets> offsetsMap_;

    std::thread       scannerThread_;
    std::atomic<bool> scanning_{false};

    ClientAddedCallback   onAdded_;
    ClientRemovedCallback onRemoved_;
};

} // namespace xeno
