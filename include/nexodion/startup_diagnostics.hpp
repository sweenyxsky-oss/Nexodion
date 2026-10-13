#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <cstdint>
#include <memory>
#include <functional>

namespace nexodion {

enum class DiagnosticSeverity {
    Ok,
    Info,
    Warning,
    Error
};

enum class DiagnosticSubsystem {
    Mount,
    Daemon,
    Network,
    Service,
    Helper,
    Loader,
    Share
};

struct DiagnosticItem {
    DiagnosticSubsystem subsystem{DiagnosticSubsystem::Service};
    std::string name;
    DiagnosticSeverity severity{DiagnosticSeverity::Ok};
    std::string summary;
    std::string details;
    std::string recovery_guidance;
    int error_code{0};
    std::string check_key; // identifies the check so a single item can be retried

    bool is_ok() const { return severity == DiagnosticSeverity::Ok || severity == DiagnosticSeverity::Info; }
    bool is_failure() const { return severity == DiagnosticSeverity::Error; }
    bool is_warning() const { return severity == DiagnosticSeverity::Warning; }
    std::string severity_string() const;
    std::string subsystem_string() const;
};

struct StartupDiagnosticsConfig {
    std::string mount_path{"/hostapp"};
    std::string daemon_host{"127.0.0.1"};
    uint16_t daemon_port{10101};
    std::string cache_path{"/data/nexodion_cache.json"};
    std::string jobs_path{"/data/nexodion_install_jobs.json"};
    bool check_network{true};
    int probe_timeout_ms{500};
    // Folder holding the bundled helper programs (console: /app0).
    std::string helper_dir{"/app0"};
    std::vector<std::string> helper_names{"nexodion_launcher.elf", "nexodion_pkg_installer.elf"};
    // ELF loader used to start the helpers; only probed when require_elf_loader is set.
    std::string elf_loader_host{"127.0.0.1"};
    uint16_t elf_loader_port{9021};
    bool require_elf_loader{false};
    // One entry per configured share. probe returns true when reachable and fills detail.
    struct ShareProbe {
        std::string id;
        std::string name;
        std::function<bool(std::string& detail)> probe;
    };
    std::vector<ShareProbe> shares;
};

struct StartupDiagnosticsReport {
    std::vector<DiagnosticItem> items;
    std::chrono::system_clock::time_point timestamp{std::chrono::system_clock::now()};

    bool all_ok() const;
    bool has_failures() const;
    bool has_warnings() const;
    size_t failure_count() const;
    size_t warning_count() const;
    std::string format_summary() const;
    std::string format_detailed_report() const;
    std::string format_on_screen_guidance() const;
    std::vector<DiagnosticItem> failed_or_warning_items() const;
    // Replaces the item with the same check_key; returns false when not found.
    bool replace_item(const DiagnosticItem& item);
};

class StartupDiagnosticsEngine {
public:
    explicit StartupDiagnosticsEngine(StartupDiagnosticsConfig config = {});

    StartupDiagnosticsReport run_all_checks();
    DiagnosticItem check_mount();
    DiagnosticItem check_daemon();
    DiagnosticItem check_network();
    DiagnosticItem check_services();
    DiagnosticItem check_helpers();
    DiagnosticItem check_elf_loader();
    std::vector<DiagnosticItem> check_shares();
    // Re-runs just the check identified by item.check_key (e.g. after fixing one problem).
    DiagnosticItem retry(const DiagnosticItem& item);

    const StartupDiagnosticsConfig& config() const { return config_; }
    void set_config(const StartupDiagnosticsConfig& config) { config_ = config; }

private:
    StartupDiagnosticsConfig config_;
};

} // namespace nexodion
