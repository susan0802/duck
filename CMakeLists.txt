cmake_minimum_required(VERSION 3.20)
project(Xeno VERSION 1.0.0 LANGUAGES CXX)

# ============================================================================
# Yêu cầu C++20 — Xeno dùng nhiều feature hiện đại
# ============================================================================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Output là DLL (shared library)
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS OFF)

# ============================================================================
# Tìm thư viện bên ngoài (qua vcpkg hoặc system)
#
# Cài đặt vcpkg:
#   git clone https://github.com/microsoft/vcpkg.git
#   .\vcpkg\bootstrap-vcpkg.bat
#
# Cài dependencies:
#   vcpkg install openssl:x64-windows
#   vcpkg install curl:x64-windows
#   vcpkg install nlohmann-json:x64-windows
#   vcpkg install spdlog:x64-windows
#   vcpkg install fmt:x64-windows
#   vcpkg install lz4:x64-windows
#   vcpkg install zstd:x64-windows
#   vcpkg install cpp-httplib:x64-windows
#   vcpkg install ixwebsocket:x64-windows
#
# Build:
#   cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=[vcpkg root]/scripts/buildsystems/vcpkg.cmake
#   cmake --build build --config Release
# ============================================================================

find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(lz4 CONFIG REQUIRED)
find_package(zstd CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)
find_package(ixwebsocket CONFIG REQUIRED)

# ============================================================================
# Tập hợp source files
# ============================================================================
set(XENO_SOURCES
    # Core — DLL entry point và cấu hình
    src/core/DllExports.cpp
    src/core/XenoConfig.cpp

    # Server — HTTP server và WebSocket
    src/server/XenoServer.cpp
    src/server/WebSocketManager.cpp

    # Client — Quản lý Roblox processes
    src/client/RBXClient.cpp
    src/client/ClientManager.cpp

    # Crypto — Mã hóa / Hash
    src/crypto/CryptoEngine.cpp
    src/crypto/HashEngine.cpp

    # Process — Windows process operations
    src/process/ProcessManager.cpp

    # Compression — LZ4 / Zstd
    src/compression/CompressionEngine.cpp

    # Utils — Tiện ích
    src/utils/Base64.cpp
    src/utils/HttpClient.cpp
)

# ============================================================================
# Tạo DLL (SHARED library)
# ============================================================================
add_library(Xeno SHARED ${XENO_SOURCES})

# Include directories
target_include_directories(Xeno
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Link thư viện
target_link_libraries(Xeno
    PRIVATE
        OpenSSL::SSL
        OpenSSL::Crypto
        CURL::libcurl
        nlohmann_json::nlohmann_json
        spdlog::spdlog
        fmt::fmt
        lz4::lz4
        $<IF:$<TARGET_EXISTS:zstd::libzstd_shared>,zstd::libzstd_shared,zstd::libzstd_static>
        httplib::httplib
        ixwebsocket::ixwebsocket
)

# Windows system libraries (chỉ link trên Windows)
if(WIN32)
    target_link_libraries(Xeno
        PRIVATE
            Ws2_32       # Winsock (cho networking)
            Crypt32      # Windows crypto API
            Psapi        # Process status API
    )
endif()

# ============================================================================
# Định nghĩa preprocessor cho DLL export
# ============================================================================
target_compile_definitions(Xeno
    PRIVATE
        XENO_BUILD_DLL      # Đánh dấu đang build DLL (dùng trong DllExports.h)
        NOMINMAX             # Tránh xung đột min/max macro
        WIN32_LEAN_AND_MEAN  # Loại bỏ headers Windows không cần
)

if(WIN32)
    target_compile_definitions(Xeno PRIVATE _WIN32_WINNT=0x0A00)  # Target Windows 10+
endif()

# ============================================================================
# Compiler warnings
# ============================================================================
if(MSVC)
    target_compile_options(Xeno PRIVATE /W4 /utf-8)
else()
    target_compile_options(Xeno PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ============================================================================
# Output file settings
# ============================================================================
set_target_properties(Xeno PROPERTIES
    OUTPUT_NAME "Xeno"                    # Xeno.dll
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ============================================================================
# Thông tin build
# ============================================================================
message(STATUS "=== Xeno DLL Build Configuration ===")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Output: ${CMAKE_BINARY_DIR}/bin/Xeno.dll")
