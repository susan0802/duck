// ============================================================================
// XenoServer.cpp — HTTP Server chính
//
// Sử dụng cpp-httplib (https://github.com/yhirose/cpp-httplib).
// Server lắng nghe trên 127.0.0.1 (chỉ local) và xử lý commands
// từ UI thông qua query parameters.
//
// Tất cả routes đăng ký trong start().
// Mỗi route handler:
//   1. Đọc query params
//   2. Validate params
//   3. Gọi engine tương ứng
//   4. Trả JSON response
//
// cpp-httplib tự quản lý thread pool cho concurrent requests.
// ============================================================================

#include <httplib.h>

#include "server/XenoServer.h"
#include "client/ClientManager.h"
#include "server/WebSocketManager.h"
#include "core/XenoConfig.h"
#include "crypto/CryptoEngine.h"
#include "crypto/HashEngine.h"
#include "compression/CompressionEngine.h"
#include "utils/Base64.h"

#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <fstream>

using json = nlohmann::json;

namespace xeno {

// ============================================================================
// Constructor / Destructor
// ============================================================================

XenoServer::XenoServer(ClientManager& clientManager, WebSocketManager& wsManager)
    : clientManager_(clientManager)
    , wsManager_(wsManager)
{}

XenoServer::~XenoServer() {
    stop();
}

// ============================================================================
// start — Khởi động server
// ============================================================================

bool XenoServer::start(int port) {
    if (running_) {
        spdlog::warn("Server already running on port {}", port_);
        return false;
    }

    server_ = std::make_unique<httplib::Server>();
    port_ = port;

    // Đăng ký tất cả route handlers
    registerScriptRoutes();
    registerFileSystemRoutes();
    registerCryptoRoutes();
    registerBase64Routes();
    registerConsoleRoutes();
    registerProcessRoutes();
    registerWebSocketRoutes();
    registerCompressionRoutes();
    registerInstanceRoutes();
    registerMiscRoutes();

    // Chạy server trên thread riêng
    // server->listen() là blocking call — chạy vòng lặp accept() liên tục
    running_ = true;
    serverThread_ = std::thread([this, port]() {
        spdlog::info("HTTP server starting on 127.0.0.1:{}", port);
        server_->listen("127.0.0.1", port);
        spdlog::info("HTTP server stopped");
    });

    return true;
}

// ============================================================================
// stop — Dừng server
// ============================================================================

void XenoServer::stop() {
    if (!running_) return;
    running_ = false;

    if (server_) server_->stop();

    if (serverThread_.joinable()) {
        serverThread_.join();
    }

    server_.reset();
    spdlog::info("Server stopped");
}

bool XenoServer::isRunning() const {
    return running_;
}

// ============================================================================
// Route Handlers
// ============================================================================

void XenoServer::registerScriptRoutes() {
    // Route chính — tất cả commands đi qua "/" với query parameter "c"
    server_->Get("/", [this](const httplib::Request& req, httplib::Response& res) {
        addCorsHeaders(res);

        std::string methodId = getParam(req, "c");
        if (methodId.empty()) {
            res.set_content(makeErrorResponse("Missing 'c' parameter: Method ID"),
                            "application/json");
            return;
        }

        std::string pid = getParam(req, "p");
        if (pid.empty()) {
            res.set_content(makeErrorResponse("Missing 'p' parameter: PID"),
                            "application/json");
            return;
        }

        auto client = clientManager_.getClient(std::stoul(pid));
        if (!client) {
            res.set_content(makeErrorResponse("Client not found"), "application/json");
            return;
        }

        // Route dựa trên method ID
        if (methodId == "Loadstring") {
            std::string script = getParam(req, "e");
            if (script.empty()) {
                res.set_content(makeErrorResponse("Missing 'e' parameter: Script"),
                                "application/json");
                return;
            }
            // Decode base64 script → send to client for execution
            auto decoded = Base64::decodeToString(script);
            if (!decoded) {
                res.set_content(makeErrorResponse("Invalid base64 script"),
                                "application/json");
                return;
            }
            // client->executeScript(*decoded);
            res.set_content(makeSuccessResponse("Script queued"), "application/json");
        }
        else {
            res.set_content(makeErrorResponse("Unknown method: " + methodId),
                            "application/json");
        }
    });

    spdlog::debug("Script routes registered");
}

void XenoServer::registerFileSystemRoutes() {
    server_->Get("/readfile", [this](const httplib::Request& req, httplib::Response& res) {
        addCorsHeaders(res);
        std::string path = getParam(req, "c");
        if (path.empty()) {
            res.set_content(makeErrorResponse("Missing 'c' parameter: Path"),
                            "application/json");
            return;
        }

        std::ifstream file(path);
        if (!file.is_open()) {
            res.set_content(makeErrorResponse("File not found"), "application/json");
            return;
        }

        std::string content((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
        res.set_content(makeSuccessResponse(content), "application/json");
    });

    server_->Get("/writefile", [this](const httplib::Request& req, httplib::Response& res) {
        addCorsHeaders(res);
        std::string path = getParam(req, "c");
        std::string data = getParam(req, "e");
        if (path.empty()) {
            res.set_content(makeErrorResponse("Missing path"), "application/json");
            return;
        }

        std::ofstream file(path);
        if (!file.is_open()) {
            res.set_content(makeErrorResponse("Cannot open file for writing"),
                            "application/json");
            return;
        }
        file << data;
        res.set_content(makeSuccessResponse("Written"), "application/json");
    });

    spdlog::debug("FileSystem routes registered");
}

void XenoServer::registerCryptoRoutes() {
    server_->Get("/crypt", [this](const httplib::Request& req, httplib::Response& res) {
        addCorsHeaders(res);
        std::string method = getParam(req, "c");
        if (method.empty()) {
            res.set_content(makeErrorResponse("Missing 'c' parameter: Crypt Method"),
                            "application/json");
            return;
        }

        CryptoEngine crypto;

        if (method == "encrypt") {
            std::string data = getParam(req, "e");
            std::string keyB64 = getParam(req, "k");
            std::string ivB64 = getParam(req, "i");
            std::string modeStr = getParam(req, "m");

            auto keyBytes = Base64::decode(keyB64);
            auto ivBytes = Base64::decode(ivB64);
            auto mode = parseCipherMode(modeStr.empty() ? "cbc" : modeStr);

            if (!keyBytes || !ivBytes || !mode) {
                res.set_content(makeErrorResponse("Invalid crypto parameters"),
                                "application/json");
                return;
            }

            auto result = crypto.encrypt(data, *keyBytes, *ivBytes, *mode);
            if (result) {
                res.set_content(makeSuccessResponse(Base64::encode(*result)),
                                "application/json");
            } else {
                res.set_content(makeErrorResponse("Encryption failed"),
                                "application/json");
            }
        }
        else if (method == "decrypt") {
            std::string dataB64 = getParam(req, "e");
            std::string keyB64 = getParam(req, "k");
            std::string ivB64 = getParam(req, "i");
            std::string modeStr = getParam(req, "m");

            auto dataBytes = Base64::decode(dataB64);
            auto keyBytes = Base64::decode(keyB64);
            auto ivBytes = Base64::decode(ivB64);
            auto mode = parseCipherMode(modeStr.empty() ? "cbc" : modeStr);

            if (!dataBytes || !keyBytes || !ivBytes || !mode) {
                res.set_content(makeErrorResponse("Invalid crypto parameters"),
                                "application/json");
                return;
            }

            auto result = crypto.decrypt(*dataBytes, *keyBytes, *ivBytes, *mode);
            if (result) {
                res.set_content(makeSuccessResponse(*result), "application/json");
            } else {
                res.set_content(makeErrorResponse("Decryption failed"),
                                "application/json");
            }
        }
        else if (method == "hash") {
            std::string data = getParam(req, "e");
            std::string algoStr = getParam(req, "a");
            auto algo = parseHashAlgorithm(algoStr.empty() ? "sha256" : algoStr);
            if (!algo) {
                res.set_content(makeErrorResponse("Invalid hash algorithm"),
                                "application/json");
                return;
            }

            HashEngine hasher;
            res.set_content(makeSuccessResponse(hasher.hashToHex(data, *algo)),
                            "application/json");
        }
        else if (method == "random") {
            std::string countStr = getParam(req, "e");
            size_t count = countStr.empty() ? 32 : std::stoul(countStr);
            auto bytes = CryptoEngine::generateRandomBytes(count);
            res.set_content(makeSuccessResponse(Base64::encode(bytes)),
                            "application/json");
        }
        else {
            res.set_content(makeErrorResponse("Invalid crypt method: " + method),
                            "application/json");
        }
    });

    spdlog::debug("Crypto routes registered");
}

void XenoServer::registerBase64Routes() {
    server_->Get("/b64", [this](const httplib::Request& req, httplib::Response& res) {
        addCorsHeaders(res);
        std::string method = getParam(req, "c");

        if (method == "encode") {
            std::string data = getParam(req, "e");
            res.set_content(makeSuccessResponse(Base64::encode(data)),
                            "application/json");
        }
        else if (method == "decode") {
            std::string data = getParam(req, "e");
            auto decoded = Base64::decodeToString(data);
            if (decoded) {
                res.set_content(makeSuccessResponse(*decoded), "application/json");
            } else {
                res.set_content(makeErrorResponse("Invalid base64 encoded data"),
                                "application/json");
            }
        }
        else {
            res.set_content(makeErrorResponse("Invalid base64 method"),
                            "application/json");
        }
    });

    spdlog::debug("Base64 routes registered");
}

void XenoServer::registerConsoleRoutes() {
    server_->Get("/rconsole", [this](const httplib::Request& req, httplib::Response& res) {
        addCorsHeaders(res);
        std::string method = getParam(req, "c");

#ifdef _WIN32
        if (method == "create") {
            AllocConsole();
            res.set_content(makeSuccessResponse("Console created"), "application/json");
        }
        else if (method == "destroy") {
            FreeConsole();
            res.set_content(makeSuccessResponse("Console destroyed"), "application/json");
        }
        else if (method == "print") {
            std::string text = getParam(req, "e");
            HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
            DWORD written;
            WriteConsoleA(hConsole, text.c_str(),
                          static_cast<DWORD>(text.size()), &written, nullptr);
            res.set_content(makeSuccessResponse("Printed"), "application/json");
        }
        else if (method == "title") {
            std::string title = getParam(req, "e");
            SetConsoleTitleA(title.c_str());
            res.set_content(makeSuccessResponse("Title set"), "application/json");
        }
        else
#endif
        {
            res.set_content(makeErrorResponse("Invalid console method"),
                            "application/json");
        }
    });

    spdlog::debug("Console routes registered");
}

void XenoServer::registerProcessRoutes() {
    server_->Get("/clients", [this](const httplib::Request& /*req*/, httplib::Response& res) {
        addCorsHeaders(res);
        res.set_content(clientManager_.toJson(), "application/json");
    });

    server_->Get("/attach", [this](const httplib::Request& req, httplib::Response& res) {
        addCorsHeaders(res);
        std::string pidStr = getParam(req, "p");
        if (pidStr.empty()) {
            res.set_content(makeErrorResponse("Missing PID"), "application/json");
            return;
        }
        uint32_t pid = std::stoul(pidStr);
        bool ok = clientManager_.attachClient(pid);
        if (ok) {
            res.set_content(makeSuccessResponse("Attached"), "application/json");
        } else {
            res.set_content(makeErrorResponse("Attach failed"), "application/json");
        }
    });

    spdlog::debug("Process routes registered");
}

void XenoServer::registerWebSocketRoutes() {
    server_->Get("/websocket", [this](const httplib::Request& req, httplib::Response& res) {
        addCorsHeaders(res);
        std::string method = getParam(req, "c");
        std::string uid = getParam(req, "u");

        if (method == "open") {
            std::string url = getParam(req, "e");
            if (uid.empty() || url.empty()) {
                res.set_content(makeErrorResponse("Missing uid or url"),
                                "application/json");
                return;
            }
            bool ok = wsManager_.openConnection(uid, url);
            res.set_content(ok ? makeSuccessResponse("Opened")
                               : makeErrorResponse("Failed to open"),
                            "application/json");
        }
        else if (method == "send") {
            std::string msg = getParam(req, "e");
            bool ok = wsManager_.sendMessage(uid, msg);
            res.set_content(ok ? makeSuccessResponse("Sent")
                               : makeErrorResponse("Send failed"),
                            "application/json");
        }
        else if (method == "close") {
            wsManager_.closeConnection(uid);
            res.set_content(makeSuccessResponse("Closed"), "application/json");
        }
        else {
            res.set_content(makeErrorResponse("Invalid WebSocket method"),
                            "application/json");
        }
    });

    spdlog::debug("WebSocket routes registered");
}

void XenoServer::registerCompressionRoutes() {
    server_->Get("/lz4", [this](const httplib::Request& req, httplib::Response& res) {
        addCorsHeaders(res);
        std::string method = getParam(req, "c");
        std::string dataB64 = getParam(req, "e");

        if (method == "compress") {
            auto decoded = Base64::decode(dataB64);
            if (!decoded) {
                res.set_content(makeErrorResponse("Invalid base64 data"),
                                "application/json");
                return;
            }
            auto compressed = CompressionEngine::lz4Compress(decoded->data(),
                                                              decoded->size());
            if (compressed) {
                res.set_content(makeSuccessResponse(Base64::encode(*compressed)),
                                "application/json");
            } else {
                res.set_content(makeErrorResponse("LZ4 compression failed"),
                                "application/json");
            }
        }
        else if (method == "decompress") {
            auto decoded = Base64::decode(dataB64);
            std::string sizeStr = getParam(req, "s");
            if (!decoded || sizeStr.empty()) {
                res.set_content(makeErrorResponse("Missing data or size"),
                                "application/json");
                return;
            }
            size_t origSize = std::stoul(sizeStr);
            auto decompressed = CompressionEngine::lz4Decompress(
                decoded->data(), decoded->size(), origSize);
            if (decompressed) {
                res.set_content(makeSuccessResponse(Base64::encode(*decompressed)),
                                "application/json");
            } else {
                res.set_content(makeErrorResponse("LZ4 decompression failed"),
                                "application/json");
            }
        }
        else {
            res.set_content(makeErrorResponse("Invalid LZ4 method"),
                            "application/json");
        }
    });

    spdlog::debug("Compression routes registered");
}

void XenoServer::registerInstanceRoutes() {
    // Instance manipulation routes — GetProperties, SetScriptable, etc.
    // These interact directly with Roblox memory via RBXClient
    server_->Get("/instance", [this](const httplib::Request& req, httplib::Response& res) {
        addCorsHeaders(res);
        std::string method = getParam(req, "c");
        std::string pidStr = getParam(req, "p");

        if (pidStr.empty()) {
            res.set_content(makeErrorResponse("Missing PID"), "application/json");
            return;
        }

        auto client = clientManager_.getClient(std::stoul(pidStr));
        if (!client) {
            res.set_content(makeErrorResponse("Client not found"), "application/json");
            return;
        }

        // TODO: implement instance operations based on method
        res.set_content(makeErrorResponse("Not yet implemented: " + method),
                        "application/json");
    });

    spdlog::debug("Instance routes registered");
}

void XenoServer::registerMiscRoutes() {
    server_->Get("/version", [](const httplib::Request& /*req*/, httplib::Response& res) {
        json j;
        j["version"] = XenoConfig::instance().version();
        res.set_content(j.dump(), "application/json");
    });

    // CORS preflight handler
    server_->Options(".*", [this](const httplib::Request& /*req*/, httplib::Response& res) {
        addCorsHeaders(res);
        res.status = 204;
    });

    spdlog::debug("Misc routes registered");
}

// ============================================================================
// Helper Methods
// ============================================================================

std::string XenoServer::makeSuccessResponse(const std::string& data) {
    json j;
    j["success"] = true;
    j["data"] = data;
    return j.dump();
}

std::string XenoServer::makeErrorResponse(const std::string& message) {
    json j;
    j["success"] = false;
    j["error"] = message;
    return j.dump();
}

void XenoServer::addCorsHeaders(httplib::Response& res) {
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type, Clients");
}

std::string XenoServer::getParam(const httplib::Request& req, const std::string& key) {
    if (req.has_param(key)) return req.get_param_value(key);
    return "";
}

} // namespace xeno
