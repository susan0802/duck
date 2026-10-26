#pragma once

// ============================================================================
// XenoConfig.h — Cấu hình chung cho toàn bộ Xeno
//
// Class này giữ tất cả settings có thể thay đổi: port server,
// đường dẫn auto-execute, version string, và các URL endpoint.
// Được thiết kế theo Singleton pattern — chỉ có 1 instance duy nhất.
// ============================================================================

#include <string>
#include <mutex>
#include <filesystem>

namespace xeno {

/// Cấu hình toàn cục của Xeno — Singleton
///
/// Ví dụ sử dụng:
///   auto& config = XenoConfig::instance();
///   int port = config.serverPort();
///   config.setServerPort(28015);
///
class XenoConfig {
public:
    // ---- Singleton access ----
    /// Lấy instance duy nhất (thread-safe nhờ C++11 magic statics)
    static XenoConfig& instance();

    // Không cho phép copy hoặc move
    XenoConfig(const XenoConfig&) = delete;
    XenoConfig& operator=(const XenoConfig&) = delete;

    // ---- Getters ----

    /// Port mà HTTP server lắng nghe (mặc định: 28015)
    int serverPort() const;

    /// Phiên bản Xeno (VD: "1.0.0")
    std::string version() const;

    /// Thư mục chứa scripts tự động chạy khi attach
    std::filesystem::path autoExecDirectory() const;

    /// URL chính để tải offsets data
    std::string primaryEndpoint() const;

    /// URL phụ (fallback) khi endpoint chính lỗi
    std::string secondaryEndpoint() const;

    /// Số lượng WebSocket connections tối đa
    int maxWebSocketConnections() const;

    // ---- Setters ----

    void setServerPort(int port);
    void setAutoExecDirectory(const std::filesystem::path& dir);
    void setMaxWebSocketConnections(int max);

    // ---- Utility ----

    /// Thay thế environment variables trong string
    /// VD: "%XENO_VERSION%" → "1.0.0"
    ///     "%XENO_SERVER_PORT%" → "28015"
    std::string expandEnvironmentVars(const std::string& input, uint32_t clientPid = 0,
                                      const std::string& robloxVersion = "") const;

private:
    XenoConfig();  // Constructor private — chỉ gọi qua instance()

    mutable std::mutex mutex_;

    int         serverPort_{28015};
    std::string version_{"1.0.0"};
    std::filesystem::path autoExecDir_;
    std::string primaryEndpoint_{"https://x3no.pages.dev/ep.zst.enc"};
    std::string secondaryEndpoint_;
    int         maxWsConnections_{50};
};

} // namespace xeno
