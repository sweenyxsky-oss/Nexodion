#pragma once
#include <chrono>
#include <string>
#include <nlohmann/json.hpp>

namespace nexodion {
struct ApiResponse {
    bool ok{false};
    int http_status{0};
    int daemon_status{-1};
    nlohmann::json data;
    std::string error;
};
// Documented ShadowMountPlus API v1 JSON POST protocol; never shell commands.
class ShadowMountApi {
public:
    ShadowMountApi(std::string host="127.0.0.1", uint16_t port=10101)
        : host_(std::move(host)),port_(port) {}
    ApiResponse request(const std::string& route, const nlohmann::json& body=nlohmann::json::object(),
                        std::chrono::milliseconds timeout=std::chrono::milliseconds(5000)) const;
    ApiResponse games() const {return request("/api/v1/games",{{"include_size",true}},std::chrono::milliseconds(60000));}
    ApiResponse mount(const std::string& title, bool writable=false) const;
    ApiResponse unmount(const std::string& title) const;
    ApiResponse uninstall(const std::string& title) const;
private:
    std::string host_;
    uint16_t port_;
};
}