#pragma once

// ============================================================================
// HttpClient.h — HTTP client wrapper dùng libcurl
//
// libcurl (https://curl.se/libcurl/) là thư viện HTTP client
// phổ biến nhất cho C/C++. Xeno dùng nó để:
//   1. Tải offsets data từ remote server
//   2. Kiểm tra version updates
//   3. Gửi telemetry (nếu có)
//
// libcurl API flow:
//   1. curl_global_init(CURL_GLOBAL_ALL)    — init 1 lần
//   2. curl_easy_init()                      — tạo session
//   3. curl_easy_setopt(handle, OPT, value)  — cấu hình
//   4. curl_easy_perform(handle)             — thực hiện request
//   5. curl_easy_getinfo(handle, INFO, &val) — lấy thông tin response
//   6. curl_easy_cleanup(handle)             — giải phóng session
//   7. curl_global_cleanup()                 — cleanup 1 lần
//
// curl_share_* được dùng để share connection cache giữa các requests
// (tái sử dụng TCP connections → nhanh hơn)
// ============================================================================

#include <string>
#include <vector>
#include <cstdint>
#include <optional>
#include <unordered_map>

namespace xeno {

/// HTTP response từ server
struct HttpResponse {
    int         statusCode{0};    ///< HTTP status code (200, 404, 500, ...)
    bool        success{false};   ///< True nếu status 2xx
    std::string body;             ///< Response body
    std::unordered_map<std::string, std::string> headers; ///< Response headers
    std::string error;            ///< Error message (nếu request thất bại)
};

/// HTTP client dựa trên libcurl
///
/// Lifecycle:
///   1. Tạo HttpClient → gọi curl_global_init
///   2. Dùng get()/post() để gửi requests
///   3. Destroy HttpClient → gọi curl_global_cleanup
///
/// curl_share_* được dùng để tái sử dụng connections:
///   Khi gửi nhiều requests đến cùng server, TCP connection
///   được giữ lại thay vì tạo mới mỗi lần (HTTP keep-alive).
///
/// Ví dụ:
///   HttpClient client;
///   auto response = client.get("https://example.com/data.json");
///   if (response.statusCode == 200) {
///       auto data = nlohmann::json::parse(response.body);
///   }
///
class HttpClient {
public:
    HttpClient();
    ~HttpClient();

    // Không cho copy (curl handle không nên copy)
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    /// HTTP GET request
    ///
    /// @param url      URL đầy đủ
    /// @param headers  Optional headers (VD: {"Authorization": "Bearer ..."})
    /// @return Response với status code, body, headers
    HttpResponse get(const std::string& url,
                     const std::unordered_map<std::string, std::string>& headers = {});

    /// HTTP POST request
    ///
    /// @param url      URL đầy đủ
    /// @param body     Request body
    /// @param headers  Optional headers
    /// @return Response
    HttpResponse post(const std::string& url,
                      const std::string& body,
                      const std::unordered_map<std::string, std::string>& headers = {});

    /// Tải file về dạng bytes
    ///
    /// Dùng cho: tải offsets data, resources
    /// Tự động handle redirects, SSL verification.
    ///
    /// @param url  URL của file
    /// @return Binary content, nullopt nếu lỗi
    std::optional<std::vector<uint8_t>> downloadBinary(const std::string& url);

    /// Lấy version string của libcurl
    static std::string curlVersion();

private:
    /// Callback cho curl — nhận data từ server
    ///
    /// libcurl gọi callback này mỗi khi nhận được một chunk data.
    /// Function pointer format theo yêu cầu của CURLOPT_WRITEFUNCTION.
    ///
    /// @param ptr      Pointer đến data chunk
    /// @param size     Luôn = 1
    /// @param nmemb    Số bytes trong chunk
    /// @param userdata Pointer đến string buffer (truyền qua CURLOPT_WRITEDATA)
    /// @return Số bytes đã xử lý (phải = size * nmemb, nếu không curl báo lỗi)
    static size_t writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata);

    void* shareHandle_{nullptr};  ///< curl_share handle (connection cache)
    bool  initialized_{false};
};

} // namespace xeno
