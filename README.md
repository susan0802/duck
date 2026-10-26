# Xeno DLL — Reverse-Engineered Educational C++ Project

> **Mục đích**: Project này được tái tạo (reverse-engineered) từ file `duck.dll` (tên gốc: `Xeno.dll`)
> nhằm mục đích **học hỏi kiến trúc phần mềm**, hiểu cách các thành phần tương tác với nhau.
> Đây **KHÔNG** phải mã nguồn gốc — nó được viết lại dựa trên phân tích binary.

## 📋 Tổng quan kiến trúc

Xeno là một DLL được thiết kế để giao tiếp với Roblox client thông qua kiến trúc client-server cục bộ (localhost).

```
┌─────────────────────────────────────────────────────────┐
│                     Xeno.dll                            │
│                                                         │
│  ┌──────────────┐  ┌──────────────┐  ┌───────────────┐ │
│  │  DLL Exports  │  │  HTTP Server │  │  WebSocket    │ │
│  │  (6 hàm)     │──│  (httplib)   │  │  Server (IX)  │ │
│  └──────────────┘  └──────┬───────┘  └───────┬───────┘ │
│                           │                   │         │
│  ┌──────────────┐  ┌──────┴───────┐  ┌───────┴───────┐ │
│  │ ProcessMgr   │  │ RBXClient    │  │  Crypto       │ │
│  │ (Win32 API)  │──│ Manager      │  │  (OpenSSL)    │ │
│  └──────────────┘  └──────────────┘  └───────────────┘ │
│                                                         │
│  ┌──────────────┐  ┌──────────────┐  ┌───────────────┐ │
│  │ HTTP Client  │  │ Compression  │  │  Utils        │ │
│  │ (libcurl)    │  │ (LZ4/Zstd)  │  │  (Base64...) │ │
│  └──────────────┘  └──────────────┘  └───────────────┘ │
└─────────────────────────────────────────────────────────┘
```

## 🔧 Các thư viện sử dụng

| Thư viện | Phiên bản | Mục đích |
|----------|-----------|----------|
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | — | HTTP Server (nhận lệnh từ UI) |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0 | Parse/serialize JSON |
| [IXWebSocket](https://github.com/machinezone/IXWebSocket) | — | WebSocket cho real-time communication |
| [spdlog](https://github.com/gabime/spdlog) | — | Logging |
| [fmt](https://github.com/fmtlib/fmt) | v12 | String formatting |
| OpenSSL 3 | libcrypto-3, libssl-3 | Mã hóa AES, hashing SHA, TLS |
| libcurl | — | HTTP client (tải offsets data) |
| LZ4 / Zstandard | — | Nén/giải nén dữ liệu |

## 📁 Cấu trúc thư mục

```
duck/
├── README.md                    # File này
├── CMakeLists.txt              # Build system
├── docs/
│   └── ARCHITECTURE.md         # Giải thích chi tiết kiến trúc
├── include/
│   ├── core/
│   │   ├── DllExports.h        # 6 hàm export của DLL
│   │   └── XenoConfig.h        # Cấu hình chung
│   ├── server/
│   │   ├── XenoServer.h        # HTTP Server chính
│   │   └── WebSocketManager.h  # Quản lý WebSocket connections
│   ├── client/
│   │   ├── RBXClient.h         # Đại diện 1 Roblox client process
│   │   └── ClientManager.h     # Quản lý nhiều RBXClient
│   ├── crypto/
│   │   ├── CryptoEngine.h      # AES encrypt/decrypt
│   │   └── HashEngine.h        # SHA, MD5 hashing
│   ├── process/
│   │   └── ProcessManager.h    # Windows process operations
│   ├── compression/
│   │   └── CompressionEngine.h # LZ4/Zstd compress/decompress
│   └── utils/
│       ├── Base64.h            # Base64 encode/decode
│       └── HttpClient.h        # libcurl wrapper
└── src/
    ├── core/
    │   ├── DllExports.cpp       # Entry points
    │   └── XenoConfig.cpp
    ├── server/
    │   ├── XenoServer.cpp       # HTTP route handlers
    │   └── WebSocketManager.cpp
    ├── client/
    │   ├── RBXClient.cpp        # Process attach/communication
    │   └── ClientManager.cpp
    ├── crypto/
    │   ├── CryptoEngine.cpp
    │   └── HashEngine.cpp
    ├── process/
    │   └── ProcessManager.cpp
    ├── compression/
    │   └── CompressionEngine.cpp
    └── utils/
        ├── Base64.cpp
        └── HttpClient.cpp
```

## 🔍 6 Hàm Export của DLL

| Hàm | Mô tả |
|-----|--------|
| `Initialize()` | Khởi tạo hệ thống: load offsets, start HTTP server |
| `Attach()` | Gắn vào một Roblox client process |
| `Execute()` | Thực thi script trên client đã attach |
| `GetClients()` | Trả về danh sách Roblox client đang chạy |
| `SetSetting()` | Cấu hình setting (port, auto-exec, ...) |
| `Version()` | Trả về version string của Xeno |

## 🔄 Flow hoạt động

1. **Initialize** → Tải offsets data từ remote endpoint → Decompress (Zstd) → Parse JSON
2. **GetClients** → Quét processes (CreateToolhelp32Snapshot) → Lọc Roblox processes
3. **Attach** → OpenProcess → ReadProcessMemory để tìm DataModel → Inject
4. **XenoServer** → Lắng nghe HTTP requests trên `127.0.0.1:{port}`
5. **WebSocket** → Kết nối real-time với Roblox client đã inject
6. **Execute** → Gửi script qua WebSocket → Client thực thi Luau code

## 🏗️ Build (yêu cầu)

- Windows 10/11 (x64)
- Visual Studio 2022 hoặc CMake 3.20+
- vcpkg (quản lý thư viện)

```bash
# Cài đặt dependencies qua vcpkg
vcpkg install openssl:x64-windows curl:x64-windows nlohmann-json:x64-windows
vcpkg install spdlog:x64-windows fmt:x64-windows lz4:x64-windows zstd:x64-windows

# Build
cmake -B build -DCMAKE_TOOLCHAIN_FILE=[vcpkg-root]/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

## ⚠️ Lưu ý

Đây là project **chỉ để học hỏi** về:
- Kiến trúc DLL và cách export functions
- Embedded HTTP/WebSocket servers
- Interprocess communication trên Windows
- Cryptography với OpenSSL
- Data compression với LZ4/Zstd
- Process memory read/write
