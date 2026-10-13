#include "nexodion/startup_diagnostics.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <netinet/in.h>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

namespace nexodion {

std::string DiagnosticItem::severity_string() const {
    switch (severity) {
        case DiagnosticSeverity::Ok: return "OK";
        case DiagnosticSeverity::Info: return "INFO";
        case DiagnosticSeverity::Warning: return "WARNING";
        case DiagnosticSeverity::Error: return "ERROR";
    }
    return "UNKNOWN";
}

std::string DiagnosticItem::subsystem_string() const {
    switch (subsystem) {
        case DiagnosticSubsystem::Mount: return "Mount";
        case DiagnosticSubsystem::Daemon: return "Daemon";
        case DiagnosticSubsystem::Network: return "Network";
        case DiagnosticSubsystem::Service: return "Service";
        case DiagnosticSubsystem::Helper: return "Helper";
        case DiagnosticSubsystem::Loader: return "ELF Loader";
        case DiagnosticSubsystem::Share: return "Share";
    }
    return "Unknown";
}

bool StartupDiagnosticsReport::all_ok() const {
    for (const auto& item : items) {
        if (item.is_failure() || item.is_warning()) {
            return false;
        }
    }
    return true;
}

bool StartupDiagnosticsReport::has_failures() const {
    for (const auto& item : items) {
        if (item.is_failure()) {
            return true;
        }
    }
    return false;
}

bool StartupDiagnosticsReport::has_warnings() const {
    for (const auto& item : items) {
        if (item.is_warning()) {
            return true;
        }
    }
    return false;
}

size_t StartupDiagnosticsReport::failure_count() const {
    size_t count = 0;
    for (const auto& item : items) {
        if (item.is_failure()) count++;
    }
    return count;
}

size_t StartupDiagnosticsReport::warning_count() const {
    size_t count = 0;
    for (const auto& item : items) {
        if (item.is_warning()) count++;
    }
    return count;
}

std::vector<DiagnosticItem> StartupDiagnosticsReport::failed_or_warning_items() const {
    std::vector<DiagnosticItem> result;
    for (const auto& item : items) {
        if (item.is_failure() || item.is_warning()) {
            result.push_back(item);
        }
    }
    return result;
}

std::string StartupDiagnosticsReport::format_summary() const {
    std::ostringstream ss;
    ss << "Diagnostics Summary: " << (has_failures() ? "FAILURES DETECTED" : (has_warnings() ? "WARNINGS DETECTED" : "ALL SYSTEMS OK"))
       << " (" << failure_count() << " errors, " << warning_count() << " warnings, " << items.size() << " checks)";
    return ss.str();
}

std::string StartupDiagnosticsReport::format_detailed_report() const {
    std::ostringstream ss;
    ss << "=== Startup Diagnostics Report ===\n";
    for (const auto& item : items) {
        ss << "[" << item.subsystem_string() << "] " << item.name << " : " << item.severity_string() << "\n";
        ss << "  Summary : " << item.summary << "\n";
        if (!item.details.empty()) {
            ss << "  Details : " << item.details << "\n";
        }
        if (!item.recovery_guidance.empty() && (item.is_failure() || item.is_warning())) {
            ss << "  Recovery:\n";
            std::istringstream lines(item.recovery_guidance);
            std::string line;
            while (std::getline(lines, line)) {
                ss << "    " << line << "\n";
            }
        }
    }
    ss << "==================================";
    return ss.str();
}

std::string StartupDiagnosticsReport::format_on_screen_guidance() const {
    std::ostringstream ss;
    auto issues = failed_or_warning_items();
    if (issues.empty()) {
        ss << "All core subsystems initialized successfully.";
        return ss.str();
    }

    for (size_t i = 0; i < issues.size(); ++i) {
        const auto& item = issues[i];
        ss << (i + 1) << ". [" << item.subsystem_string() << "] " << item.summary << "\n";
        if (!item.recovery_guidance.empty()) {
            std::istringstream lines(item.recovery_guidance);
            std::string line;
            while (std::getline(lines, line)) {
                ss << "   • " << line << "\n";
            }
        }
        if (i + 1 < issues.size()) {
            ss << "\n";
        }
    }
    return ss.str();
}

StartupDiagnosticsEngine::StartupDiagnosticsEngine(StartupDiagnosticsConfig config)
    : config_(std::move(config)) {}

StartupDiagnosticsReport StartupDiagnosticsEngine::run_all_checks() {
    StartupDiagnosticsReport report;
    report.items.push_back(check_mount());
    report.items.push_back(check_daemon());
    if (config_.check_network) {
        report.items.push_back(check_network());
    }
    report.items.push_back(check_services());
    report.items.push_back(check_helpers());
    if (config_.require_elf_loader) report.items.push_back(check_elf_loader());
    for (auto& share : check_shares()) report.items.push_back(std::move(share));
    report.items[0].check_key = "mount";
    report.items[1].check_key = "daemon";
    for (auto& it : report.items) if (it.check_key.empty()) {
        if (it.subsystem == DiagnosticSubsystem::Network) it.check_key = "network";
        else if (it.subsystem == DiagnosticSubsystem::Service) it.check_key = "services";
    }
    return report;
}

bool StartupDiagnosticsReport::replace_item(const DiagnosticItem& item) {
    for (auto& it : items) if (!item.check_key.empty() && it.check_key == item.check_key) { it = item; return true; }
    return false;
}

namespace {
bool probe_tcp(const std::string& host, uint16_t port, int timeout_ms, std::string& why) {
    const int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { why = std::strerror(errno); return false; }
    const int flags = fcntl(sock, F_GETFL, 0);
    if (flags >= 0) fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) { close(sock); why = "invalid address"; return false; }
    bool ok = false;
    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) ok = true;
    else if (errno == EINPROGRESS) {
        pollfd p{sock, POLLOUT, 0};
        int code = 0; socklen_t len = sizeof(code);
        if (poll(&p, 1, timeout_ms) > 0 && getsockopt(sock, SOL_SOCKET, SO_ERROR, &code, &len) == 0 && code == 0) ok = true;
        else why = code ? std::strerror(code) : "timed out";
    } else why = std::strerror(errno);
    close(sock);
    return ok;
}
}

DiagnosticItem StartupDiagnosticsEngine::check_helpers() {
    DiagnosticItem item;
    item.subsystem = DiagnosticSubsystem::Helper;
    item.name = "Helper Programs";
    item.check_key = "helpers";
    std::vector<std::string> missing;
    std::error_code ec;
    for (const auto& name : config_.helper_names) {
        const auto path = std::filesystem::path(config_.helper_dir) / name;
        if (!std::filesystem::is_regular_file(path, ec) || std::filesystem::file_size(path, ec) == 0 || ec) missing.push_back(name);
    }
    if (missing.empty()) {
        item.summary = "Install and launch helpers are present";
        item.details = "Found in " + config_.helper_dir;
        return item;
    }
    std::string list;
    for (const auto& m : missing) list += (list.empty() ? "" : ", ") + m;
    item.severity = DiagnosticSeverity::Error;
    item.summary = "Missing helper program: " + list;
    item.details = "Expected in " + config_.helper_dir + ". Installing and launching games will not work.";
    item.recovery_guidance = "Reinstall the full Nexodion package so both helper files are in the app folder, then choose Retry.";
    item.error_code = ENOENT;
    return item;
}

DiagnosticItem StartupDiagnosticsEngine::check_elf_loader() {
    DiagnosticItem item;
    item.subsystem = DiagnosticSubsystem::Loader;
    item.name = "ELF Loader";
    item.check_key = "elf_loader";
    std::string why;
    if (probe_tcp(config_.elf_loader_host, config_.elf_loader_port, config_.probe_timeout_ms, why)) {
        item.summary = "ELF loader is answering on port " + std::to_string(config_.elf_loader_port);
        return item;
    }
    item.severity = DiagnosticSeverity::Error;
    item.summary = "ELF loader is not running";
    item.details = config_.elf_loader_host + ":" + std::to_string(config_.elf_loader_port) + " (" + why + ")";
    item.recovery_guidance = "Start your ELF loader (it listens on port " + std::to_string(config_.elf_loader_port) +
                             "), then choose Retry. Installs and launches need it.";
    item.error_code = ECONNREFUSED;
    return item;
}

std::vector<DiagnosticItem> StartupDiagnosticsEngine::check_shares() {
    std::vector<DiagnosticItem> out;
    for (const auto& share : config_.shares) {
        DiagnosticItem item;
        item.subsystem = DiagnosticSubsystem::Share;
        item.name = "Share: " + share.name;
        item.check_key = "share:" + share.id;
        std::string detail;
        const bool ok = share.probe && share.probe(detail);
        item.details = detail;
        if (ok) item.summary = share.name + " is reachable";
        else {
            item.severity = DiagnosticSeverity::Warning;
            item.summary = share.name + " cannot be reached";
            item.recovery_guidance = "Check that the NAS or drive is on and connected, and that the address and password in "
                                     "Settings > Mounting are right, then choose Retry. Games from this share stay hidden until then.";
            item.error_code = EHOSTUNREACH;
        }
        out.push_back(std::move(item));
    }
    return out;
}

DiagnosticItem StartupDiagnosticsEngine::retry(const DiagnosticItem& prior) {
    DiagnosticItem item;
    const auto& k = prior.check_key;
    if (k == "mount") item = check_mount();
    else if (k == "daemon") item = check_daemon();
    else if (k == "network") item = check_network();
    else if (k == "services") item = check_services();
    else if (k == "helpers") item = check_helpers();
    else if (k == "elf_loader") item = check_elf_loader();
    else if (k.starts_with("share:")) {
        const auto saved = config_.shares;
        auto keep = config_.shares;
        keep.erase(std::remove_if(keep.begin(), keep.end(), [&](const auto& s){ return "share:" + s.id != k; }), keep.end());
        config_.shares = keep;
        auto all = check_shares();
        config_.shares = saved;
        if (!all.empty()) item = all.front(); else return prior;
    } else return prior;
    item.check_key = k;
    return item;
}

DiagnosticItem StartupDiagnosticsEngine::check_mount() {
    DiagnosticItem item;
    item.subsystem = DiagnosticSubsystem::Mount;
    item.name = "Root Scan Mount Probing";

    std::error_code ec;
    if (!std::filesystem::exists(config_.mount_path, ec)) {
        item.severity = DiagnosticSeverity::Error;
        item.summary = "Scan root path does not exist";
        item.details = "Path '" + config_.mount_path + "' was not found on the filesystem.";
        item.recovery_guidance =
            "1. Connect your external USB drive or verify host filesystem mount.\n"
            "2. In your PS5 jailbreak environment, verify that /hostapp payload is loaded.\n"
            "3. Launch with --scan-dir <directory> to select a different accessible path.";
        item.error_code = ENOENT;
        return item;
    }

    if (!std::filesystem::is_directory(config_.mount_path, ec)) {
        item.severity = DiagnosticSeverity::Error;
        item.summary = "Scan root is not a directory";
        item.details = "Path '" + config_.mount_path + "' exists but is not a directory.";
        item.recovery_guidance = "Select a valid folder containing game packages or homebrew applications.";
        item.error_code = ENOTDIR;
        return item;
    }

    // Try iterating
    bool is_empty = true;
    for (auto it = std::filesystem::directory_iterator(config_.mount_path, ec);
         !ec && it != std::filesystem::directory_iterator(); ++it) {
        is_empty = false;
        break;
    }

    if (ec) {
        item.severity = DiagnosticSeverity::Error;
        item.summary = "Scan root cannot be read (Permission Denied)";
        item.details = "Filesystem error while reading directory: " + ec.message();
        item.recovery_guidance = "Check permissions for " + config_.mount_path + " or run with root payload permissions.";
        item.error_code = ec.value();
        return item;
    }

    if (is_empty) {
        item.severity = DiagnosticSeverity::Warning;
        item.summary = "Scan root directory is empty";
        item.details = "Directory '" + config_.mount_path + "' contains no files or subdirectories.";
        item.recovery_guidance =
            "1. Copy PS5/PS4 packages (.pkg, .exfat, .ffpkg) or folders with sce_sys/param.json.\n"
            "2. Mount an active NFS share or external storage drive to this location.";
        return item;
    }

    item.severity = DiagnosticSeverity::Ok;
    item.summary = "Scan root is mounted and readable";
    item.details = "Directory '" + config_.mount_path + "' verified.";
    return item;
}

DiagnosticItem StartupDiagnosticsEngine::check_daemon() {
    DiagnosticItem item;
    item.subsystem = DiagnosticSubsystem::Daemon;
    item.name = "ShadowMountPlus Daemon Probe";

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        item.severity = DiagnosticSeverity::Error;
        item.summary = "Socket creation failed for daemon probe";
        item.details = std::string("Error creating socket: ") + std::strerror(errno);
        item.recovery_guidance = "Check network stack initialization and system socket privileges.";
        item.error_code = errno;
        return item;
    }

    // Set non-blocking
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.daemon_port);
    if (inet_pton(AF_INET, config_.daemon_host.c_str(), &addr.sin_addr) <= 0) {
        close(sock);
        item.severity = DiagnosticSeverity::Error;
        item.summary = "Invalid daemon IP address";
        item.details = "Failed to parse daemon IP address: " + config_.daemon_host;
        item.recovery_guidance = "Specify a valid IPv4 address (e.g. 127.0.0.1) for the daemon host.";
        item.error_code = EINVAL;
        return item;
    }

    int res = connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (res < 0 && errno == EINPROGRESS) {
        struct pollfd pfd{};
        pfd.fd = sock;
        pfd.events = POLLOUT;
        int poll_res = poll(&pfd, 1, config_.probe_timeout_ms);
        if (poll_res > 0) {
            int so_error = 0;
            socklen_t len = sizeof(so_error);
            getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len);
            if (so_error == 0) {
                res = 0;
            } else {
                errno = so_error;
                res = -1;
            }
        } else {
            res = -1;
            errno = ETIMEDOUT;
        }
    }

    close(sock);

    if (res == 0) {
        item.severity = DiagnosticSeverity::Ok;
        item.summary = "ShadowMountPlus daemon online";
        item.details = "Daemon reachable at " + config_.daemon_host + ":" + std::to_string(config_.daemon_port);
    } else {
        item.severity = DiagnosticSeverity::Warning;
        item.summary = "ShadowMountPlus daemon offline or unreachable";
        item.details = "Failed to connect to " + config_.daemon_host + ":" + std::to_string(config_.daemon_port) + " (" + std::strerror(errno) + ")";
        item.recovery_guidance =
            "1. Ensure the ShadowMountPlus homebrew application is installed on your PS5.\n"
            "2. Launch ShadowMountPlus in the background before starting Nexodion.\n"
            "3. Remote NFS browsing and virtual mounting will remain locked until the daemon is active.";
        item.error_code = errno;
    }

    return item;
}

DiagnosticItem StartupDiagnosticsEngine::check_network() {
    DiagnosticItem item;
    item.subsystem = DiagnosticSubsystem::Network;
    item.name = "Network Subsystem Initializer";

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        item.severity = DiagnosticSeverity::Error;
        item.summary = "Network socket initialization failure";
        item.details = "Failed to allocate UDP socket (" + std::string(std::strerror(errno)) + ")";
        item.recovery_guidance =
            "1. Verify PS5 Network Settings and ensure LAN cable or Wi-Fi is connected.\n"
            "2. Check that jailbreak payload did not disable socket networking APIs.";
        item.error_code = errno;
        return item;
    }
    close(sock);

    item.severity = DiagnosticSeverity::Ok;
    item.summary = "Network stack initialized and ready";
    item.details = "Socket interface operational.";
    return item;
}

DiagnosticItem StartupDiagnosticsEngine::check_services() {
    DiagnosticItem item;
    item.subsystem = DiagnosticSubsystem::Service;
    item.name = "Runtime Storage & Persistence Services";

    auto check_writable = [](const std::string& path) -> bool {
        std::error_code ec;
        std::filesystem::path p(path);
        auto parent = p.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, ec);
            if (ec) return false;
        }
        // Try creating or opening file for append
        std::ofstream out(path, std::ios::app);
        if (!out.is_open()) {
            return false;
        }
        out.close();
        return true;
    };

    bool cache_ok = check_writable(config_.cache_path);
    bool jobs_ok = check_writable(config_.jobs_path);

    if (!cache_ok || !jobs_ok) {
        item.severity = DiagnosticSeverity::Error;
        item.summary = "Service persistence paths not writable";
        std::ostringstream ss;
        if (!cache_ok) ss << "Cache path [" << config_.cache_path << "] write failed. ";
        if (!jobs_ok) ss << "Job state path [" << config_.jobs_path << "] write failed.";
        item.details = ss.str();
        item.recovery_guidance =
            "1. Ensure the /data homebrew directory is mounted with read-write permissions.\n"
            "2. Check free storage space on internal SSD /data partition.\n"
            "3. If simulating on host, pass writable paths via --cache <path> and --jobs <path>.";
        item.error_code = EACCES;
        return item;
    }

    item.severity = DiagnosticSeverity::Ok;
    item.summary = "Cache and job persistence services operational";
    item.details = "Verified write access to cache and job state files.";
    return item;
}

} // namespace nexodion
