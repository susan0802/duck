// ============================================================================
// DllExports.cpp — Triển khai 6 hàm export của DLL
//
// Đây là "bộ mặt" của DLL — các hàm mà chương trình bên ngoài gọi.
// Mỗi hàm export là một thin wrapper gọi xuống các engine bên dưới.
//
// DllMain: Entry point khi DLL được load/unload.
//   - DLL_PROCESS_ATTACH: DLL vừa được load vào process
//   - DLL_PROCESS_DETACH: DLL sắp bị unload
//   - DLL_THREAD_ATTACH/DETACH: thread mới tạo/huỷ trong process
// ============================================================================

#include "core/DllExports.h"
#include "core/XenoConfig.h"
#include "server/XenoServer.h"
#include "server/WebSocketManager.h"
#include "client/ClientManager.h"
#include "utils/HttpClient.h"
#include "utils/Base64.h"
#include "compression/CompressionEngine.h"
#include "crypto/CryptoEngine.h"

#include <memory>
#include <string>
#include <spdlog/spdlog.h>

// ============================================================================
// Global instances — sống suốt vòng đời DLL
// unique_ptr để kiểm soát thứ tự khởi tạo/huỷ
// ============================================================================

static std::unique_ptr<xeno::ClientManager>    g_clientManager;
static std::unique_ptr<xeno::WebSocketManager> g_wsManager;
static std::unique_ptr<xeno::XenoServer>       g_server;
static bool g_initialized = false;

// Buffer tĩnh cho Version() — không cần free
static std::string g_versionStr;

// Buffer cho GetClients() — caller cần free
static std::string g_clientsBuffer;

// ============================================================================
// DllMain — Entry point của DLL
// ============================================================================

#ifdef _WIN32
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            // DLL vừa được load vào process
            // DisableThreadLibraryCalls: tối ưu — không cần notify khi thread tạo/huỷ
            DisableThreadLibraryCalls(hModule);
            spdlog::info("Xeno DLL loaded");
            break;

        case DLL_PROCESS_DETACH:
            // DLL sắp unload — cleanup
            spdlog::info("Xeno DLL unloading");
            if (g_server) g_server->stop();
            if (g_wsManager) g_wsManager->shutdown();
            if (g_clientManager) g_clientManager->stopScanning();
            g_server.reset();
            g_wsManager.reset();
            g_clientManager.reset();
            break;

        case DLL_THREAD_ATTACH:
        case DLL_THREAD_DETACH:
            // Không xử lý — đã DisableThreadLibraryCalls
            break;
    }
    return TRUE;
}
#endif

// ============================================================================
// Initialize — Khởi tạo toàn bộ hệ thống
// ============================================================================

XENO_API bool Initialize() {
    if (g_initialized) {
        spdlog::warn("Initialize() called but already initialized");
        return true;
    }

    try {
        auto& config = xeno::XenoConfig::instance();
        spdlog::info("Xeno Version: {}", config.version());

        spdlog::info("Attempting to retrieve RBXClient data...");
        xeno::HttpClient httpClient;

        std::string offsetsJson;
        bool dataRetrieved = false;

        // Thử primary endpoint: ep.zst.enc = Base64(Zstd(JSON))
        auto response = httpClient.downloadBinary(config.primaryEndpoint());
        if (response.has_value()) {
            // Flow: Download → Base64 Decode → Zstd Decompress → JSON
            std::string responseStr(response->begin(), response->end());
            auto decoded = xeno::Base64::decode(responseStr);

            if (decoded.has_value()) {
                auto decompressed = xeno::CompressionEngine::zstdDecompress(decoded.value());
                if (decompressed.has_value()) {
                    offsetsJson.assign(decompressed->begin(), decompressed->end());
                    spdlog::info("Successfully retrieved RBXClient data (Primary)");
                    dataRetrieved = true;
                } else {
                    spdlog::warn("RBXClient data resource decompression failed "
                                 "(It is likely corrupted). Using secondary endpoint");
                }
            } else {
                spdlog::warn("RBXClient data base64 decode failed. Using secondary endpoint");
            }
        } else {
            spdlog::warn("Primary endpoint failed. Trying secondary...");
        }

        // Fallback: secondary endpoint trả về plain JSON
        if (!dataRetrieved) {
            auto secondary = httpClient.downloadBinary(config.secondaryEndpoint());
            if (secondary.has_value()) {
                offsetsJson.assign(secondary->begin(), secondary->end());
                spdlog::info("Successfully retrieved RBXClient data from endpoint (Secondary)");
                dataRetrieved = true;
            } else {
                spdlog::error("Failed to retrieve RBXClient data from secondary endpoint");
            }
        }

        if (!dataRetrieved || offsetsJson.empty()) {
            spdlog::error("RBXClient data is missing due to corruption or bad endpoint");
            return false;
        }

        // ---- Bước 2: Khởi tạo Client Manager ----
        g_clientManager = std::make_unique<xeno::ClientManager>();
        g_clientManager->loadOffsets(offsetsJson);
        g_clientManager->loadCache();       // Khôi phục state từ cache
        g_clientManager->startScanning();   // Bắt đầu quét Roblox processes

        // ---- Bước 3: Khởi tạo WebSocket Manager ----
        g_wsManager = std::make_unique<xeno::WebSocketManager>();
        if (!g_wsManager->initialize()) {
            spdlog::error("Failed to initialize IXWebSocket network system. "
                          "Xeno WebSocket library will not work!");
            // Vẫn tiếp tục — một số tính năng vẫn hoạt động không cần WS
        }

        // ---- Bước 4: Khởi động HTTP Server ----
        //
        // Server lắng nghe trên 127.0.0.1 (localhost only — an toàn)
        // UI gửi lệnh đến server qua HTTP requests
        //
        g_server = std::make_unique<xeno::XenoServer>(*g_clientManager, *g_wsManager);
        if (!g_server->start(config.serverPort())) {
            spdlog::error("Failed to start HTTP server on port {}", config.serverPort());
            return false;
        }

        spdlog::info("Xeno initialized successfully on port {}", config.serverPort());

        // In thông tin phiên bản các thư viện
        spdlog::info("OpenSSL Version:          {}", "OpenSSL 3.x");
        spdlog::info("cURL Version:             {}", httpClient.curlVersion());
        spdlog::info("LZ4 Version:              {}", xeno::CompressionEngine::lz4Version());
        spdlog::info("Meta@Zstandard Version:   {}", xeno::CompressionEngine::zstdVersion());

        g_initialized = true;
        return true;

    } catch (const std::exception& e) {
        spdlog::error("Initialize failed with exception: {}", e.what());
        return false;
    }
}

// ============================================================================
// Attach — Gắn vào Roblox client process
// ============================================================================

XENO_API bool Attach(unsigned long pid) {
    if (!g_initialized || !g_clientManager) {
        spdlog::error("Cannot attach: Xeno not initialized");
        return false;
    }

    spdlog::info("Attempting to attach to PID: {}", pid);
    return g_clientManager->attachClient(static_cast<uint32_t>(pid));
}

// ============================================================================
// Execute — Thực thi Luau script
// ============================================================================

XENO_API bool Execute(unsigned long pid, const char* script) {
    if (!g_initialized || !g_clientManager) {
        spdlog::error("Cannot execute: Xeno not initialized");
        return false;
    }

    if (!script) {
        spdlog::error("Execute called with null script");
        return false;
    }

    auto client = g_clientManager->getClient(static_cast<uint32_t>(pid));
    if (!client) {
        spdlog::error("Execute: client PID {} not found", pid);
        return false;
    }

    if (!client->isAttached()) {
        spdlog::error("Execute: client PID {} is not attached", pid);
        return false;
    }

    return client->executeScript(script);
}

// ============================================================================
// GetClients — Danh sách clients dạng JSON
// ============================================================================

XENO_API const char* GetClients() {
    if (!g_initialized || !g_clientManager) {
        g_clientsBuffer = "[]";
        return g_clientsBuffer.c_str();
    }

    // toJson() trả về JSON array string
    g_clientsBuffer = g_clientManager->toJson();
    return g_clientsBuffer.c_str();
}

// ============================================================================
// SetSetting — Thay đổi cấu hình
// ============================================================================

XENO_API bool SetSetting(const char* key, const char* value) {
    if (!key || !value) return false;

    auto& config = xeno::XenoConfig::instance();
    std::string k(key);
    std::string v(value);

    if (k == "Port") {
        int port = std::stoi(v);
        config.setServerPort(port);
        spdlog::info("Setting changed: Port = {}", port);
        return true;
    }

    if (k == "AutoExecDir") {
        config.setAutoExecDirectory(v);
        spdlog::info("Setting changed: AutoExecDir = {}", v);
        return true;
    }

    if (k == "MaxWebSocketConnections") {
        config.setMaxWebSocketConnections(std::stoi(v));
        return true;
    }

    spdlog::warn("Unknown setting key: {}", k);
    return false;
}

// ============================================================================
// Version — Trả về version string
// ============================================================================

XENO_API const char* Version() {
    g_versionStr = xeno::XenoConfig::instance().version();
    return g_versionStr.c_str();
}
