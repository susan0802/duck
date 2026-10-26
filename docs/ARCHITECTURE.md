# Kiến trúc chi tiết — Xeno DLL

## 1. Tổng quan

Xeno hoạt động như một **bridge** (cầu nối) giữa giao diện người dùng (UI — viết bằng .NET/C#)
và Roblox client processes. DLL được load bởi ứng dụng .NET, sau đó tạo một HTTP server cục bộ
để UI gửi lệnh xuống.

```
┌──────────────┐     HTTP/WS      ┌──────────────┐     Memory R/W     ┌──────────────┐
│   .NET UI    │ ◄──────────────► │   Xeno.dll   │ ◄────────────────► │ Roblox.exe   │
│  (Frontend)  │   127.0.0.1:port │  (Backend)   │   Win32 API        │  (Target)    │
└──────────────┘                  └──────────────┘                    └──────────────┘
```

## 2. Luồng khởi tạo (Initialize Flow)

```
Initialize()
    │
    ├── 1. Load cấu hình (XenoConfig)
    │       └── Đọc settings: port, auto-execute directory, ...
    │
    ├── 2. Tải offsets data (HttpClient + CompressionEngine)
    │       ├── GET https://x3no.pages.dev/ep.zst.enc
    │       ├── Decrypt (AES-256) → Decompress (Zstd) → Parse JSON
    │       └── Fallback: endpoint phụ nếu chính lỗi
    │
    ├── 3. Khởi tạo ProcessManager
    │       └── Quét Roblox processes đang chạy
    │
    ├── 4. Start HTTP Server (XenoServer)
    │       ├── Bind 127.0.0.1:{port}
    │       ├── Đăng ký route handlers
    │       └── Chạy trên thread riêng
    │
    └── 5. Start WebSocket Server
            └── Lắng nghe connections từ injected clients
```

## 3. HTTP Server — Route Handlers

XenoServer sử dụng `cpp-httplib` và giao tiếp bằng query parameters.
Header `Content-Type: application/json` cho response. CORS header cho phép UI truy cập.

### Các nhóm endpoint (suy luận từ parameter validation messages):

| Nhóm | Parameter `c` | Mô tả |
|------|--------------|-------|
| Script | Script Name | Loadstring, execute Luau scripts |
| Console | Console Method | RConsole create/print/clear |
| Crypt | Crypt Method | Encrypt/decrypt data |
| Base64 | Base64 Method | Encode/decode Base64 |
| WebSocket | WebSocket Method | WS open/send/close |
| Lz4 | Lz4 Method | Compress/decompress LZ4 |
| FileSystem | Path | ReadFile/WriteFile/MakeFolder/... |
| Instance | Instance Pointer | Get/set instance properties |
| Process | PID / Process ID | Attach/detach processes |

### Request format (query params):
```
GET /?c=<method>&p=<pid>&e=<extra_data>&...
```

### Response format:
```json
{
    "success": true,
    "data": "..."
}
```

## 4. RBXClient — Quản lý Roblox Process

Mỗi Roblox process được đại diện bởi một `RBXClient` instance:

```cpp
class RBXClient {
    DWORD       pid_;              // Process ID
    HANDLE      processHandle_;    // OpenProcess handle
    uintptr_t   dataModel_;        // Address of DataModel in target
    uintptr_t   renderView_;       // Address of RenderView
    std::string version_;          // Roblox client version
    bool        attached_;         // Đã inject chưa
    // ...
};
```

### Lifecycle của RBXClient:

```
Process detected
    │
    ├── OpenProcess(PROCESS_ALL_ACCESS)
    │
    ├── ReadProcessMemory → Tìm RenderView
    │       └── Từ RenderView → Tìm DataModel address
    │
    ├── Apply offsets (version-specific)
    │       └── Offsets map: version → {dataModel, renderView, scriptContext, ...}
    │
    ├── Inject (VirtualAllocEx + WriteProcessMemory)
    │
    ├── Wait for ping (xác nhận inject thành công)
    │
    └── Ready → Có thể Execute scripts
```

### PlayerRemoving (Auto-reattach):
Khi player rời game, Xeno tự động reattach:
```
PlayerRemoving signal
    │
    ├── Wait for new DataModel (game mới)
    ├── Wait for CoreGui loaded
    └── Re-inject
```

## 5. WebSocket Communication

IXWebSocket được dùng cho giao tiếp real-time giữa DLL và injected code trong Roblox:

```
Xeno.dll (WS Server)  ◄────────►  Roblox Client (WS Client)
    │                                    │
    ├── Send script to execute           │
    ├── Receive execution results  ◄─────┤
    ├── Receive game signals       ◄─────┤
    └── Ping/pong heartbeat        ◄────►┘
```

- Max connections có giới hạn (log: "Max WebSocket Connections reached")
- Có WebSocket Cleaner thread dọn dẹp dead connections
- UID dùng để identify từng connection

## 6. Crypto Engine

OpenSSL 3 được sử dụng cho:

### Encryption (AES-256):
```
Supported modes: GCM, CCM, CBC, CTR, ECB, OFB, OCB, CFB128, XTS
```

### Hashing:
```
MD5, SHA1, SHA224, SHA256, SHA384, SHA512, SHA512/224, SHA512/256
SHA3-224, SHA3-256, SHA3-384, SHA3-512
```

### TLS:
- Client & Server TLS support
- Certificate verification
- Custom CA certificates

## 7. Compression Engine

Hai thuật toán nén:

### LZ4:
- Compress/decompress nhanh
- Dùng cho data nhỏ, cần tốc độ

### Zstandard (Zstd):
- Tỉ lệ nén cao hơn LZ4
- Dùng cho resource files (offsets data)

## 8. Process Manager (Windows API)

```cpp
// Quét processes
CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
Process32FirstW / Process32NextW

// Thao tác memory
OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid)
ReadProcessMemory(hProcess, baseAddr, buffer, size, &bytesRead)
WriteProcessMemory(hProcess, baseAddr, data, size, &bytesWritten)
VirtualAllocEx(hProcess, NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)
VirtualQueryEx(hProcess, addr, &memInfo, sizeof(memInfo))

// Thông tin process
GetModuleFileNameW / GetModuleFileNameA
K32GetProcessImageFileNameW
GetProcessTimes
GetExitCodeProcess
TerminateProcess
```

## 9. Caching System

Xeno cache dữ liệu để tăng tốc:
- RBXClient info (PID → client data)
- RBXBytecode (compiled Luau bytecode)
- RBXMFlags (memory flags/states)
- RBXClientInfo (version, offsets, ...)

Log: `"Loaded existing cache: {} RBXClients, {} RBXBytecode, {} RBXMFlags, {} RBXClientInfos"`

## 10. Auto-Execute

Khi client được attach, Xeno tự động chạy scripts từ thư mục `autoexec`:
```
Auto Execute Directory: <path>
Executed {} script(s) from autoexec directory
```

## 11. Environment Variables (thay thế trong scripts)

| Variable | Ý nghĩa |
|----------|---------|
| `%XENO_VERSION%` | Phiên bản Xeno |
| `%XENO_SERVER_PORT%` | Port HTTP server |
| `%XENO_CLIENT_PID%` | PID của Roblox client |
| `%ROBLOX_CLIENT_VERSION%` | Version Roblox |

## 12. Error Handling

Xeno sử dụng `spdlog` cho logging với format:
```
[PID (version)][::MethodName]: Message
```

Ví dụ:
```
[12345 (0.620.0.6200589)][::Loadstring]: DataModel was finished loading
[12345 (0.620.0.6200589)][::PlayerRemoving]: Player leaving ping received
```
