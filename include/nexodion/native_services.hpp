#pragma once
#include "installer.hpp"
#include <string>
#include <map>
#include <vector>

namespace nexodion {
// Native PS5 React payload relay. Host builds refuse rather than simulate success.
class NativePackageDelegate final : public IInstallDelegate {
public:
    bool install_package(const InstallTask&, std::function<bool(uint64_t,uint64_t)>,
                         InstallErrorCode&, std::string&) override;
    bool is_platform_supported() const override;
    std::string platform_status_description() const override;
    // Root that must contain <TITLE_ID>/ after a confirmed install (console: /user/app).
    void set_verification_root(std::string root) { apps_root_ = std::move(root); }
    void set_verification_timeout_ms(int ms) { verify_timeout_ms_ = ms; }
private:
    std::string apps_root_{"/user/app"};
    int verify_timeout_ms_{15000};
};

// Checks run before an install is sent to the console: package readable,
// free space, existing installation and version.
struct ReadinessCheck { std::string name; bool passed{false}; bool blocking{true}; std::string detail; std::string fix; };
struct InstallReadiness {
    bool ok{true};                      // false when any blocking check failed
    std::vector<ReadinessCheck> checks;
    std::string title_id;
    std::string package_version;
    std::string installed_version;      // empty when not installed or unknown
    bool already_installed{false};
    bool same_or_older{false};          // installed version >= package version
    std::string summary() const;
};
// Compares dotted numeric versions ("01.020" vs "1.2"); returns <0, 0, >0. Non-numeric parts compare as text.
int compare_versions(const std::string& a, const std::string& b);
// Reads <apps_root>/<title_id>/sce_sys/param.json (contentVersion) or param.sfo (APP_VER).
std::string installed_title_version(const std::string& apps_root, const std::string& title_id);
InstallReadiness check_install_readiness(const std::string& package_path, const std::string& apps_root,
                                         const std::string& known_title_id = "",
                                         uint64_t safety_margin_bytes = 512ULL * 1024ULL * 1024ULL);

// Maps an installer helper "fail <hex code> <reason>" line to an error code and readable message.
struct InstallFailure { InstallErrorCode code{InstallErrorCode::Unknown}; std::string message; };
InstallFailure classify_installer_failure(const std::string& line);
// True when <apps_root>/<title_id> exists as a non-empty directory (installation landed).
bool verify_installed_title(const std::string& apps_root, const std::string& title_id, std::string& error);

// Launch support per console format, decided before anything is sent to the console.
struct LaunchCheck {
    bool can_launch{false};
    std::string title;   // short status, e.g. "PS2 launch unavailable"
    std::string message; // what happened
    std::string steps;   // what the user can do
    std::string launch_title_id; // app sent to the launcher (the game, or its emulator)
};

// Emulator used for a legacy platform. Nexodion opens the emulator app; the
// forks document no "boot this file" request, so the game is chosen on the
// emulator's own shelf.
struct EmulatorAdapter {
    NavTab platform{NavTab::PS1};
    std::string name;
    std::string repository; // empty = not forked yet (placeholder)
    std::string title_id;   // installed app ID; empty = unknown
};
const std::vector<EmulatorAdapter>& emulator_adapters();
const EmulatorAdapter* emulator_for(NavTab platform);
bool native_runtime_available();
using InstalledCheck = std::function<bool(const std::string& title_id)>;
using LauncherOverrides = std::map<std::string, std::string>; // see LibraryState::launchers

// The launcher actually used for a legacy platform after applying Settings.
struct ResolvedLauncher {
    bool disabled{false};
    bool custom{false};
    std::string name;
    std::string title_id;   // empty = not available (placeholder)
    std::string repository;
};
ResolvedLauncher resolve_launcher(NavTab platform, const LauncherOverrides& overrides);

struct LauncherValidation { bool ok{false}; std::string status; std::string steps; };
LauncherValidation validate_launcher(NavTab platform, const LauncherOverrides& overrides, const InstalledCheck& installed);

LaunchCheck evaluate_launch(const GameEntry& entry, const InstalledCheck& installed, bool runtime_available,
                            const LauncherOverrides& overrides = {});
// Turns a launcher error into a clear status with next steps.
LaunchCheck explain_launch_failure(const std::string& error);
bool native_launch_title(const std::string& title_id, std::string& error);
bool valid_native_title(const std::string& title_id);
bool valid_native_package_path(const std::string& path);
}