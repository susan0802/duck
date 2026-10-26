// ============================================================================
// HttpClient.cpp — libcurl HTTP client wrapper
//
// libcurl là thư viện HTTP client phổ biến nhất trên C/C++.
// Xeno dùng để download offsets data từ endpoint.
//
// curl_easy API flow:
//   1. curl_easy_init()           — tạo handle
//   2. curl_easy_setopt(...)      — cấu hình (URL, callbacks, headers)
//   3. curl_easy_perform()        — thực hiện request (blocking)
//   4. curl_easy_getinfo(...)     — lấy response info (status code, etc)
//   5. curl_easy_cleanup()        — free handle
//
// Write callback:
//   curl gọi callback mỗi khi nhận được data chunk.
//   size * nmemb = số bytes data nhận được.
//   Return: phải trả lại size * nmemb, nếu không curl sẽ abort.
// ============================================================================

#include "utils/HttpClient.h"
#include <spdlog/spdlog.h>

#include <curl/curl.h>

namespace xeno {

// ============================================================================
// Constructor / Destructor — quản lý curl global state
// ============================================================================

HttpClient::HttpClient() {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // Tạo share handle để tái sử dụng connections
    shareHandle_ = curl_share_init();
    if (shareHandle_) {
        curl_share_setopt(static_cast<CURLSH*>(shareHandle_),
                          CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
        curl_share_setopt(static_cast<CURLSH*>(shareHandle_),
                          CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
    }

    initialized_ = true;
}

HttpClient::~HttpClient() {
    if (shareHandle_) {
        curl_share_cleanup(static_cast<CURLSH*>(shareHandle_));
        shareHandle_ = nullptr;
    }
    curl_global_cleanup();
    initialized_ = false;
}

// ============================================================================
// Write callbacks — nhận data từ server
// ============================================================================

size_t HttpClient::writeCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buffer = static_cast<std::string*>(userdata);
    size_t totalBytes = size * nmemb;
    buffer->append(ptr, totalBytes);
    return totalBytes;
}

static size_t writeBinaryCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buffer = static_cast<std::vector<uint8_t>*>(userdata);
    size_t totalBytes = size * nmemb;
    auto* data = reinterpret_cast<uint8_t*>(ptr);
    buffer->insert(buffer->end(), data, data + totalBytes);
    return totalBytes;
}

// ============================================================================
// Helper: Cấu hình chung cho curl handle
// ============================================================================

static void setupCommonOptions(CURL* curl, CURLSH* share, long timeoutSeconds = 30) {
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Xeno/1.0");
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

    if (share) {
        curl_easy_setopt(curl, CURLOPT_SHARE, share);
    }
}

// ============================================================================
// Helper: Set custom headers
// ============================================================================

static struct curl_slist* setHeaders(
    CURL* curl,
    const std::unordered_map<std::string, std::string>& headers
) {
    struct curl_slist* list = nullptr;
    for (const auto& [key, value] : headers) {
        std::string header = key + ": " + value;
        list = curl_slist_append(list, header.c_str());
    }
    if (list) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
    }
    return list;
}

// ============================================================================
// get — HTTP GET request
// ============================================================================

HttpResponse HttpClient::get(
    const std::string& url,
    const std::unordered_map<std::string, std::string>& headers
) {
    HttpResponse response;

    CURL* curl = curl_easy_init();
    if (!curl) {
        response.error = "Failed to init curl";
        spdlog::error("curl_easy_init failed");
        return response;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    setupCommonOptions(curl, static_cast<CURLSH*>(shareHandle_));

    auto* headerList = setHeaders(curl, headers);

    CURLcode res = curl_easy_perform(curl);

    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.statusCode);
    } else {
        response.error = curl_easy_strerror(res);
        spdlog::error("HTTP GET {} failed: {}", url, response.error);
    }

    if (headerList) curl_slist_free_all(headerList);
    curl_easy_cleanup(curl);

    return response;
}

// ============================================================================
// post — HTTP POST request
// ============================================================================

HttpResponse HttpClient::post(
    const std::string& url,
    const std::string& body,
    const std::unordered_map<std::string, std::string>& headers
) {
    HttpResponse response;

    CURL* curl = curl_easy_init();
    if (!curl) {
        response.error = "Failed to init curl";
        return response;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));

    setupCommonOptions(curl, static_cast<CURLSH*>(shareHandle_));
    auto* headerList = setHeaders(curl, headers);

    CURLcode res = curl_easy_perform(curl);

    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.statusCode);
    } else {
        response.error = curl_easy_strerror(res);
        spdlog::error("HTTP POST {} failed: {}", url, response.error);
    }

    if (headerList) curl_slist_free_all(headerList);
    curl_easy_cleanup(curl);

    return response;
}

// ============================================================================
// downloadBinary — Download file as binary bytes
//
// Đây là hàm được dùng trong Initialize() để download offsets:
//   auto data = httpClient.downloadBinary("https://x3no.pages.dev/ep.zst.enc");
//   → decrypt → decompress → parse JSON
// ============================================================================

std::optional<std::vector<uint8_t>> HttpClient::downloadBinary(const std::string& url) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        spdlog::error("curl_easy_init failed");
        return std::nullopt;
    }

    std::vector<uint8_t> buffer;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBinaryCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    setupCommonOptions(curl, static_cast<CURLSH*>(shareHandle_), 60);

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        spdlog::error("Download {} failed: {}", url, curl_easy_strerror(res));
        curl_easy_cleanup(curl);
        return std::nullopt;
    }

    long statusCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &statusCode);
    curl_easy_cleanup(curl);

    if (statusCode < 200 || statusCode >= 300) {
        spdlog::error("Download {} returned status {}", url, statusCode);
        return std::nullopt;
    }

    return buffer;
}

// ============================================================================
// curlVersion — Version string
// ============================================================================

std::string HttpClient::curlVersion() {
    return curl_version();
}

} // namespace xeno
