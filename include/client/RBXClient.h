#pragma once

// ============================================================================
// RBXClient.h — Đại diện cho 1 Roblox client process
//
// Mỗi instance RBXClient quản lý connection đến 1 Roblox process:
//   - Giữ HANDLE của process
//   - Lưu các address quan trọng (DataModel, RenderView, ScriptContext)
//   - Thực hiện read/write memory
//   - Quản lý lifecycle: detect → attach → execute → reattach → cleanup
//
// Các "offsets" là khoảng cách từ base address đến các field trong
// Roblox's internal structures. Chúng thay đổi theo mỗi Roblox update,
// nên phải tải từ server (endpoint).
// ============================================================================

#include <string>
#include <cstdint>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace xeno {

/// Offsets cho một phiên bản Roblox cụ thể
///
/// Mỗi khi Roblox update, vị trí các field trong memory thay đổi.
/// Offsets được tải từ remote server và áp dụng theo version.
///
struct RBXOffsets {
    uintptr_t dataModel{0};       ///< Offset đến DataModel từ RenderView
    uintptr_t scriptContext{0};   ///< Offset đến ScriptContext
    uintptr_t workspace{0};      ///< Offset đến Workspace
    uintptr_t players{0};        ///< Offset đến Players service
    // ... thêm offsets khác tùy version
};

/// Trạng thái của RBXClient
enum class ClientState {
    Detected,       ///< Process được phát hiện nhưng chưa attach
    Attaching,      ///< Đang trong quá trình attach
    WaitingForGame, ///< Đợi player vào game
    Attached,       ///< Đã attach thành công, sẵn sàng execute
    Reattaching,    ///< Đang reattach (player rời game)
    Closed          ///< Process đã đóng
};

/// Convert state thành string (cho logging)
const char* clientStateToString(ClientState state);

/// Đại diện cho 1 Roblox client process
///
/// Lifecycle:
///   Detected → Attaching → WaitingForGame → Attached
///       ↑                                       │
///       └──── Reattaching ◄─────────────────────┘
///                                        (PlayerRemoving)
///
class RBXClient {
public:
    /// @param pid      Process ID của Roblox
    /// @param version  Roblox client version string
    explicit RBXClient(uint32_t pid, const std::string& version = "");
    ~RBXClient();

    // Không cho copy (HANDLE không nên copy)
    RBXClient(const RBXClient&) = delete;
    RBXClient& operator=(const RBXClient&) = delete;

    // Move OK
    RBXClient(RBXClient&& other) noexcept;
    RBXClient& operator=(RBXClient&& other) noexcept;

    // ---- Getters ----
    uint32_t    pid() const { return pid_; }
    std::string version() const;
    ClientState state() const;
    bool        isAttached() const;
    uintptr_t   dataModelAddress() const { return dataModel_; }
    uintptr_t   renderViewAddress() const { return renderView_; }

    // ---- Core Operations ----

    /// Mở process handle (OpenProcess)
    ///
    /// Yêu cầu quyền PROCESS_ALL_ACCESS bao gồm:
    ///   - PROCESS_VM_READ      : đọc memory
    ///   - PROCESS_VM_WRITE     : ghi memory
    ///   - PROCESS_VM_OPERATION : cấp phát memory (VirtualAllocEx)
    ///   - PROCESS_QUERY_INFORMATION : lấy thông tin process
    ///
    /// @return true nếu mở thành công
    bool openProcess();

    /// Đóng process handle
    void closeProcess();

    /// Process còn chạy không?
    bool isProcessAlive() const;

    /// Tìm RenderView address trong process memory
    ///
    /// RenderView là object gốc trong Roblox engine, từ đó
    /// có thể tìm được DataModel và các service khác.
    ///
    /// @return true nếu tìm thấy
    bool findRenderView();

    /// Tìm DataModel address từ RenderView
    ///
    /// DataModel là root của Roblox instance tree,
    /// chứa Workspace, Players, ReplicatedStorage, ...
    ///
    /// @return true nếu tìm thấy
    bool findDataModel();

    /// Áp dụng offsets cho version hiện tại
    void applyOffsets(const RBXOffsets& offsets);

    /// Thực hiện attach đầy đủ
    ///
    /// Flow:
    ///   1. openProcess()
    ///   2. findRenderView()
    ///   3. findDataModel()
    ///   4. applyOffsets()
    ///   5. Inject code
    ///   6. Wait for success ping
    ///
    /// @return true nếu attach hoàn tất
    bool attach();

    /// Thực thi Luau script
    /// @param script  Luau source code
    /// @return true nếu gửi thành công
    bool executeScript(const std::string& script);

    // ---- Memory Operations ----

    /// Đọc memory từ target process
    /// @param address  Địa chỉ trong target process
    /// @param buffer   Buffer để chứa data đọc được
    /// @param size     Số bytes cần đọc
    /// @return true nếu đọc thành công
    bool readMemory(uintptr_t address, void* buffer, size_t size) const;

    /// Ghi memory vào target process
    /// @param address  Địa chỉ trong target process
    /// @param data     Data cần ghi
    /// @param size     Số bytes cần ghi
    /// @return true nếu ghi thành công
    bool writeMemory(uintptr_t address, const void* data, size_t size);

    /// Cấp phát memory trong target process
    /// @param size   Số bytes cần cấp phát
    /// @param flags  Protection flags (VD: PAGE_READWRITE)
    /// @return Address của vùng nhớ đã cấp, 0 nếu lỗi
    uintptr_t allocateMemory(size_t size, uint32_t flags = 0x40 /*PAGE_EXECUTE_READWRITE*/);

    // ---- Auto-Execute ----

    /// Chạy tất cả scripts trong autoexec directory
    /// @param directory  Đường dẫn thư mục autoexec
    /// @return Số scripts đã chạy
    int handleAutoExecute(const std::string& directory);

    // ---- Serialization ----

    /// Serialize thành JSON string cho API response
    std::string toJson() const;

private:
    /// Thread xử lý PlayerRemoving (reattach khi player rời game)
    void playerRemovingHandler();

    /// Tìm CoreGui (cần cho inject)
    bool waitForCoreGui(int timeoutMs = 30000);

    /// Đợi DataModel load xong
    bool waitForDataModel(int timeoutMs = 30000);

    uint32_t    pid_;               ///< Process ID
    std::string version_;           ///< Roblox version
    ClientState state_{ClientState::Detected};

#ifdef _WIN32
    HANDLE processHandle_{nullptr}; ///< Process handle từ OpenProcess
#else
    void* processHandle_{nullptr};  ///< Placeholder cho non-Windows
#endif

    uintptr_t renderView_{0};       ///< RenderView address
    uintptr_t dataModel_{0};        ///< DataModel address
    uintptr_t scriptContext_{0};    ///< ScriptContext address

    RBXOffsets offsets_;             ///< Offsets cho version hiện tại

    mutable std::mutex mutex_;
    std::thread reattachThread_;    ///< Thread cho PlayerRemoving handler
    std::atomic<bool> shouldClose_{false};
};

} // namespace xeno
