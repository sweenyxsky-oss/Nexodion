#include "nexodion/daemon_client.hpp"
#include "nexodion/shadowmount_api.hpp"

#include <nlohmann/json.hpp>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cerrno>
#include <cstring>
#include <sstream>
#include <algorithm>
#include <condition_variable>

namespace nexodion {

const std::vector<std::string>& DaemonLock::protected_features() {
    static const std::vector<std::string> features = {
        "nfs_browser",
        "nfs_mount",
        "install_game",
        "install_dlc",
        "install_all_dlc",
        "install_update",
        "game_mount",
        "game_uninstall"
    };
    return features;
}

DaemonLock::DaemonLock(std::string host, uint16_t port)
    : host_(std::move(host)), port_(port) {
    daemon_info_.host = host_;
    daemon_info_.port = port_;
}

DaemonLock::~DaemonLock() {
    stop_background_monitor();
}

DaemonLock::DaemonLock(DaemonLock&& other) noexcept {
    other.stop_background_monitor();
    std::lock_guard<std::mutex> lock(other.mutex_);
    host_ = std::move(other.host_);
    port_ = other.port_;
    state_ = other.state_;
    daemon_info_ = std::move(other.daemon_info_);
    callback_ = std::move(other.callback_);
}

DaemonLock& DaemonLock::operator=(DaemonLock&& other) noexcept {
    if (this != &other) {
        stop_background_monitor();
        other.stop_background_monitor();

        std::scoped_lock lock(mutex_, other.mutex_);
        host_ = std::move(other.host_);
        port_ = other.port_;
        state_ = other.state_;
        daemon_info_ = std::move(other.daemon_info_);
        callback_ = std::move(other.callback_);
    }
    return *this;
}

bool DaemonLock::is_unlocked() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == LockState::Unlocked;
}

LockState DaemonLock::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

DaemonInfo DaemonLock::daemon_info() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return daemon_info_;
}

std::string DaemonLock::lock_message() const {
    return SHADOWMOUNT_REQUIRED_MESSAGE;
}

bool DaemonLock::is_feature_allowed(const std::string& feature_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == LockState::Unlocked) {
        return true;
    }
    const auto& protected_list = protected_features();
    return std::find(protected_list.begin(), protected_list.end(), feature_name) == protected_list.end();
}

void DaemonLock::set_target(std::string host, uint16_t port) {
    std::lock_guard<std::mutex> lock(mutex_);
    host_ = std::move(host);
    port_ = port;
    daemon_info_.host = host_;
    daemon_info_.port = port_;
}

std::pair<std::string, uint16_t> DaemonLock::target() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {host_, port_};
}

void DaemonLock::set_state_change_callback(StateChangeCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(callback);
}

void DaemonLock::update_state(LockState new_state, const DaemonInfo& info) {
    LockState old_state;
    StateChangeCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        old_state = state_;
        state_ = new_state;
        daemon_info_ = info;
        cb = callback_;
    }

    if (cb && old_state != new_state) {
        cb(old_state, new_state, info);
    }
}

ProbeResult DaemonLock::probe(std::chrono::milliseconds timeout) {
    std::string current_host;
    uint16_t current_port;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current_host = host_;
        current_port = port_;
    }

    DaemonInfo info;
    info.host = current_host;
    info.port = current_port;
    info.reachable = false;

    auto t0 = std::chrono::steady_clock::now();
    std::string body;
    int status_code = 0;

    auto response=ShadowMountApi(current_host,current_port).request("/api/v1/version",nlohmann::json::object(),timeout);
    bool http_ok=response.ok;
    body=response.data.is_object()?response.data.dump():"";
    status_code=response.http_status;
    auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
    info.latency = latency;

    ProbeResult result;
    if (!http_ok || status_code != 200) {
        info.reachable = false;
        info.error_message = http_ok ? ("HTTP status " + std::to_string(status_code)) : "Connection refused or timed out";
        result.success = false;
        result.new_state = LockState::Locked;
        result.info = info;
        update_state(LockState::Locked, info);
        return result;
    }

    try {
        auto parsed = nlohmann::json::parse(body);
        info.reachable = true;
        info.api_version = parsed.value("api_version", 0);
        if(info.api_version!=1) throw std::runtime_error("Unsupported ShadowMountPlus API version");
        info.shadowmount_version = parsed.value("shadowmount_version", "unknown");

        if (parsed.contains("capabilities") && parsed["capabilities"].is_array()) {
            for (const auto& cap : parsed["capabilities"]) {
                if (cap.is_string()) {
                    info.capabilities.push_back(cap.get<std::string>());
                }
            }
        }

        result.success = true;
        result.new_state = LockState::Unlocked;
        result.info = info;
        update_state(LockState::Unlocked, info);
        return result;

    } catch (const std::exception& ex) {
        info.reachable = false;
        info.error_message = std::string("Malformed JSON from daemon: ") + ex.what();
        result.success = false;
        result.new_state = LockState::Error;
        result.info = info;
        update_state(LockState::Error, info);
        return result;
    }
}

void DaemonLock::start_background_monitor(std::chrono::milliseconds interval) {
    if (monitor_running_.exchange(true)) {
        return; // Already running
    }

    monitor_thread_ = std::thread([this, interval]() {
        while (monitor_running_.load()) {
            probe(std::chrono::milliseconds(1000));

            // Sleep in 100ms increments to exit promptly on stop
            auto elapsed = std::chrono::milliseconds(0);
            while (monitor_running_.load() && elapsed < interval) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                elapsed += std::chrono::milliseconds(100);
            }
        }
    });
}

void DaemonLock::stop_background_monitor() {
    if (!monitor_running_.exchange(false)) {
        return;
    }
    if (monitor_thread_.joinable()) {
        monitor_thread_.join();
    }
}

} // namespace nexodion
