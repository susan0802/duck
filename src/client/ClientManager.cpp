// ============================================================================
// ClientManager.cpp — Quản lý collection RBXClients
//
// Background scanner chạy liên tục để:
//   1. Phát hiện Roblox processes mới → tạo RBXClient
//   2. Phát hiện processes đã chết → cleanup RBXClient
//   3. Cache management
// ============================================================================

#include "client/ClientManager.h"
#include "process/ProcessManager.h"
#include "core/XenoConfig.h"

#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <fstream>

using json = nlohmann::json;

namespace xeno {

ClientManager::ClientManager() = default;

ClientManager::~ClientManager() {
    stopScanning();
    removeAllClients();
}

// ============================================================================
// startScanning / stopScanning
// ============================================================================

void ClientManager::startScanning() {
    if (scanning_) return;

    scanning_ = true;
    scannerThread_ = std::thread(&ClientManager::scannerThread, this);
    spdlog::info("Client scanner started");
}

void ClientManager::stopScanning() {
    scanning_ = false;
    if (scannerThread_.joinable()) {
        scannerThread_.join();
    }
}

// ============================================================================
// Client Access
// ============================================================================

std::shared_ptr<RBXClient> ClientManager::getClient(uint32_t pid) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = clients_.find(pid);
    return (it != clients_.end()) ? it->second : nullptr;
}

std::vector<std::shared_ptr<RBXClient>> ClientManager::getAllClients() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::shared_ptr<RBXClient>> result;
    result.reserve(clients_.size());
    for (const auto& [pid, client] : clients_) {
        result.push_back(client);
    }
    return result;
}

std::vector<uint32_t> ClientManager::getClientPids() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<uint32_t> pids;
    pids.reserve(clients_.size());
    for (const auto& [pid, _] : clients_) {
        pids.push_back(pid);
    }
    return pids;
}

size_t ClientManager::clientCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return clients_.size();
}

// ============================================================================
// attachClient — Attach vào Roblox client
// ============================================================================

bool ClientManager::attachClient(uint32_t pid) {
    auto client = getClient(pid);
    if (!client) {
        spdlog::error("Cannot attach: client PID {} not found", pid);
        return false;
    }

    if (client->isAttached()) {
        spdlog::info("Client was already attached, handling important core functions "
                     "for the client :D");
        return true;
    }

    // Tìm offsets cho version này
    std::string version = client->version();
    RBXOffsets offsets;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = offsetsMap_.find(version);
        if (it != offsetsMap_.end()) {
            offsets = it->second;
        } else {
            // Tìm version mặc định
            spdlog::warn("Offsets for version \"{}\" not found. Using offsets from "
                         "default version: {}. Please report this to me (Rizve)",
                         version, "default");
            // Dùng offsets mặc định nếu có
        }
    }

    client->applyOffsets(offsets);
    return client->attach();
}

// ============================================================================
// removeClient / removeAllClients
// ============================================================================

void ClientManager::removeClient(uint32_t pid) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = clients_.find(pid);
    if (it != clients_.end()) {
        clients_.erase(it);
        if (onRemoved_) onRemoved_(pid);
        spdlog::info("Removed client PID: {}", pid);
    }
}

void ClientManager::removeAllClients() {
    std::lock_guard<std::mutex> lock(mutex_);
    clients_.clear();
}

// ============================================================================
// loadOffsets — Parse offsets JSON
//
// Format expected:
// {
//   "versions": {
//     "0.620.0.6200589": {
//       "dataModel": 1234,
//       "scriptContext": 5678,
//       ...
//     },
//     "default": "0.620.0.6200589"   // alias
//   }
// }
// ============================================================================

void ClientManager::loadOffsets(const std::string& offsetsJson) {
    try {
        auto j = json::parse(offsetsJson);

        std::lock_guard<std::mutex> lock(mutex_);
        offsetsMap_.clear();

        if (j.contains("versions") && j["versions"].is_object()) {
            for (auto& [version, data] : j["versions"].items()) {
                if (data.is_object()) {
                    RBXOffsets offsets;
                    if (data.contains("dataModel"))
                        offsets.dataModel = data["dataModel"].get<uintptr_t>();
                    if (data.contains("scriptContext"))
                        offsets.scriptContext = data["scriptContext"].get<uintptr_t>();
                    if (data.contains("workspace"))
                        offsets.workspace = data["workspace"].get<uintptr_t>();
                    if (data.contains("players"))
                        offsets.players = data["players"].get<uintptr_t>();

                    offsetsMap_[version] = offsets;
                }
                else if (data.is_string()) {
                    // Alias: version "X" points to version "Y"
                    std::string target = data.get<std::string>();
                    spdlog::info("Offsets for version \"{}\" points to version \"{}\" "
                                 "with same offsets", version, target);
                }
            }
        }

        spdlog::info("Loaded {} offset versions", offsetsMap_.size());

    } catch (const json::exception& e) {
        spdlog::error("Failed to parse RBXClient data: \"{}\". "
                      "Endpoint may be down or data is corrupted.", e.what());
    }
}

// ============================================================================
// Callbacks
// ============================================================================

void ClientManager::onClientAdded(ClientAddedCallback callback) {
    onAdded_ = std::move(callback);
}

void ClientManager::onClientRemoved(ClientRemovedCallback callback) {
    onRemoved_ = std::move(callback);
}

// ============================================================================
// toJson — Serialize tất cả clients
// ============================================================================

std::string ClientManager::toJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    json arr = json::array();
    for (const auto& [pid, client] : clients_) {
        arr.push_back(json::parse(client->toJson()));
    }
    return arr.dump();
}

// ============================================================================
// Cache — Lưu/đọc state
// ============================================================================

void ClientManager::loadCache() {
    // Trong thực tế, đọc cache file từ temp directory
    // Nếu có cache hợp lệ → khôi phục RBXClient states
    //
    // auto cachePath = std::filesystem::temp_directory_path() / "Xeno" / "cache.json";
    // if (std::filesystem::exists(cachePath)) {
    //     std::ifstream file(cachePath);
    //     auto j = json::parse(file);
    //     // Restore clients, bytecode, mflags, clientinfos
    //     spdlog::info("Loaded existing cache: {} RBXClients, {} RBXBytecode, "
    //                  "{} RBXMFlags, {} RBXClientInfos",
    //                  clients, bytecodes, mflags, infos);
    // }
}

void ClientManager::saveCache() const {
    // Serialize current state → cache file
}

// ============================================================================
// scanForClients — Quét Roblox processes
//
// Sử dụng ProcessManager::enumerateProcesses() (Toolhelp32 API)
// để tìm tất cả instances của RobloxPlayerBeta.exe
// ============================================================================

void ClientManager::scanForClients() {
    // Lấy danh sách Roblox processes hiện tại
    auto robloxProcesses = ProcessManager::findProcessesByName(L"RobloxPlayerBeta.exe");

    std::lock_guard<std::mutex> lock(mutex_);

    // Thêm processes mới
    for (const auto& proc : robloxProcesses) {
        if (clients_.count(proc.pid) == 0) {
            // Process mới phát hiện
            auto client = std::make_shared<RBXClient>(proc.pid, proc.exeName);
            clients_[proc.pid] = client;

            if (onAdded_) {
                // Gọi callback ngoài lock (để tránh deadlock)
                // Trong thực tế, nên dùng deferred callback pattern
                onAdded_(proc.pid);
            }
        }
    }

    // Xóa processes đã chết
    std::vector<uint32_t> deadPids;
    for (const auto& [pid, client] : clients_) {
        if (!client->isProcessAlive()) {
            deadPids.push_back(pid);
        }
    }

    for (uint32_t pid : deadPids) {
        spdlog::info("Client was closed while attaching");
        clients_.erase(pid);
        if (onRemoved_) onRemoved_(pid);
    }
}

// ============================================================================
// scannerThread — Background scanning loop
//
// Chạy mỗi 2 giây, gọi scanForClients()
// ============================================================================

void ClientManager::scannerThread() {
    spdlog::info("Client Checker thread: {:#x}",
                 std::hash<std::thread::id>{}(std::this_thread::get_id()));

    while (scanning_) {
        scanForClients();

        // Ngủ 2 giây (chia nhỏ để có thể stop nhanh)
        for (int i = 0; i < 20 && scanning_; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

} // namespace xeno
