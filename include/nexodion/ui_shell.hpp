#pragma once

#include "game_metadata.hpp"
#include "scanner.hpp"
#include "index_cache.hpp"
#include "daemon_client.hpp"
#include "input.hpp"
#include "views.hpp"
#include "installer.hpp"
#include "inspector.hpp"
#include "startup_diagnostics.hpp"
#include "event_log.hpp"
#include "native_services.hpp"
#include "source_manager.hpp"
#include "package_identity.hpp"
#include "library_state.hpp"
#include "native_services.hpp"
#include <thread>
#include <atomic>

#include <string>
#include <vector>
#include <optional>
#include <memory>
#include <functional>
#include <mutex>
#include <set>

namespace nexodion {

enum class DialogType {
    None,
    ShadowMountLock,
    FolderPicker,
    SearchFilter,
    Notification,
    RecoveryGuidance,
    InstallHistory,
    RetryConfirmation,
    PreflightSummary,
    ExportFilePicker,
    StartupDiagnostics,
    Settings,
    Sources,
    SourceEditor,
    SourceActions,
    SourceRemove,
    TextEntry,
    GameActions,
    ConfirmUninstall,
    MountingDiagnostics,
    StorageUsage,
    LauncherSettings,
    LauncherOptions,
    LauncherPicker,
    ErrorHistory,
    InterruptedInstalls,
    GameDetails,
    InstallCheck,
    BulkActions,
    InstallQueue,
    QueueItem
};

struct DialogModel {
    DialogType type{DialogType::None};
    std::string title;
    std::string message;
    std::string secondary_text;
    std::vector<std::string> options;
    int selected_option{0};
    std::string extra_payload;
};

class UiShell {
public:
    UiShell(std::shared_ptr<DaemonLock> daemon_client = nullptr,
            std::string cache_path = "/tmp/nexodion_cache.json",
            std::string install_state_path = "");
    ~UiShell();

    const NavigationBar& navigation() const { return navigation_; }
    void select_tab(NavTab tab);
    SourceManager& sources() { return sources_; }
    const SourceManager& sources() const { return sources_; }
    void show_settings();
    void show_sources();
    void set_sources_path(std::string path) { sources_path_ = std::move(path); }
    void show_source_editor(const std::string& id = "");
    void show_text_entry(const std::string& initial, const std::string& purpose);
    bool handle_source_workflow(const InputManager& input);
    void show_game_actions();
    void run_game_operation(const std::string& operation);
    void refresh_installed_titles();
    bool handle_game_workflow(const InputManager& input);
    void set_sort_by_size(bool value) { set_library_sort(value ? LibrarySort::Size : LibrarySort::Title); }
    // Library browsing preferences (persisted when a UI state path is set).
    void set_library_sort(LibrarySort sort);
    LibrarySort library_sort() const { return library_state_.sort; }
    bool is_favorite(const GameEntry& entry) const;
    void toggle_favorite(const GameEntry& entry);
    void record_played(const GameEntry& entry, int64_t unix_seconds = 0);
    const LibraryState& library_state() const { return library_state_; }
    // Loads saved preferences from `path` (if present) and saves future changes there.
    void set_ui_state_path(std::string path);
    // Launch decision for the focused title (same check the Launch action uses).
    std::optional<LaunchCheck> focused_launch_check() const;
    void set_launch_runtime_override(std::optional<bool> available) { launch_runtime_override_ = available; }
    // Free/total space for every enabled source path; runs transport calls, so the
    // UI uses it from a worker via show_storage_usage().
    struct StorageRow { std::string source_id; std::string storage_type; std::string path; StorageUsage usage; };
    std::vector<StorageRow> query_storage_usage();
    void show_storage_usage();
    // Settings > Emulators & Launchers (PS1/PS2/PS3).
    void show_launcher_settings();
    void show_launcher_options(NavTab platform);
    // "" = built-in emulator, "none" = disabled, else a PS4/PS5 app title ID. Persisted.
    bool set_launcher(NavTab platform, const std::string& choice, std::string* error = nullptr);
    LauncherValidation validate_launcher_for(NavTab platform) const;
    // Searchable list of confirmed-installed apps (name or ID) for choosing a launcher.
    void show_launcher_picker();
    void set_launcher_search(const std::string& query) { launcher_search_ = query; }
    struct InstalledApp { std::string title; std::string title_id; };
    std::vector<InstalledApp> installed_apps(const std::string& query = "") const;
    // Test hook: replaces the confirmed installed-title list.
    void set_installed_titles(std::vector<GameEntry> entries) { installed_entries_ = std::move(entries); }
    bool is_storage_busy() const { return storage_busy_; }
    void tick(); // Apply background results on the UI thread only.
    void show_mounting_diagnostics();
    // Rescans only the shares that had unreadable packages (after fixing a share or its credentials).
    bool retry_unreadable_packages();
    // Re-reads one package (by index into last_scan_statistics().packages) on a worker; applied in tick().
    bool retry_package(size_t index);
    bool is_retrying_package() const { return package_retry_busy_; }
    enum class DiagFilter { All, Unreadable, FilenameFallback };
    void set_diagnostics_search(const std::string& query) { diag_search_ = query; }
    const std::string& diagnostics_search() const { return diag_search_; }
    void set_diagnostics_filter(DiagFilter filter) { diag_filter_ = filter; }
    void set_diagnostics_share(std::string share) { diag_share_ = std::move(share); }
    const ScanStatistics& last_scan_statistics() const { return last_scan_stats_; }
    struct ScanProgress { std::string current_item; size_t items_found{0}; bool running{false}; };
    const ScanProgress& scan_progress() const { return scan_progress_; }
    // Writes the last scan's package diagnostics as JSON; returns false with `error` on failure.
    bool save_scan_diagnostics(const std::string& path, std::string* error = nullptr) const;
    void set_diagnostics_report_path(std::string path) { diagnostics_report_path_ = std::move(path); }
    // Writes only the currently filtered diagnostics (Show/Share/Search) as a
    // shareable JSON report; returns false with `error` on failure.
    bool export_filtered_diagnostics(const std::string& path, std::string* error = nullptr) const;
    void set_diagnostics_export_path(std::string path) { diagnostics_export_path_ = std::move(path); }
    const std::string& diagnostics_export_path() const { return diagnostics_export_path_; }

    // Mode and Layout getters/setters
    LibraryMode library_mode() const { return library_mode_; }
    void set_library_mode(LibraryMode mode);
    void toggle_library_mode();

    ViewLayout view_layout() const { return view_layout_; }
    void set_view_layout(ViewLayout layout) { view_layout_ = layout; }
    void toggle_view_layout();

    FormatFilter format_filter() const { return format_filter_; }
    void set_format_filter(FormatFilter filter);
    void cycle_next_filter();
    void cycle_prev_filter();

    const std::string& search_query() const { return search_query_; }
    void set_search_query(const std::string& query);

    // Active path
    const std::string& current_scan_path() const { return current_scan_path_; }
    void set_scan_path(const std::string& path);

    // Data management
    void set_entries(std::vector<GameEntry> entries);
    const std::vector<GameEntry>& all_entries() const { return all_entries_; }
    const std::vector<GameEntry>& filtered_entries() const { return filtered_entries_; }

    size_t focused_index() const { return focused_index_; }
    void set_focused_index(size_t index);
    std::optional<GameEntry> focused_entry() const;

    // Inspector state and controls
    bool is_inspector_open() const { return inspector_ != nullptr; }
    GameInspector* inspector() { return inspector_.get(); }
    const GameInspector* inspector() const { return inspector_.get(); }
    void open_inspector(const GameEntry& entry);
    void close_inspector();

    // Installer pipeline access
    std::shared_ptr<InstallPipeline> installer() const { return installer_; }
    void set_installer(std::shared_ptr<InstallPipeline> installer);

    // Dialog state
    DialogType active_dialog() const { return dialog_model_.type; }
    const DialogModel& dialog_model() const { return dialog_model_; }
    void show_shadowmount_lock();
    void show_recovery_guidance(const std::string& title, const std::string& message, const std::string& steps);
    void show_install_history();
    void show_retry_confirmation(const InstallTask& task);
    void show_preflight_summary(const GameItem& item, const std::string& title_id);
    void show_preflight_summary(const std::string& package_path, const std::string& title_id, const std::string& display_name = "");
    void dismiss_dialog();
    // Startup diagnostics dialog
    void show_startup_diagnostics(const StartupDiagnosticsReport& report);
    // Error history and activity log (what happened, why, what to do, code).
    void record_event(const std::string& severity, const std::string& area, const std::string& what,
                      const std::string& why = "", const std::string& fix = "");
    EventLog& events() { return events_; }
    const EventLog& events() const { return events_; }
    void set_log_directory(std::string dir) { events_.set_directory(std::move(dir)); }
    void show_error_history();
    // Installs stopped by a restart: Verify (checks the title folder) or Retry.
    bool show_interrupted_installs();
    bool verify_interrupted_install(const std::string& task_id);
    bool retry_interrupted_install(const std::string& task_id);
    void set_apps_root(std::string root) { apps_root_ = std::move(root); }
    // Game details page and pre-install checks for the focused game.
    void show_game_details();
    void show_install_check();
    // Newest package version for an entry, and the console's installed version (empty when unknown).
    std::string newest_package_version(const GameEntry& entry) const;
    std::string update_note(const GameEntry& entry) const;
    // Bulk actions on the games currently shown in the tab.
    void show_bulk_actions();
    // Install queue: reorder, pause/resume, and one-at-a-time continue.
    void show_install_queue();
    void show_queue_item(const std::string& task_id);
    int bulk_set_favorites(bool favorite);
    int bulk_queue_installs();
    int bulk_retry_failed_installs();
    // Settings backup: library preferences + mounting (never passwords).
    bool export_settings_backup(const std::string& path, std::string* error = nullptr) const;
    bool import_settings_backup(const std::string& path, std::string* error = nullptr);
    // Refreshes the library from the shares while the cached list stays on screen.
    bool start_background_refresh();
    bool background_refresh_active() const { return background_refresh_; }
    // Fills share probes (and console-only helper/loader checks) from the current settings.
    StartupDiagnosticsConfig startup_config(StartupDiagnosticsConfig base = {}) const;
    // Re-runs one problem from the startup checks; returns true when it now passes.
    bool retry_startup_check(size_t issue_index);
    const StartupDiagnosticsReport& startup_diagnostics_report() const { return startup_diagnostics_report_; }


    // Install History navigation, filtering, and sorting
    const std::vector<InstallTask>& history_items() const { return history_items_; }
    size_t history_selected_index() const { return history_selected_index_; }
    void history_select_next();
    void history_select_prev();
    void set_history_search_query(const std::string& query);
    const std::string& history_search_query() const { return history_search_query_; }
    std::optional<InstallTask> selected_history_item() const;

    const HistoryFilterOptions& history_filter_options() const { return history_filter_options_; }
    void set_history_filter_options(const HistoryFilterOptions& options);
    void set_history_outcome_filter(std::optional<InstallStatus> outcome);
    void set_history_format_filter(const std::string& format);
    void set_history_date_range(const std::string& from, const std::string& to);
    void set_history_sort(HistorySortField field, HistorySortOrder order);
    void cycle_history_outcome_filter();
    void cycle_history_format_filter();
    void cycle_history_sort();

    // History export
    std::string export_history_json(int indent = 2) const;
    std::string export_history_csv() const;
    bool export_history_to_file(const std::string& filepath, bool as_json = true) const;

    // Export file picker and configurable filenames
    void open_export_file_picker(bool as_json = true);
    void set_export_directory(const std::string& dir);
    void set_export_filename(const std::string& filename);
    void set_export_as_json(bool as_json);
    const std::string& export_directory() const { return export_directory_; }
    const std::string& export_filename() const { return export_filename_; }
    bool export_as_json() const { return export_as_json_; }
    std::string export_full_path() const;
    void cycle_export_directory();
    void cycle_export_filename_preset();
    void toggle_export_format();
    bool execute_export();

    // Preflight summary preference before dispatch
    bool preflight_summary_before_dispatch() const { return preflight_summary_before_dispatch_; }
    void set_preflight_summary_before_dispatch(bool enable) { preflight_summary_before_dispatch_ = enable; }

    // Input processing
    bool handle_pad_input(const InputManager& input);

    // Rendering output models
    std::vector<CardVisual> get_current_view_cards() const;
    std::vector<ButtonPrompt> get_contextual_prompts() const;

    // Scanner integration
    void trigger_scan(bool force_rescan = false,
                      std::function<void(bool success)> on_complete = nullptr);
    bool is_scanning() const { return scanner_.is_scanning(); }

    // Daemon client access
    bool is_shadowmount_available() const;

    // View layout engine access
    ViewLayoutEngine& layout_engine() { return layout_engine_; }
    const ViewLayoutEngine& layout_engine() const { return layout_engine_; }

    // Post-install refresh: updates index cache and any open inspector instance
    void refresh_after_install(const std::string& title_id, const std::string& installed_path);

    // Check if an entry is installed locally
    bool is_title_installed(const std::string& title_id) const;

private:
    std::thread operation_worker_;
    std::atomic<bool> operation_busy_{false};
    struct OperationResult {bool ok;std::string message;std::string operation;std::string title;};
    std::optional<OperationResult> pending_operation_;
    std::optional<std::vector<GameEntry>> pending_installed_;
    std::vector<GameEntry> installed_entries_;
    std::string action_title_;
    ScanStatistics last_scan_stats_;
    ScanProgress scan_progress_;
    std::optional<ScanProgress> pending_progress_;
    std::string diagnostics_report_path_{"/data/nexodion/diagnostics/scan_diagnostics.json"};
    std::string diagnostics_export_path_{"/data/nexodion/diagnostics/scan_diagnostics_filtered.json"};
    std::string sources_path_;
    SourceConfig source_draft_;
    std::string source_edit_id_;
    std::string source_draft_secret_;  // in-session SMB password being edited; never persisted
    std::string cover_cache_dir_{"/data/nexodion/covers"};
    std::string text_value_;
    std::string text_purpose_;
    int text_character_{0};
    LibraryState library_state_;
    std::string ui_state_path_;
    NavTab launcher_platform_{NavTab::PS1};
    std::string launcher_search_;
    std::vector<std::string> picker_ids_; // option row -> title ID
    size_t picker_header_rows_{0};
    std::optional<bool> launch_runtime_override_;
    void save_library_state();
    std::thread storage_worker_;
    std::atomic<bool> storage_busy_{false};
    std::optional<std::vector<StorageRow>> pending_storage_;
    std::vector<StorageRow> storage_rows_;
    void render_storage_usage();
    void update_source_editor();
    void update_text_entry();
    bool persist_source_change(const std::string& before);
    NavigationBar navigation_;
    SourceManager sources_;
    struct ScanResult {
        std::vector<GameEntry> entries;
        std::function<void(bool)> callback;
        std::vector<std::string> roots;
        ScanStatistics stats;
        std::set<std::string> only_sources; // non-empty: partial retry scan merged into existing results
    };
    std::set<std::string> retry_sources_;
    DiagFilter diag_filter_{DiagFilter::All};
    std::string diag_share_; // empty = every share
    std::string diag_search_; // filename, title or title ID
    std::vector<size_t> diag_rows_; // option index -> package index (offset by header rows)
    size_t diag_header_rows_{0};
    std::thread package_retry_worker_;
    std::atomic<bool> package_retry_busy_{false};
    struct PackageRetryResult { size_t index; PackageDiagnostic diag; PackageIdentity identity; };
    std::optional<PackageRetryResult> pending_package_retry_;
    std::vector<std::string> cached_roots_;
    std::mutex event_mutex_;
    std::optional<ScanResult> pending_scan_;
    std::vector<InstallTask> pending_installs_;
    bool pending_batch_{false};
    LibraryMode library_mode_{LibraryMode::InstalledGames};
    ViewLayout view_layout_{ViewLayout::Coverflow};
    FormatFilter format_filter_{FormatFilter::All};
    std::string search_query_;
    std::string current_scan_path_{"/hostapp"};

    std::vector<GameEntry> all_entries_;
    std::vector<GameEntry> filtered_entries_;
    size_t focused_index_{0};

    DialogModel dialog_model_;
    StartupDiagnosticsReport startup_diagnostics_report_;
    EventLog events_;
    std::string apps_root_{"/user/app"};
    std::string settings_backup_path_{"/data/nexodion/backup/settings_backup.json"};
    std::string logs_export_dir_{"/data/nexodion/logs_export"};
    std::vector<std::string> interrupted_ids_;
    std::vector<std::string> queue_ids_;
    std::string queue_item_id_;
    bool background_refresh_{false};
    bool handle_recovery_choice(const std::string& choice);
    void apply_settings_path(const std::string& purpose, const std::string& value);

    // Install History dialog state
    std::string history_search_query_;
    size_t history_selected_index_{0};
    std::vector<InstallTask> history_items_;
    HistoryFilterOptions history_filter_options_;
    bool preflight_summary_before_dispatch_{false};
    void update_history_view();

    // Export configuration and picker state
    std::string export_directory_{"/data/exports"};
    std::string export_filename_{"install_history_export"};
    bool export_as_json_{true};
    size_t export_dir_index_{0};
    size_t export_name_index_{0};
    void update_export_picker_dialog();

    ViewLayoutEngine layout_engine_;
    ScannerEngine scanner_;
    IndexCache cache_;
    std::shared_ptr<DaemonLock> daemon_client_;

    std::unique_ptr<GameInspector> inspector_;
    std::shared_ptr<InstallPipeline> installer_;

    void update_filtered_entries();
    void setup_pipeline_callbacks();
};

} // namespace nexodion
