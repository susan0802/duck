#pragma once

// ============================================================================
// ProcessManager.h — Quản lý Windows processes
//
// Sử dụng Windows API để:
//   1. Liệt kê tất cả processes đang chạy
//   2. Lọc theo tên (tìm Roblox)
//   3. Mở process handle
//   4. Đọc/ghi memory
//   5. Cấp phát memory trong target process
//
// Các Windows API chính:
//   - CreateToolhelp32Snapshot: chụp "ảnh" danh sách processes
//   - Process32FirstW/NextW: duyệt qua danh sách
//   - OpenProcess: mở process với quyền chỉ định
//   - ReadProcessMemory: đọc memory từ process khác
//   - WriteProcessMemory: ghi memory vào process khác
//   - VirtualAllocEx: cấp phát memory trong process khác
//   - VirtualQueryEx: query thông tin về vùng nhớ
//
// Tất cả function là static — không cần instance.
// ============================================================================

#include <string>
#include <vector>
#include <cstdint>
#include <optional>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace xeno {

/// Thông tin về 1 process
struct ProcessInfo {
    uint32_t    pid;            ///< Process ID
    uint32_t    parentPid;      ///< Parent process ID
    std::string exeName;        ///< Tên file executable (VD: "RobloxPlayerBeta.exe")
    std::wstring exeNameW;      ///< Tên file (wide string — Unicode)
};

/// Thông tin vùng nhớ (từ VirtualQueryEx)
struct MemoryRegionInfo {
    uintptr_t baseAddress;      ///< Địa chỉ bắt đầu
    size_t    regionSize;       ///< Kích thước vùng nhớ
    uint32_t  protect;          ///< Memory protection (PAGE_READWRITE, ...)
    uint32_t  state;            ///< State (MEM_COMMIT, MEM_RESERVE, MEM_FREE)
    uint32_t  type;             ///< Type (MEM_PRIVATE, MEM_MAPPED, MEM_IMAGE)
};

/// Tiện ích thao tác Windows processes
///
/// Tất cả methods là static — gọi trực tiếp:
///   auto processes = ProcessManager::enumerateProcesses();
///   auto roblox = ProcessManager::findProcessesByName(L"RobloxPlayerBeta.exe");
///
class ProcessManager {
public:
    // ---- Process Enumeration ----

    /// Liệt kê tất cả processes đang chạy
    ///
    /// Sử dụng Toolhelp32 API:
    ///   1. CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    ///      → Tạo snapshot chứa thông tin tất cả processes
    ///   2. Process32FirstW(snapshot, &entry)
    ///      → Lấy process đầu tiên
    ///   3. Process32NextW(snapshot, &entry) lặp lại
    ///      → Lấy process tiếp theo
    ///   4. CloseHandle(snapshot)
    ///      → Giải phóng snapshot
    ///
    /// @return Vector chứa thông tin tất cả processes
    static std::vector<ProcessInfo> enumerateProcesses();

    /// Tìm processes theo tên executable
    /// @param name  Tên file (case-insensitive), VD: L"RobloxPlayerBeta.exe"
    /// @return Danh sách processes khớp tên
    static std::vector<ProcessInfo> findProcessesByName(const std::wstring& name);

    /// Process có đang chạy không?
    /// Mở process handle tạm thời để kiểm tra exit code
    static bool isProcessRunning(uint32_t pid);

    // ---- Process Handle ----

    /// Mở process handle với quyền chỉ định
    ///
    /// @param pid    Process ID
    /// @param access Access rights, VD:
    ///   - PROCESS_ALL_ACCESS     : toàn quyền
    ///   - PROCESS_VM_READ        : chỉ đọc memory
    ///   - PROCESS_VM_WRITE       : ghi memory
    ///   - PROCESS_VM_OPERATION   : VirtualAllocEx, VirtualProtectEx
    ///   - PROCESS_QUERY_INFORMATION : GetExitCodeProcess, ...
    ///
    /// @return Process handle, nullptr nếu lỗi (thường do thiếu quyền admin)
#ifdef _WIN32
    static HANDLE openProcess(uint32_t pid, uint32_t access = PROCESS_ALL_ACCESS);
#endif

    /// Lấy đường dẫn đầy đủ của executable
    /// @param pid  Process ID
    /// @return Full path, VD: "C:\\Program Files\\Roblox\\RobloxPlayerBeta.exe"
    static std::optional<std::string> getProcessImagePath(uint32_t pid);

    // ---- Memory Operations ----

    /// Đọc memory từ target process
    ///
    /// Nội bộ gọi ReadProcessMemory:
    ///   ReadProcessMemory(hProcess, (LPCVOID)address, buffer, size, &bytesRead)
    ///
    /// @return Số bytes đọc được (0 nếu lỗi)
#ifdef _WIN32
    static size_t readMemory(HANDLE process, uintptr_t address,
                             void* buffer, size_t size);
#endif

    /// Ghi memory vào target process
    ///
    /// Nội bộ gọi WriteProcessMemory:
    ///   WriteProcessMemory(hProcess, (LPVOID)address, data, size, &bytesWritten)
    ///
    /// @return Số bytes ghi được (0 nếu lỗi)
#ifdef _WIN32
    static size_t writeMemory(HANDLE process, uintptr_t address,
                              const void* data, size_t size);
#endif

    /// Cấp phát memory trong target process
    ///
    /// Nội bộ gọi VirtualAllocEx:
    ///   VirtualAllocEx(hProcess, NULL, size,
    ///                  MEM_COMMIT | MEM_RESERVE, protection)
    ///
    /// @param process     Process handle
    /// @param size        Số bytes cần cấp
    /// @param protection  VD: PAGE_READWRITE (0x04), PAGE_EXECUTE_READWRITE (0x40)
    /// @return Address của vùng nhớ mới, 0 nếu lỗi
#ifdef _WIN32
    static uintptr_t allocateMemory(HANDLE process, size_t size,
                                    uint32_t protection = PAGE_EXECUTE_READWRITE);
#endif

    /// Query thông tin vùng nhớ tại address
    ///
    /// Nội bộ gọi VirtualQueryEx:
    ///   VirtualQueryEx(hProcess, (LPCVOID)address, &memInfo, sizeof(memInfo))
    ///
#ifdef _WIN32
    static std::optional<MemoryRegionInfo> queryMemory(HANDLE process, uintptr_t address);
#endif

    // ---- Utility ----

    /// Lấy PID của process hiện tại
    static uint32_t getCurrentProcessId();

    /// Lấy console window handle
    /// Dùng cho AllocConsole/FreeConsole operations
#ifdef _WIN32
    static HWND getConsoleWindow();
#endif
};

} // namespace xeno
