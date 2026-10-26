// ============================================================================
// ProcessManager.cpp — Windows Process Management
//
// Đây là layer thấp nhất — giao tiếp trực tiếp với Windows kernel
// thông qua Win32 API.
//
// Toolhelp32 API:
//   Cung cấp "snapshot" (ảnh chụp) của hệ thống tại một thời điểm,
//   bao gồm danh sách processes, threads, modules, heaps.
//   → Dùng để liệt kê tất cả processes đang chạy.
//
// Process Memory API:
//   Cho phép đọc/ghi memory của process khác.
//   → Yêu cầu quyền (privileges) phù hợp.
//   → Cần HANDLE từ OpenProcess().
//
// Virtual Memory API:
//   Cho phép cấp phát, thay đổi protection, query memory trong process khác.
//   → VirtualAllocEx: cấp phát memory trong remote process
//   → VirtualQueryEx: lấy thông tin vùng nhớ
//   → VirtualProtectEx: thay đổi read/write/execute permissions
// ============================================================================

#include "process/ProcessManager.h"
#include <spdlog/spdlog.h>
#include <algorithm>

#ifdef _WIN32
#include <TlHelp32.h>   // Toolhelp32 API
#include <Psapi.h>       // Process status API (K32GetProcessImageFileNameW)
#pragma comment(lib, "Psapi.lib")
#endif

namespace xeno {

// ============================================================================
// enumerateProcesses — Liệt kê tất cả processes
//
// Toolhelp32 API flow:
//   1. CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
//      → Chụp snapshot chứa thông tin TẤT CẢ processes
//      → TH32CS_SNAPPROCESS = 0x02: chỉ snapshot processes
//      → 0: không giới hạn process nào
//
//   2. PROCESSENTRY32W entry;
//      entry.dwSize = sizeof(PROCESSENTRY32W);
//      → Struct chứa thông tin 1 process
//      → PHẢI set dwSize trước khi gọi Process32FirstW
//      → 'W' = Wide string version (Unicode)
//
//   3. Process32FirstW(snapshot, &entry)
//      → Lấy process đầu tiên
//      → Trả FALSE nếu snapshot rỗng
//
//   4. Process32NextW(snapshot, &entry) — lặp lại
//      → Lấy process tiếp theo
//      → Trả FALSE khi hết
//
//   5. CloseHandle(snapshot)
//      → QUAN TRỌNG: phải đóng handle, nếu không → handle leak
// ============================================================================

std::vector<ProcessInfo> ProcessManager::enumerateProcesses() {
    std::vector<ProcessInfo> result;

#ifdef _WIN32
    // 1. Chụp snapshot
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        spdlog::error("CreateToolhelp32Snapshot failed: {}", GetLastError());
        return result;
    }

    // 2. Chuẩn bị struct
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(PROCESSENTRY32W);
    // ↑ BẮT BUỘC! Nếu quên set dwSize → Process32FirstW fails

    // 3. Lấy process đầu tiên
    if (!Process32FirstW(snapshot, &entry)) {
        CloseHandle(snapshot);
        return result;
    }

    // 4. Duyệt qua tất cả processes
    do {
        ProcessInfo info;
        info.pid = entry.th32ProcessID;
        info.parentPid = entry.th32ParentProcessID;
        info.exeNameW = entry.szExeFile;

        // Convert wide string → narrow string (UTF-8)
        // Dùng WideCharToMultiByte
        int size = WideCharToMultiByte(CP_UTF8, 0, entry.szExeFile, -1,
                                       nullptr, 0, nullptr, nullptr);
        if (size > 0) {
            info.exeName.resize(size - 1); // -1 vì không cần null terminator
            WideCharToMultiByte(CP_UTF8, 0, entry.szExeFile, -1,
                                info.exeName.data(), size, nullptr, nullptr);
        }

        result.push_back(std::move(info));

    } while (Process32NextW(snapshot, &entry));

    // 5. Đóng handle
    CloseHandle(snapshot);
#endif

    return result;
}

// ============================================================================
// findProcessesByName — Lọc processes theo tên
//
// Case-insensitive comparison vì Windows file system là case-insensitive.
// ============================================================================

std::vector<ProcessInfo> ProcessManager::findProcessesByName(const std::wstring& name) {
    auto all = enumerateProcesses();
    std::vector<ProcessInfo> result;

    // Convert name to lowercase cho so sánh
    std::wstring nameLower = name;
    std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::towlower);

    for (auto& proc : all) {
        std::wstring exeLower = proc.exeNameW;
        std::transform(exeLower.begin(), exeLower.end(), exeLower.begin(), ::towlower);

        if (exeLower == nameLower) {
            result.push_back(std::move(proc));
        }
    }

    return result;
}

// ============================================================================
// isProcessRunning
//
// GetExitCodeProcess:
//   - Nếu process chưa exit → trả về STILL_ACTIVE (259)
//   - Nếu process đã exit → trả về exit code thực
//
// Lưu ý: Nếu process exit với code 259, hàm này sẽ sai!
// Nhưng đây là convention chuẩn của Windows API.
// ============================================================================

bool ProcessManager::isProcessRunning(uint32_t pid) {
#ifdef _WIN32
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return false;

    DWORD exitCode = 0;
    bool alive = GetExitCodeProcess(proc, &exitCode) && exitCode == STILL_ACTIVE;
    CloseHandle(proc);
    return alive;
#else
    return false;
#endif
}

// ============================================================================
// openProcess — Mở process handle
//
// Access rights là bitmask — có thể OR nhiều quyền:
//   PROCESS_VM_READ          (0x0010) : ReadProcessMemory
//   PROCESS_VM_WRITE         (0x0020) : WriteProcessMemory
//   PROCESS_VM_OPERATION     (0x0008) : VirtualAllocEx, VirtualProtectEx
//   PROCESS_QUERY_INFORMATION(0x0400) : GetExitCodeProcess, ...
//   PROCESS_ALL_ACCESS       (0x1F0FFF): tất cả quyền
//
// Nếu target chạy elevated (Run as Administrator) mà caller thì không
// → ERROR_ACCESS_DENIED (5)
// ============================================================================

#ifdef _WIN32
HANDLE ProcessManager::openProcess(uint32_t pid, uint32_t access) {
    HANDLE handle = OpenProcess(access, FALSE, pid);
    if (!handle) {
        spdlog::error("OpenProcess failed for PID {}: error {}", pid, GetLastError());
    }
    return handle;
}
#endif

// ============================================================================
// getProcessImagePath — Lấy đường dẫn exe
//
// K32GetProcessImageFileNameW trả về đường dẫn device format:
//   "\Device\HarddiskVolume3\Program Files\Roblox\RobloxPlayerBeta.exe"
//
// GetModuleFileNameExW trả về đường dẫn DOS format:
//   "C:\Program Files\Roblox\RobloxPlayerBeta.exe"
// ============================================================================

std::optional<std::string> ProcessManager::getProcessImagePath(uint32_t pid) {
#ifdef _WIN32
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return std::nullopt;

    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;

    // Thử GetModuleFileNameW trước (cho DOS path)
    if (K32GetProcessImageFileNameW(proc, path, size)) {
        CloseHandle(proc);
        // Convert wide → narrow
        int len = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
        std::string result(len - 1, 0);
        WideCharToMultiByte(CP_UTF8, 0, path, -1, result.data(), len, nullptr, nullptr);
        return result;
    }

    CloseHandle(proc);
    return std::nullopt;
#else
    return std::nullopt;
#endif
}

// ============================================================================
// readMemory — Đọc memory từ process khác
//
// ReadProcessMemory:
//   - hProcess: handle của target process (từ OpenProcess)
//   - lpBaseAddress: address trong target process
//   - lpBuffer: buffer local để chứa data
//   - nSize: số bytes cần đọc
//   - lpNumberOfBytesRead: [out] số bytes thực sự đọc được
//
// Lỗi thường gặp:
//   - ERROR_PARTIAL_COPY (299): đọc được 1 phần (vùng nhớ không hoàn toàn readable)
//   - ERROR_NOACCESS (998): vùng nhớ protected (PAGE_NOACCESS hoặc PAGE_GUARD)
// ============================================================================

#ifdef _WIN32
size_t ProcessManager::readMemory(HANDLE process, uintptr_t address,
                                  void* buffer, size_t size) {
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address),
                           buffer, size, &bytesRead)) {
        // Không log mỗi lần fail — quá nhiều calls trong pattern scanning
        return 0;
    }
    return static_cast<size_t>(bytesRead);
}
#endif

// ============================================================================
// writeMemory — Ghi memory vào process khác
//
// Tương tự ReadProcessMemory nhưng ghi thay vì đọc.
// Yêu cầu PROCESS_VM_WRITE + PROCESS_VM_OPERATION.
//
// Lưu ý: Vùng nhớ đích phải có PAGE_READWRITE hoặc PAGE_EXECUTE_READWRITE.
// Nếu không, cần gọi VirtualProtectEx trước để thay đổi protection.
// ============================================================================

#ifdef _WIN32
size_t ProcessManager::writeMemory(HANDLE process, uintptr_t address,
                                   const void* data, size_t size) {
    SIZE_T bytesWritten = 0;
    if (!WriteProcessMemory(process, reinterpret_cast<LPVOID>(address),
                            data, size, &bytesWritten)) {
        spdlog::error("WriteProcessMemory failed at {:#x}: error {}",
                      address, GetLastError());
        return 0;
    }
    return static_cast<size_t>(bytesWritten);
}
#endif

// ============================================================================
// allocateMemory — Cấp phát memory trong process khác
//
// VirtualAllocEx:
//   - hProcess: target process
//   - lpAddress: NULL = OS chọn address
//   - dwSize: số bytes
//   - flAllocationType: MEM_COMMIT | MEM_RESERVE
//       MEM_RESERVE: đặt chỗ address space
//       MEM_COMMIT: thực sự cấp physical memory
//       Thường dùng cả 2 cùng lúc
//   - flProtect: protection flags
//       PAGE_READWRITE (0x04): đọc/ghi
//       PAGE_EXECUTE_READWRITE (0x40): đọc/ghi/execute (cho code injection)
//
// @return Address trong target process
// ============================================================================

#ifdef _WIN32
uintptr_t ProcessManager::allocateMemory(HANDLE process, size_t size,
                                          uint32_t protection) {
    LPVOID addr = VirtualAllocEx(process, nullptr, size,
                                 MEM_COMMIT | MEM_RESERVE, protection);
    if (!addr) {
        spdlog::error("VirtualAllocEx failed: error {}", GetLastError());
        return 0;
    }
    return reinterpret_cast<uintptr_t>(addr);
}
#endif

// ============================================================================
// queryMemory — Query thông tin vùng nhớ
//
// VirtualQueryEx trả về MEMORY_BASIC_INFORMATION:
//   - BaseAddress: address bắt đầu vùng nhớ
//   - RegionSize: kích thước
//   - State: MEM_COMMIT / MEM_RESERVE / MEM_FREE
//   - Protect: PAGE_READWRITE / PAGE_EXECUTE_READ / ...
//   - Type: MEM_PRIVATE / MEM_MAPPED / MEM_IMAGE
//
// Rất hữu ích cho:
//   - Pattern scanning: liệt kê tất cả committed regions
//   - Memory mapping: hiểu layout memory của process
// ============================================================================

#ifdef _WIN32
std::optional<MemoryRegionInfo> ProcessManager::queryMemory(
    HANDLE process, uintptr_t address
) {
    MEMORY_BASIC_INFORMATION memInfo{};
    if (VirtualQueryEx(process, reinterpret_cast<LPCVOID>(address),
                       &memInfo, sizeof(memInfo)) == 0) {
        return std::nullopt;
    }

    MemoryRegionInfo info;
    info.baseAddress = reinterpret_cast<uintptr_t>(memInfo.BaseAddress);
    info.regionSize  = memInfo.RegionSize;
    info.protect     = memInfo.Protect;
    info.state       = memInfo.State;
    info.type        = memInfo.Type;
    return info;
}
#endif

// ============================================================================
// Utility
// ============================================================================

uint32_t ProcessManager::getCurrentProcessId() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return 0;
#endif
}

#ifdef _WIN32
HWND ProcessManager::getConsoleWindow() {
    return GetConsoleWindow();
}
#endif

} // namespace xeno
