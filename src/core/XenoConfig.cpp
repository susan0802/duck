// ============================================================================
// XenoConfig.cpp — Triển khai Singleton cấu hình
//
// Thread-safety: mọi getter/setter đều lock mutex.
// C++11 garantees thread-safe initialization cho static local variables
// (§6.7 [stmt.dcl]), nên instance() không cần mutex riêng.
// ============================================================================

#include "core/XenoConfig.h"
#include <algorithm>
#include <sstream>

namespace xeno {

// ============================================================================
// Singleton — Meyer's Singleton Pattern
//
// static local variable được khởi tạo đúng 1 lần, thread-safe.
// Đây là pattern được khuyến nghị trong C++ hiện đại thay cho
// double-checked locking hay eager initialization.
// ============================================================================

XenoConfig& XenoConfig::instance() {
    static XenoConfig instance;
    return instance;
}

XenoConfig::XenoConfig() {
    // Đường dẫn auto-execute mặc định
    // Trong thực tế, sẽ dùng GetTempPathW hoặc user directory
    autoExecDir_ = std::filesystem::temp_directory_path() / "Xeno" / "autoexec";
}

// ---- Getters (thread-safe) ----

int XenoConfig::serverPort() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return serverPort_;
}

std::string XenoConfig::version() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return version_;
}

std::filesystem::path XenoConfig::autoExecDirectory() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return autoExecDir_;
}

std::string XenoConfig::primaryEndpoint() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return primaryEndpoint_;
}

std::string XenoConfig::secondaryEndpoint() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return secondaryEndpoint_;
}

int XenoConfig::maxWebSocketConnections() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return maxWsConnections_;
}

// ---- Setters (thread-safe) ----

void XenoConfig::setServerPort(int port) {
    std::lock_guard<std::mutex> lock(mutex_);
    serverPort_ = port;
}

void XenoConfig::setAutoExecDirectory(const std::filesystem::path& dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    autoExecDir_ = dir;
}

void XenoConfig::setMaxWebSocketConnections(int max) {
    std::lock_guard<std::mutex> lock(mutex_);
    maxWsConnections_ = max;
}

// ============================================================================
// expandEnvironmentVars — Thay thế placeholder trong string
//
// Xeno sử dụng pattern %VARIABLE_NAME% để inject giá trị runtime
// vào scripts và responses. Tương tự Windows environment variables.
//
// VD: "Version: %XENO_VERSION%" → "Version: 1.0.0"
// ============================================================================

std::string XenoConfig::expandEnvironmentVars(
    const std::string& input,
    uint32_t clientPid,
    const std::string& robloxVersion
) const {
    std::string result = input;

    // Helper: tìm và thay thế tất cả occurrences
    auto replaceAll = [](std::string& str, const std::string& from, const std::string& to) {
        size_t pos = 0;
        while ((pos = str.find(from, pos)) != std::string::npos) {
            str.replace(pos, from.length(), to);
            pos += to.length();
        }
    };

    // Lock một lần để đọc tất cả config values
    std::lock_guard<std::mutex> lock(mutex_);

    replaceAll(result, "%XENO_VERSION%",        version_);
    replaceAll(result, "%XENO_SERVER_PORT%",     std::to_string(serverPort_));
    replaceAll(result, "%XENO_CLIENT_PID%",      std::to_string(clientPid));
    replaceAll(result, "%ROBLOX_CLIENT_VERSION%", robloxVersion);

    return result;
}

} // namespace xeno
