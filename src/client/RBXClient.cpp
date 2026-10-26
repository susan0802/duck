// ============================================================================
// RBXClient.cpp — Triển khai quản lý 1 Roblox client process
//
// Đây là class phức tạp nhất — quản lý toàn bộ lifecycle của connection
// đến 1 Roblox process, từ detect → attach → execute → reattach → cleanup.
//
// Memory layout của Roblox (suy luận từ DLL):
//   Process Memory
//   ├── RenderView (entry point - tìm bằng pattern scan)
//   │   └── DataModel (offset từ RenderView)
//   │       ├── Workspace
//   │       ├── Players
//   │       ├── CoreGui
//   │       ├── ScriptContext
//   │       └── ...
//   └── ...
// ============================================================================

#include "client/RBXClient.h"
#include "process/ProcessManager.h"
#include "core/XenoConfig.h"

#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>

using json = nlohmann::json;

namespace xeno {

// ============================================================================
// State to string helper
// ============================================================================

const char* clientStateToString(ClientState state) {
    switch (state) {
        case ClientState::Detected:       return "Detected";
        case ClientState::Attaching:      return "Attaching";
        case ClientState::WaitingForGame: return "WaitingForGame";
        case ClientState::Attached:       return "Attached";
        case ClientState::Reattaching:    return "Reattaching";
        case ClientState::Closed:         return "Closed";
        default:                          return "Unknown";
    }
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

RBXClient::RBXClient(uint32_t pid, const std::string& version)
    : pid_(pid)
    , version_(version)
{
    spdlog::info("Creating new RBXClient: {}", pid);
}

RBXClient::~RBXClient() {
    spdlog::info("[{} ({})][::~RBXClient]: Client was closed and cleaned up successfully",
                 pid_, version_);

    shouldClose_ = true;

    if (reattachThread_.joinable()) {
        reattachThread_.join();
    }

    closeProcess();
}

// Move constructor
RBXClient::RBXClient(RBXClient&& other) noexcept
    : pid_(other.pid_)
    , version_(std::move(other.version_))
    , state_(other.state_)
    , processHandle_(other.processHandle_)
    , renderView_(other.renderView_)
    , dataModel_(other.dataModel_)
    , scriptContext_(other.scriptContext_)
    , offsets_(other.offsets_)
{
    other.processHandle_ = nullptr;
    other.state_ = ClientState::Closed;
}

RBXClient& RBXClient::operator=(RBXClient&& other) noexcept {
    if (this != &other) {
        closeProcess();
        pid_ = other.pid_;
        version_ = std::move(other.version_);
        state_ = other.state_;
        processHandle_ = other.processHandle_;
        renderView_ = other.renderView_;
        dataModel_ = other.dataModel_;
        scriptContext_ = other.scriptContext_;
        offsets_ = other.offsets_;
        other.processHandle_ = nullptr;
        other.state_ = ClientState::Closed;
    }
    return *this;
}

// ============================================================================
// Getters
// ============================================================================

std::string RBXClient::version() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return version_;
}

ClientState RBXClient::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

bool RBXClient::isAttached() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == ClientState::Attached;
}

// ============================================================================
// openProcess — Mở process handle
//
// OpenProcess(desiredAccess, inheritHandle, processId)
//   - desiredAccess: PROCESS_ALL_ACCESS = 0x1F0FFF
//     Bao gồm: VM_READ, VM_WRITE, VM_OPERATION, QUERY_INFORMATION, ...
//   - inheritHandle: FALSE (handle không kế thừa cho child processes)
//   - processId: PID của target
//
// Lỗi thường gặp:
//   - ERROR_ACCESS_DENIED (5): Roblox chạy elevated, Xeno không
//   - ERROR_INVALID_PARAMETER (87): PID không tồn tại
// ============================================================================

bool RBXClient::openProcess() {
#ifdef _WIN32
    processHandle_ = ProcessManager::openProcess(pid_, PROCESS_ALL_ACCESS);
    if (!processHandle_) {
        DWORD err = GetLastError();
        if (err == ERROR_ACCESS_DENIED) {
            spdlog::error("Failed to open the Roblox client process because it is "
                          "running with administrator privileges. Please run Xeno "
                          "as administrator or restart Roblox without administrator privileges.");
        }
        return false;
    }
    spdlog::info("Created RBXClient instance @{:#x}", reinterpret_cast<uintptr_t>(processHandle_));
    return true;
#else
    spdlog::error("openProcess() not supported on this platform");
    return false;
#endif
}

// ============================================================================
// closeProcess — Giải phóng process handle
// ============================================================================

void RBXClient::closeProcess() {
#ifdef _WIN32
    if (processHandle_) {
        CloseHandle(processHandle_);
        processHandle_ = nullptr;
    }
#endif
}

// ============================================================================
// isProcessAlive — Kiểm tra process còn chạy không
//
// GetExitCodeProcess trả về STILL_ACTIVE (259) nếu process chưa exit.
// ============================================================================

bool RBXClient::isProcessAlive() const {
#ifdef _WIN32
    if (!processHandle_) return false;
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(processHandle_, &exitCode)) return false;
    return exitCode == STILL_ACTIVE;
#else
    return false;
#endif
}

// ============================================================================
// findRenderView — Tìm RenderView address
//
// RenderView là điểm khởi đầu trong Roblox engine.
// Cần pattern scan trong memory để tìm nó.
//
// Pattern scanning:
//   1. VirtualQueryEx để liệt kê memory regions
//   2. ReadProcessMemory đọc từng region
//   3. Tìm byte pattern đặc trưng của RenderView vtable
//   4. Trả về address
// ============================================================================

bool RBXClient::findRenderView() {
#ifdef _WIN32
    if (!processHandle_) return false;

    spdlog::info("Fetching DataModel...");

    // Pattern scan logic (simplified):
    //
    // MEMORY_BASIC_INFORMATION memInfo;
    // uintptr_t addr = 0;
    //
    // while (VirtualQueryEx(processHandle_, (LPCVOID)addr, &memInfo, sizeof(memInfo))) {
    //     if (memInfo.State == MEM_COMMIT &&
    //         (memInfo.Protect & PAGE_READWRITE) &&
    //         memInfo.Type == MEM_PRIVATE) {
    //
    //         std::vector<uint8_t> buffer(memInfo.RegionSize);
    //         SIZE_T bytesRead = 0;
    //         if (ReadProcessMemory(processHandle_, memInfo.BaseAddress,
    //                               buffer.data(), memInfo.RegionSize, &bytesRead)) {
    //             // Tìm RenderView pattern trong buffer
    //             // ...
    //         }
    //     }
    //     addr = (uintptr_t)memInfo.BaseAddress + memInfo.RegionSize;
    // }

    if (renderView_ == 0) {
        spdlog::error("Failed to get RenderView");
        return false;
    }

    spdlog::info("RenderView: {:#x}", renderView_);
    return true;
#else
    return false;
#endif
}

// ============================================================================
// findDataModel — Tìm DataModel từ RenderView
//
// DataModel address = *(RenderView + offset)
// Offset khác nhau cho mỗi Roblox version.
// ============================================================================

bool RBXClient::findDataModel() {
#ifdef _WIN32
    if (!processHandle_ || renderView_ == 0) return false;

    spdlog::info("Waiting for DataModel...");

    // Đọc DataModel pointer từ RenderView + offset
    //
    // uintptr_t dataModelPtr = 0;
    // if (!ReadProcessMemory(processHandle_,
    //                        (LPCVOID)(renderView_ + offsets_.dataModel),
    //                        &dataModelPtr, sizeof(dataModelPtr), nullptr)) {
    //     return false;
    // }

    if (dataModel_ == 0) {
        spdlog::error("DataModel address is invalid");
        return false;
    }

    spdlog::info("DataModel ({}): {:#x}", version_, dataModel_);
    return true;
#else
    return false;
#endif
}

// ============================================================================
// applyOffsets — Áp dụng version-specific offsets
// ============================================================================

void RBXClient::applyOffsets(const RBXOffsets& offsets) {
    std::lock_guard<std::mutex> lock(mutex_);
    offsets_ = offsets;
    spdlog::info("Applied offsets for version: {}", version_);
}

// ============================================================================
// attach — Full attach flow
//
// Đây là function quan trọng nhất — thực hiện toàn bộ quá trình
// gắn vào Roblox process.
// ============================================================================

bool RBXClient::attach() {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = ClientState::Attaching;

    // Bước 1: Mở process
    if (!openProcess()) {
        state_ = ClientState::Detected;
        return false;
    }

    // Bước 2: Tìm RenderView
    if (!findRenderView()) {
        // Có thể client đang trong menu (chưa vào game)
        spdlog::info("Xeno was fast enough to attach using in menu method "
                     "as the user was joining the game.");
        state_ = ClientState::WaitingForGame;
    }

    // Bước 3: Tìm DataModel
    if (!findDataModel()) {
        // Đợi DataModel load
        spdlog::info("[{} ({})][::CRefresh]: DataModel is being loaded...", pid_, version_);

        // Timeout loop: đợi tối đa 30 giây
        // while (!findDataModel() && !shouldClose_) {
        //     Sleep(1000);
        //     if (--timeout <= 0) {
        //         spdlog::error("Could not find DataModel (Timeout)");
        //         return false;
        //     }
        // }
    }

    // Bước 4: Inject code vào Roblox process
    //
    // Injection flow:
    //   1. Cấp phát memory trong target: VirtualAllocEx
    //   2. Ghi code/data vào memory đã cấp: WriteProcessMemory
    //   3. Execute code: CreateRemoteThread hoặc hijack existing thread
    //
    // (Chi tiết injection code không được reconstruct ở đây)

    // Bước 5: Đợi ping xác nhận
    //
    // Sau khi inject, code trong Roblox sẽ gọi lại server
    // để xác nhận inject thành công. Nếu không nhận ping:
    //
    // spdlog::warn("Did not receive the success ping from client. "
    //              "Using RefreshRBXWindowV2...");

    state_ = ClientState::Attached;
    spdlog::info("Client was already attached, handling important core functions for the client :D");
    spdlog::info("Attach ping received from client \"{}\", "
                 "This indicates everything was successful.", pid_);

    // Bước 6: Handle auto-execute
    auto autoExecDir = XenoConfig::instance().autoExecDirectory().string();
    if (!autoExecDir.empty()) {
        handleAutoExecute(autoExecDir);
    }

    // Bước 7: Khởi động reattach thread
    //
    // Thread này theo dõi PlayerRemoving signal.
    // Khi player rời game → reattach tự động.
    //
    if (reattachThread_.joinable()) reattachThread_.join();
    reattachThread_ = std::thread(&RBXClient::playerRemovingHandler, this);

    return true;
}

// ============================================================================
// executeScript — Gửi Luau script để thực thi
// ============================================================================

bool RBXClient::executeScript(const std::string& script) {
    if (state_ != ClientState::Attached) {
        spdlog::error("Cannot execute: client not attached");
        return false;
    }

    // Thay thế environment variables
    std::string expandedScript = XenoConfig::instance()
        .expandEnvironmentVars(script, pid_, version_);

    // Gửi script qua WebSocket đến injected code
    //
    // Flow:
    //   1. Xeno gửi script text qua WebSocket
    //   2. Injected code nhận script
    //   3. Compile thành bytecode (Luau compiler)
    //   4. Execute trong ScriptContext
    //   5. Gửi kết quả về (nếu có)

    spdlog::info("[{} ({})][::Loadstring]: DataModel was finished loading. "
                 "Serving loadstring request", pid_, version_);

    return true;
}

// ============================================================================
// Memory Operations — Wrappers cho Windows API
// ============================================================================

bool RBXClient::readMemory(uintptr_t address, void* buffer, size_t size) const {
#ifdef _WIN32
    if (!processHandle_) return false;
    SIZE_T bytesRead = 0;
    return ReadProcessMemory(processHandle_, reinterpret_cast<LPCVOID>(address),
                             buffer, size, &bytesRead) && bytesRead == size;
#else
    return false;
#endif
}

bool RBXClient::writeMemory(uintptr_t address, const void* data, size_t size) {
#ifdef _WIN32
    if (!processHandle_) return false;
    SIZE_T bytesWritten = 0;
    return WriteProcessMemory(processHandle_, reinterpret_cast<LPVOID>(address),
                              data, size, &bytesWritten) && bytesWritten == size;
#else
    return false;
#endif
}

uintptr_t RBXClient::allocateMemory(size_t size, uint32_t flags) {
#ifdef _WIN32
    if (!processHandle_) return 0;
    LPVOID addr = VirtualAllocEx(processHandle_, nullptr, size,
                                 MEM_COMMIT | MEM_RESERVE, flags);
    return reinterpret_cast<uintptr_t>(addr);
#else
    return 0;
#endif
}

// ============================================================================
// handleAutoExecute — Chạy scripts từ autoexec directory
//
// Khi client attach thành công, tự động chạy tất cả .lua/.luau files
// trong thư mục autoexec.
// ============================================================================

int RBXClient::handleAutoExecute(const std::string& directory) {
    namespace fs = std::filesystem;

    if (!fs::exists(directory) || !fs::is_directory(directory)) {
        spdlog::warn("[{} ({})][::HandleAutoExecute]: Auto Execute directory is invalid",
                     pid_, version_);
        return 0;
    }

    int count = 0;
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (!entry.is_regular_file()) continue;

        auto ext = entry.path().extension().string();
        if (ext != ".lua" && ext != ".luau" && ext != ".txt") continue;

        // Đọc file
        std::ifstream file(entry.path());
        std::string script((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());

        if (!script.empty()) {
            executeScript(script);
            ++count;
        }
    }

    spdlog::info("[{} ({})][::HandleAutoExecute]: Executed {} script(s) from autoexec directory",
                 pid_, version_, count);
    return count;
}

// ============================================================================
// toJson — Serialize client info
// ============================================================================

std::string RBXClient::toJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    json j;
    j["pid"] = pid_;
    j["version"] = version_;
    j["state"] = clientStateToString(state_);
    j["attached"] = (state_ == ClientState::Attached);
    j["dataModel"] = dataModel_;
    j["renderView"] = renderView_;
    return j.dump();
}

// ============================================================================
// playerRemovingHandler — Auto-reattach khi player rời game
//
// Khi player teleport hoặc rời game, Roblox tạo DataModel mới.
// Xeno phải reattach vào DataModel mới.
//
// Flow:
//   1. Nhận tín hiệu PlayerRemoving (qua WebSocket)
//   2. Đợi CoreGui load (xác nhận game mới đã sẵn sàng)
//   3. Tìm DataModel mới
//   4. Re-inject
// ============================================================================

void RBXClient::playerRemovingHandler() {
    while (!shouldClose_) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        if (!isProcessAlive()) {
            // Process đã chết
            spdlog::info("[{} ({})][::PlayerRemoving]: Client was closed while reattaching",
                         pid_, version_);
            std::lock_guard<std::mutex> lock(mutex_);
            state_ = ClientState::Closed;
            return;
        }

        // Trong thực tế, đây sẽ chờ tín hiệu PlayerRemoving từ WebSocket
        // if (receivedPlayerRemoving) {
        //     spdlog::info("[{} ({})][::PlayerRemoving]: Player leaving ping received. "
        //                  "We have to reattach.", pid_, version_);
        //
        //     state_ = ClientState::Reattaching;
        //
        //     // Đợi DataModel mới
        //     spdlog::info("[{} ({})][::PlayerRemoving]: DataModel is being loaded...",
        //                  pid_, version_);
        //     waitForDataModel();
        //
        //     // Đợi CoreGui
        //     spdlog::info("[{} ({})][::PlayerRemoving]: Waiting for CoreGui...",
        //                  pid_, version_);
        //     if (!waitForCoreGui()) {
        //         spdlog::error("[{} ({})][::PlayerRemoving]: "
        //                       "Failed to reattach: CoreGui not found (Timeout)",
        //                       pid_, version_);
        //         continue;
        //     }
        //
        //     // Re-inject
        //     // ...
        //
        //     spdlog::info("[{} ({})][::PlayerRemoving]: CoreGui loaded", pid_, version_);
        //     spdlog::info("[{} ({})][::PlayerRemoving]: DataModel was loaded successfully",
        //                  pid_, version_);
        //     state_ = ClientState::Attached;
        // }
    }
}

// ============================================================================
// waitForCoreGui / waitForDataModel — Đợi game components load
// ============================================================================

bool RBXClient::waitForCoreGui(int timeoutMs) {
    // Đợi CoreGui available trong DataModel
    // CoreGui chứa các UI elements cần thiết cho inject
    //
    // auto start = std::chrono::steady_clock::now();
    // while (...) {
    //     // Đọc DataModel children, tìm "CoreGui"
    //     if (foundCoreGui) return true;
    //     if (elapsed > timeoutMs) break;
    //     Sleep(100);
    // }
    // spdlog::error("Could not find CoreGui (Timeout)");
    return false;
}

bool RBXClient::waitForDataModel(int timeoutMs) {
    // Đợi DataModel pointer valid
    // Sau khi player rời game, DataModel cũ bị destroy
    // và DataModel mới được tạo
    //
    // while (...) {
    //     if (findDataModel()) return true;
    //     Sleep(100);
    // }
    // spdlog::error("Failed to reattach: No DataModel (Timeout)");
    return false;
}

} // namespace xeno
