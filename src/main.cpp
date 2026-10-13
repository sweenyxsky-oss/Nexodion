/**
 * Nexodion - Native Payload & Application Entrypoint
 * 
 * Target: Sony PlayStation 5 (Prospero) & Host Development
 * License: GNU General Public License v3.0
 */

#include "nexodion/daemon_client.hpp"
#include "nexodion/game_metadata.hpp"
#include "nexodion/index_cache.hpp"
#include "nexodion/input.hpp"
#include "nexodion/inspector.hpp"
#include "nexodion/installer.hpp"
#include "nexodion/scanner.hpp"
#include "nexodion/startup_diagnostics.hpp"
#include "nexodion/library_state.hpp"
#include "nexodion/ui_shell.hpp"
#include "nexodion/views.hpp"
#include "nexodion/native_runtime.hpp"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#if defined(__PROSPERO__) || defined(__PS5__)
#define PS5_PLATFORM 1
#else
#define PS5_PLATFORM 0
#endif

namespace {
volatile std::sig_atomic_t g_running = 1;

void handle_signal(int sig) {
    (void)sig;
    g_running = 0;
}

#if !PS5_PLATFORM
std::string resolve_default_path(const std::string& preferred, const std::string& fallback) {
    std::error_code ec;
    if (std::filesystem::exists(preferred, ec)) {
        return preferred;
    }
    return fallback;
}
#endif
} // namespace

int main(int argc, char* argv[]) {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    std::cout << "====================================================\n";
    std::cout << "  Nexodion - Native Launcher v1.0.0     \n";
#if PS5_PLATFORM
    std::cout << "  Target Platform: Sony PlayStation 5 (Prospero)   \n";
#else
    std::cout << "  Target Platform: Host / Simulator Mode           \n";
#endif
    std::cout << "====================================================\n" << std::endl;

    // Parse command line arguments
    std::string root_scan_dir = "/hostapp";
    std::string cache_path = "/data/nexodion_cache.json";
    std::string install_state_path = "/data/nexodion_install_jobs.json";
    std::string daemon_host = "127.0.0.1";
    uint16_t daemon_port = 10101;
    bool headless = false;
    std::string sources_path="/data/nexodion_sources.json";
    bool explicit_sources=false, explicit_cache=false, explicit_jobs=false, explicit_root=false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--headless") {
            headless = true;
        } else if (arg == "--scan-dir" && i + 1 < argc) {
            root_scan_dir = argv[++i];
            explicit_root=true;
        } else if (arg == "--sources" && i + 1 < argc) {
            sources_path = argv[++i];
            explicit_sources=true;
        } else if (arg == "--cache" && i + 1 < argc) {
            cache_path = argv[++i];
            explicit_cache=true;
        } else if (arg == "--jobs" && i + 1 < argc) {
            install_state_path = argv[++i];
            explicit_jobs=true;
        } else if (arg == "--daemon-port" && i + 1 < argc) {
            daemon_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: nexodion.elf [options]\n"
                      << "  --scan-dir <path>     Root library scan directory (default: /hostapp)\n"
                      << "  --sources <path>      Load Local/NFS/SMB source configuration JSON\n"
                      << "  --cache <path>        Path to index cache file (default: /data/nexodion_cache.json)\n"
                      << "  --jobs <path>         Path to job persistence file (default: /data/nexodion_install_jobs.json)\n"
                      << "  --daemon-port <port>  ShadowMountPlus daemon port (default: 10101)\n"
                      << "  --headless            Run single-pass scan and state verification then exit\n";
            return 0;
        }
    }

    // Resolve directories for host vs target PS5
#if !PS5_PLATFORM
    if(!explicit_cache) cache_path = resolve_default_path(cache_path, "/tmp/nexodion_cache.json");
    if(!explicit_jobs) install_state_path = resolve_default_path(install_state_path, "/tmp/nexodion_install_jobs.json");
    if(!explicit_sources) sources_path="/tmp/nexodion_sources.json";
    if (!explicit_root && !std::filesystem::exists(root_scan_dir)) {
        root_scan_dir = "/tmp";
    }
#endif
    (void)explicit_cache;(void)explicit_jobs;(void)explicit_root;

    std::cout << "[Init] Library Scan Root : " << root_scan_dir << "\n";
    std::cout << "[Init] Index Cache File  : " << cache_path << "\n";
    std::cout << "[Init] Job State File    : " << install_state_path << "\n";
    std::cout << "[Init] ShadowMount Daemon: " << daemon_host << ":" << daemon_port << "\n\n";

    // 1. Run Structured Startup Diagnostics (Mount, Daemon, Network, Services)
    nexodion::StartupDiagnosticsConfig diag_config;
    diag_config.mount_path = root_scan_dir;
    diag_config.daemon_host = daemon_host;
    diag_config.daemon_port = daemon_port;
    diag_config.cache_path = cache_path;
    diag_config.jobs_path = install_state_path;

    std::cout << "[Diagnostics] Running startup health checks...\n";
    nexodion::StartupDiagnosticsEngine diag_engine(diag_config);
    auto diag_report = diag_engine.run_all_checks();
    std::cout << diag_report.format_detailed_report() << "\n\n";

    // 2. Initialize ShadowMountPlus Daemon Probe & Feature Lock
    auto daemon_client = std::make_shared<nexodion::DaemonLock>(daemon_host, daemon_port);
    daemon_client->probe();
    const bool daemon_online = daemon_client->is_unlocked();
    std::cout << "[Probe] ShadowMountPlus Daemon Status: " 
              << (daemon_online ? "ONLINE (Features Unlocked)" : "OFFLINE (Remote Features Locked)") << "\n";

    // 3. Initialize UI Shell, Index Cache & Install Pipeline
    nexodion::UiShell shell(daemon_client, cache_path, install_state_path);
    shell.set_scan_path(root_scan_dir);
    shell.set_sources_path(sources_path);
    shell.set_ui_state_path(explicit_sources ? (std::filesystem::path(sources_path).parent_path() / "nexodion_ui_state.json").string()
                                             : std::string(PS5_PLATFORM ? "/data/nexodion/ui_state.json" : "/tmp/nexodion_ui_state.json"));
    std::error_code source_ec;
    if (!std::filesystem::exists(sources_path,source_ec) && !explicit_sources) {
        nexodion::SourceConfig source;
        source.id = "local";
        source.display_name = "Local Library";
        source.mount_point = root_scan_dir;
        if (auto error = shell.sources().add_source(source)) {
            std::cerr << "[Sources] " << *error << '\n'; return 1;
        }
    } else {
        std::string error;
        if (const auto note = nexodion::recover_json_file(sources_path); !note.empty()) {
            std::cerr << "[Sources] " << note << '\n';
            shell.record_event("warning", "settings", "Mounting settings were damaged", note, "Check Settings > Mounting and save again.");
        }
        if(!shell.sources().load_file(sources_path,&error)) {
            // Keep running with no shares instead of exiting; the user can fix them in Mounting.
            std::cerr<<"[Sources] "<<error<<'\n';
            shell.record_event("error", "settings", "Mounting settings could not be loaded", error, "Open Settings > Mounting and add your shares again.");
        }
    }

    std::unique_ptr<nexodion::NativeRuntime> runtime;
    if (!headless) {
        std::string error;
        runtime = nexodion::create_native_runtime(error);
        if (!runtime) { std::cerr << "[Display] " << error << '\n'; return 1; }
        nexodion::paint_native_shell(*runtime, shell, runtime->controller_connected());
        runtime->present();
    }

    shell.set_log_directory(PS5_PLATFORM ? "/data/nexodion/logs" : "/tmp/nexodion_logs");
    // Re-run with the loaded shares, helpers and ELF loader so every problem gets its own Retry.
    diag_report = nexodion::StartupDiagnosticsEngine(shell.startup_config(diag_config)).run_all_checks();
    for (const auto& issue : diag_report.failed_or_warning_items())
        shell.record_event(issue.is_failure() ? "error" : "warning", "startup", issue.summary, issue.details, issue.recovery_guidance);

    // Present on-screen startup diagnostics dialog if warnings or failures were detected
    if (diag_report.has_failures() || diag_report.has_warnings()) {
        std::cout << "[Diagnostics] Active issues detected. Presenting on-screen recovery guidance dialog.\n";
        shell.show_startup_diagnostics(diag_report);
    } else if (shell.show_interrupted_installs()) {
        std::cout << "[Installs] Interrupted installs found; offering Verify or Retry.\n";
    }
    // Refresh the library from the shares while the cached list stays on screen.
    if (!diag_report.has_failures() && shell.start_background_refresh())
        std::cout << "[Library] Background refresh started.\n";

    // 4. Perform initial scan of root path
    std::cout << "[Scanner] Probing directory: " << root_scan_dir << "...\n";
    bool scan_finished = false;
    bool scan_ok=false;
    nexodion::InputManager input_manager;
    shell.trigger_scan(true, [&](bool success) {
        scan_ok=success;
        scan_finished = true;
    });

    // Poll until initial scan completes
    while (!scan_finished && g_running) {
        shell.tick();
        if (runtime) {
            nexodion::PadState pad;
            if (!runtime->poll(pad)) { g_running = 0; break; }
            input_manager.process_pad_input(pad);
            shell.handle_pad_input(input_manager);
            nexodion::paint_native_shell(*runtime, shell, runtime->controller_connected());
            runtime->present();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    const size_t total_games = shell.all_entries().size();
    std::cout << "[Scanner] Scan complete. Found " << total_games << " game entries.\n";

    if (headless) {
        if(!scan_ok){std::cerr<<"[Headless] Scan failed\n";return 1;}
        std::cout << "[Headless] Execution finished successfully.\n";
        return 0;
    }

    // 5. Interactive Application Loop
    std::cout << "[App] Entering interactive UI loop. Ready for DualSense input.\n";
    if(daemon_online) shell.refresh_installed_titles();

    while (g_running) {
        // Poll for inputs and tick animations / background workers
        shell.tick();
        nexodion::PadState pad;
        if (!runtime || !runtime->poll(pad)) break;
        input_manager.process_pad_input(pad);
        shell.handle_pad_input(input_manager);
        nexodion::paint_native_shell(*runtime, shell, runtime->controller_connected());
        runtime->present();
        std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60 Hz tick
    }

    std::cout << "\n[Shutdown] Shutting down Nexodion cleanly...\n";
    return 0;
}
