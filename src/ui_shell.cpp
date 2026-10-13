#include <set>
#include <utility>
#include <filesystem>
#include <chrono>
#include <map>
#include <fstream>
#include "nexodion/ui_shell.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <nlohmann/json.hpp>

namespace nexodion {
using nlohmann::json;

UiShell::UiShell(std::shared_ptr<DaemonLock> daemon_client,
                 std::string cache_path,
                 std::string install_state_path)
    : cache_(std::move(cache_path)),
      daemon_client_(daemon_client ? daemon_client : std::make_shared<DaemonLock>()),
      installer_(std::make_shared<InstallPipeline>(nullptr, std::move(install_state_path))) {
    // Attempt fast index load on startup
    std::vector<GameEntry> cached_entries;
    if (cache_.load(cached_entries)) {
        set_entries(std::move(cached_entries));
    }
    setup_pipeline_callbacks();
    if (installer_) {
        history_filter_options_ = installer_->saved_history_filter_options();
    }
}

UiShell::~UiShell() {
    if(package_retry_worker_.joinable()) package_retry_worker_.join();
    if(operation_worker_.joinable()) operation_worker_.join();
    if(storage_worker_.joinable()) storage_worker_.join();
    scanner_.stop();
    if (installer_) {
        installer_->stop_worker();
        installer_->set_task_finished_callback(nullptr);
        installer_->set_batch_finished_callback(nullptr);
    }
}

void UiShell::tick() {
    std::optional<OperationResult> operation;
    std::optional<ScanResult> scan;
    std::vector<InstallTask> installs;
    bool batch;
    {
        std::lock_guard lock(event_mutex_);
        scan = std::move(pending_scan_);
        pending_scan_.reset();
        installs.swap(pending_installs_);
        batch = pending_batch_;
        pending_batch_ = false;
        operation=std::move(pending_operation_);pending_operation_.reset();
        if(pending_storage_) {
            storage_rows_=std::move(*pending_storage_);pending_storage_.reset();
            if(storage_worker_.joinable()) storage_worker_.join();
            storage_busy_=false;
            if(dialog_model_.type==DialogType::StorageUsage) render_storage_usage();
        }
        if(pending_package_retry_) {
            auto r=std::move(*pending_package_retry_);pending_package_retry_.reset();
            if(package_retry_worker_.joinable()) package_retry_worker_.join();
            package_retry_busy_=false;
            auto& st=last_scan_stats_;
            if(r.index<st.packages.size() && st.packages[r.index].path==r.diag.path) {
                auto& old=st.packages[r.index];
                auto bump=[&](const std::string& s,int d){
                    auto& c=s=="file_contents"?st.packages_from_contents:s=="filename_fallback"?st.packages_from_filename:st.packages_unidentified; c+=d;};
                bump(old.identity_source,-1);bump(r.diag.identity_source,1);
                old=r.diag;
                if(r.identity.ok) for(auto& e:all_entries_) {
                    if(e.source_id!=old.source_id) continue;
                    const bool match=(e.base_game && e.base_game->absolute_path==old.path) ||
                        std::any_of(e.dlcs.begin(),e.dlcs.end(),[&](const GameItem& i){return i.absolute_path==old.path;}) ||
                        std::any_of(e.updates.begin(),e.updates.end(),[&](const GameItem& i){return i.absolute_path==old.path;});
                    if(!match) continue;
                    if(!r.identity.title_id.empty()) e.title_id=r.identity.title_id;
                    if(!r.identity.title.empty()) e.title=r.identity.title;
                    if(!r.identity.content_id.empty()) e.content_id=r.identity.content_id;
                    if(!r.identity.region.empty()) e.region=r.identity.region;
                    e.confidence=r.identity.source==IdentitySource::PackageHeader?MetadataConfidence::Verified:MetadataConfidence::Filename;
                    break;
                }
                update_filtered_entries();
            }
        }
        if(pending_progress_) {scan_progress_=std::move(*pending_progress_);pending_progress_.reset();}
        if(pending_installed_) {installed_entries_=std::move(*pending_installed_);pending_installed_.reset();update_filtered_entries();}
    }
    if(operation) {
        operation_busy_=false;
        if(operation_worker_.joinable()) operation_worker_.join();
        if(operation->operation=="launch") {
            if(operation->ok) {
                for(const auto& e:filtered_entries_) if(e.title_id==operation->title){record_played(e);break;}
                show_recovery_guidance("Launched",operation->message.empty()?operation->title+" was started by the console.":operation->message,"");
            } else {
                const auto why=explain_launch_failure(operation->message);
                show_recovery_guidance(why.title,why.message,why.steps);
            }
        } else if(operation->operation!="refresh" || !operation->ok)
            show_recovery_guidance(operation->ok?"Operation confirmed":"Operation failed",operation->message,
                operation->ok?"":"Check the required service and retry.");
        if(operation->ok && operation->operation!="refresh" && operation->operation!="launch") refresh_installed_titles();
    }
    if (dialog_model_.type == DialogType::MountingDiagnostics) show_mounting_diagnostics();
    if (scan) {
        if (scan->only_sources.empty()) {
            cached_roots_=std::move(scan->roots);
            last_scan_stats_=std::move(scan->stats);
            set_entries(std::move(scan->entries));
        } else {
            // Partial retry: replace only the retried shares' entries and diagnostics.
            const auto& only=scan->only_sources;
            auto merged=all_entries_;
            std::erase_if(merged,[&](const GameEntry& e){return only.count(e.source_id)>0;});
            for(auto& e:scan->entries) merged.push_back(std::move(e));
            for(auto& r:scan->roots) if(std::find(cached_roots_.begin(),cached_roots_.end(),r)==cached_roots_.end()) cached_roots_.push_back(r);
            auto& st=last_scan_stats_;
            std::erase_if(st.packages,[&](const PackageDiagnostic& d){return only.count(d.source_id)>0;});
            for(auto& d:scan->stats.packages) st.packages.push_back(std::move(d));
            st.packages_from_contents=st.packages_from_filename=st.packages_unidentified=0;
            for(const auto& d:st.packages) {
                if(d.identity_source=="file_contents") st.packages_from_contents++;
                else if(d.identity_source=="filename_fallback") st.packages_from_filename++;
                else st.packages_unidentified++;
            }
            set_entries(std::move(merged));
        }
        if (dialog_model_.type == DialogType::MountingDiagnostics) show_mounting_diagnostics();
        std::string error;
        const bool saved=cache_.save_atomic(all_entries_, current_scan_path_, &error, cached_roots_);
        if(!saved) show_recovery_guidance("Cache not saved",error,"Library remains available for this session.");
        if (scan->callback) scan->callback(true);
    }
    for (const auto& task : installs) {
        if (task.status == InstallStatus::Completed) refresh_after_install(task.title_id, task.source_path);
    }
    if ((!installs.empty() || batch) && inspector_ && installer_) inspector_->sync_with_pipeline(*installer_);
}

void UiShell::select_tab(NavTab tab) {
    navigation_.set_active(tab);
    // Browsing local metadata does not require a network mounting daemon.
    library_mode_ = tab == NavTab::Installed ? LibraryMode::InstalledGames : LibraryMode::LibraryBrowser;
    focused_index_ = 0;
    update_filtered_entries();
}

void UiShell::show_settings() {
    dialog_model_ = {};
    dialog_model_.type = DialogType::Settings;
    dialog_model_.title = "Nexodion Settings";
    dialog_model_.message = current_scan_path_;
    dialog_model_.options = {"Mounting", "Rescan Library", "Search Library", "Format: " + std::string(to_string(format_filter_)),
        "Sort: " + std::string(to_string(library_state_.sort)), "Emulators & Launchers", "Refresh Installed", "Diagnostics", "Install History",
        "Install Queue", "Interrupted Installs", "Error History", "Bulk Actions", "Back Up Settings", "Restore Settings", "Close"};
}

void UiShell::show_sources() {
    sources_.set_daemon_unlocked(is_shadowmount_available());
    sources_.refresh_all();
    dialog_model_ = {};
    dialog_model_.type = DialogType::Sources;
    dialog_model_.title = "Mounting";
    dialog_model_.message = sources_.list().empty() ? "No configured sources" : "Configured mount points";
    for (const auto& source : sources_.list()) dialog_model_.options.push_back("Source: " + source.id);
    dialog_model_.options.insert(dialog_model_.options.end(), {"Add Source", "Refresh Sources"});
    // Scan is offered only once at least one enabled source is confirmed mounted.
    for (const auto& source : sources_.list())
        if (source.enabled && sources_.state_of(source.id) == SourceState::Mounted) { dialog_model_.options.push_back("Scan Library"); break; }
    dialog_model_.options.push_back("Scan Diagnostics");
    dialog_model_.options.push_back("Storage Usage");
    dialog_model_.options.push_back("Back");
}

void UiShell::show_mounting_diagnostics() {
    const int selected = dialog_model_.type == DialogType::MountingDiagnostics ? dialog_model_.selected_option : 0;
    const auto& s = last_scan_stats_;
    dialog_model_ = {};
    dialog_model_.type = DialogType::MountingDiagnostics;
    dialog_model_.title = "Mounting Diagnostics";
    dialog_model_.message = scan_progress_.running
        ? "Scanning: " + scan_progress_.current_item + " (" + std::to_string(scan_progress_.items_found) + " found)"
        : package_retry_busy_ ? std::string("Retrying package...")
        : "Last scan: " + std::to_string(s.packages.size()) + " packages";
    dialog_model_.secondary_text = "From file contents: " + std::to_string(s.packages_from_contents) +
        " | Filename fallback: " + std::to_string(s.packages_from_filename) +
        " | Unidentified: " + std::to_string(s.packages_unidentified);
    if (!diag_share_.empty() && std::none_of(s.packages.begin(), s.packages.end(),
            [&](const PackageDiagnostic& p) { return p.source_id == diag_share_; })) diag_share_.clear();
    const char* filter_name = diag_filter_ == DiagFilter::Unreadable ? "Unreadable files"
        : diag_filter_ == DiagFilter::FilenameFallback ? "Filename fallback" : "All packages";
    size_t unreadable = 0;
    for (const auto& p : s.packages) if (!p.reason.empty()) ++unreadable;
    dialog_model_.options = {std::string("Show: ") + filter_name, "Share: " + (diag_share_.empty() ? std::string("All shares") : diag_share_),
                             "Search: " + (diag_search_.empty() ? std::string("(none)") : diag_search_),
                             "Retry Unreadable Packages (" + std::to_string(unreadable) + ")", "Save Diagnostics Report",
                             "Export Filtered Report", "Back to Mounting"};
    diag_header_rows_ = dialog_model_.options.size();
    diag_rows_.clear();
    auto lower = [](std::string v) { std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return std::tolower(c); }); return v; };
    const std::string needle = lower(diag_search_);
    size_t shown = 0;
    for (size_t index = 0; index < s.packages.size(); ++index) {
        const auto& p = s.packages[index];
        if (!needle.empty()) {
            const auto slash = p.path.find_last_of('/');
            const std::string file = slash == std::string::npos ? p.path : p.path.substr(slash + 1);
            if (lower(file).find(needle) == std::string::npos && lower(p.title).find(needle) == std::string::npos &&
                lower(p.title_id).find(needle) == std::string::npos) continue;
        }
        if (diag_filter_ == DiagFilter::Unreadable && p.reason.empty()) continue;
        if (diag_filter_ == DiagFilter::FilenameFallback && p.identity_source != "filename_fallback") continue;
        if (!diag_share_.empty() && p.source_id != diag_share_) continue;
        ++shown;
        const auto slash = p.path.find_last_of('/');
        std::string line = "- " + (slash == std::string::npos ? p.path : p.path.substr(slash + 1)) + " [" + p.source_id + "] ";
        line += p.identity_source == "file_contents" ? "file contents" : p.identity_source == "filename_fallback" ? "filename fallback" : "unidentified";
        if (!p.title_id.empty()) line += " " + p.title_id;
        if (!p.reason.empty()) line += ": " + p.reason + " [press to retry]";
        diag_rows_.push_back(index);
        dialog_model_.options.push_back(std::move(line));
    }
    dialog_model_.secondary_text += " | Showing: " + std::to_string(shown);
    dialog_model_.selected_option = std::clamp(selected, 0, static_cast<int>(dialog_model_.options.size()) - 1);
}

bool UiShell::retry_unreadable_packages() {
    std::set<std::string> shares;
    for (const auto& p : last_scan_stats_.packages)
        if (!p.reason.empty() && (diag_share_.empty() || p.source_id == diag_share_)) shares.insert(p.source_id);
    if (shares.empty() || scanner_.is_scanning()) return false;
    retry_sources_ = std::move(shares);
    trigger_scan(true);
    return true;
}

bool UiShell::retry_package(size_t index) {
    if (package_retry_busy_ || index >= last_scan_stats_.packages.size()) return false;
    const PackageDiagnostic diag = last_scan_stats_.packages[index];
    if (diag.reason.empty()) return false;
    sources_.refresh_all(); // picks up updated mounts or session credentials
    std::optional<FileServerAddress> address;
    bool found = false;
    for (const auto& spec : sources_.active_scan_specs()) {
        const auto key = is_remote_root_path(spec.path) ? spec.path : std::filesystem::path(spec.path).lexically_normal().string();
        if (key == diag.root) { found = true; if (is_remote_root_path(spec.path)) address = spec.address; break; }
    }
    if (!found) return false;
    if (package_retry_worker_.joinable()) package_retry_worker_.join();
    package_retry_busy_ = true;
    package_retry_worker_ = std::thread([this, index, diag, address] {
        PackageRetryResult result{index, diag, {}};
        std::string error;
        std::unique_ptr<IFileTransport> transport;
        if (address) transport = make_file_transport(*address, error);
        if (address && !transport) {
            result.diag.reason = "Share unavailable: " + error;
        } else {
            FileVfs vfs(std::move(transport), address ? address->remote_path : "/");
            uint64_t size = 0;
            std::string read_error;
            PackageIdentity id;
            if (!vfs.regular_file_size(diag.path, size, read_error)) id.error = "File could not be opened.";
            else id = identify_package([&](uint64_t off, size_t len, std::vector<unsigned char>& out) {
                    std::string e; const bool ok = vfs.read_range(diag.path, off, len, out, e);
                    if (!ok && read_error.empty()) { read_error = e; } return ok; }, size);
            result.diag.reason.clear();
            if (!id.ok) {
                result.diag.reason = read_error.empty() ? id.error : id.error + " (" + read_error + ")";
                const auto slash = diag.path.find_last_of('/');
                id = identify_from_filename(slash == std::string::npos ? diag.path : diag.path.substr(slash + 1));
            }
            result.diag.identity_source = !id.ok ? "unidentified"
                : id.source == IdentitySource::PackageHeader ? "file_contents" : "filename_fallback";
            result.diag.title_id = id.title_id;
            result.diag.title = id.title;
            result.identity = id;
        }
        std::lock_guard lock(event_mutex_);
        pending_package_retry_ = std::move(result);
    });
    return true;
}

bool UiShell::save_scan_diagnostics(const std::string& path, std::string* error) const {
    nlohmann::json j;
    j["generated_at_unix"] = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    j["summary"] = {{"packages", last_scan_stats_.packages.size()}, {"from_file_contents", last_scan_stats_.packages_from_contents},
                    {"filename_fallback", last_scan_stats_.packages_from_filename}, {"unidentified", last_scan_stats_.packages_unidentified},
                    {"files_scanned", last_scan_stats_.files_scanned}, {"directories_scanned", last_scan_stats_.directories_scanned}};
    j["packages"] = nlohmann::json::array();
    for (const auto& p : last_scan_stats_.packages)
        j["packages"].push_back({{"path", p.path}, {"source_id", p.source_id}, {"identity_source", p.identity_source},
                                 {"title_id", p.title_id}, {"reason", p.reason}});
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) { if (error) *error = "Cannot write " + tmp; return false; }
        out << j.dump(2);
        if (!out) { if (error) *error = "Write failed for " + tmp; return false; }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) { if (error) *error = ec.message(); return false; }
    return true;
}

bool UiShell::export_filtered_diagnostics(const std::string& path, std::string* error) const {
    auto lower = [](std::string v) { std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return std::tolower(c); }); return v; };
    const std::string needle = lower(diag_search_);
    auto matches = [&](const PackageDiagnostic& p) {
        if (!needle.empty()) {
            const auto slash = p.path.find_last_of('/');
            const std::string file = slash == std::string::npos ? p.path : p.path.substr(slash + 1);
            if (lower(file).find(needle) == std::string::npos && lower(p.title).find(needle) == std::string::npos &&
                lower(p.title_id).find(needle) == std::string::npos) return false;
        }
        if (diag_filter_ == DiagFilter::Unreadable && p.reason.empty()) return false;
        if (diag_filter_ == DiagFilter::FilenameFallback && p.identity_source != "filename_fallback") return false;
        if (!diag_share_.empty() && p.source_id != diag_share_) return false;
        return true;
    };
    nlohmann::json j;
    j["report"] = "nexodion_scan_diagnostics_filtered";
    j["generated_at_unix"] = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    j["filters"] = {{"show", diag_filter_ == DiagFilter::Unreadable ? "unreadable"
                        : diag_filter_ == DiagFilter::FilenameFallback ? "filename_fallback" : "all"},
                    {"share", diag_share_}, {"search", diag_search_}};
    j["total_packages"] = last_scan_stats_.packages.size();
    size_t matched = 0, from_contents = 0, from_filename = 0, unidentified = 0;
    j["packages"] = nlohmann::json::array();
    for (const auto& p : last_scan_stats_.packages) {
        if (!matches(p)) continue;
        ++matched;
        if (p.identity_source == "file_contents") ++from_contents;
        else if (p.identity_source == "filename_fallback") ++from_filename;
        else ++unidentified;
        j["packages"].push_back({{"path", p.path}, {"source_id", p.source_id}, {"root", p.root},
                                 {"identity_source", p.identity_source}, {"title_id", p.title_id},
                                 {"title", p.title}, {"reason", p.reason}});
    }
    j["matched"] = matched;
    j["matched_by_identity"] = {{"file_contents", from_contents}, {"filename_fallback", from_filename}, {"unidentified", unidentified}};
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) { if (error) *error = "Cannot write " + tmp; return false; }
        out << j.dump(2);
        if (!out) { if (error) *error = "Write failed for " + tmp; return false; }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) { if (error) *error = ec.message(); return false; }
    return true;
}

void UiShell::set_installer(std::shared_ptr<InstallPipeline> installer) {
    if (installer_) {
        installer_->stop_worker();
        installer_->set_task_finished_callback(nullptr);
        installer_->set_batch_finished_callback(nullptr);
    }
    installer_ = std::move(installer);
    setup_pipeline_callbacks();
    if (installer_) {
        history_filter_options_ = installer_->saved_history_filter_options();
    }
}

void UiShell::setup_pipeline_callbacks() {
    if (!installer_) return;

    installer_->set_task_finished_callback([this](const InstallTask& task) {
        std::lock_guard lock(event_mutex_);
        pending_installs_.push_back(task);
    });

    installer_->set_batch_finished_callback([this](const BatchProgress& batch) {
        (void)batch;
        std::lock_guard lock(event_mutex_);
        pending_batch_ = true;
    });
}

bool UiShell::is_shadowmount_available() const {
    if (!daemon_client_) return false;
    return daemon_client_->is_unlocked();
}

void UiShell::set_library_mode(LibraryMode mode) {
    if (mode == LibraryMode::LibraryBrowser) {
        // Native network file access removed the daemon dependency for
        // browsing; the lock only gates daemon-dependent actions.
        bool has_usable_sources = false;
        for (const auto& config : sources_.list()) if (config.enabled) { has_usable_sources = true; break; }
        if (!is_shadowmount_available() && !has_usable_sources) {
            show_shadowmount_lock();
            return;
        }
    }
    library_mode_ = mode;
    navigation_.set_active(mode == LibraryMode::InstalledGames ? NavTab::Installed : NavTab::PS5);
    update_filtered_entries();
    dismiss_dialog();
}

void UiShell::toggle_library_mode() {
    if (library_mode_ == LibraryMode::InstalledGames) {
        set_library_mode(LibraryMode::LibraryBrowser);
    } else {
        set_library_mode(LibraryMode::InstalledGames);
    }
}

void UiShell::toggle_view_layout() {
    if (view_layout_ == ViewLayout::Coverflow) {
        view_layout_ = ViewLayout::BlockGrid;
    } else {
        view_layout_ = ViewLayout::Coverflow;
    }
    save_library_state();
}

void UiShell::set_format_filter(FormatFilter filter) {
    format_filter_ = filter;
    update_filtered_entries();
}

void UiShell::cycle_next_filter() {
    format_filter_ = next_filter(format_filter_);
    update_filtered_entries();
}

void UiShell::cycle_prev_filter() {
    format_filter_ = prev_filter(format_filter_);
    update_filtered_entries();
}

void UiShell::set_search_query(const std::string& query) {
    search_query_ = query;
    update_filtered_entries();
}

void UiShell::set_scan_path(const std::string& path) {
    current_scan_path_ = path;
}

void UiShell::set_entries(std::vector<GameEntry> entries) {
    all_entries_ = std::move(entries);
    // First-seen times power "Recently Added"; titles keep their original time across rescans.
    bool added = false;
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    for (const auto& e : all_entries_) added |= library_state_.first_seen.emplace(library_key(e), now).second;
    if (added) save_library_state();
    update_filtered_entries();
}

void UiShell::set_focused_index(size_t index) {
    if (filtered_entries_.empty()) {
        focused_index_ = 0;
        return;
    }
    if (index >= filtered_entries_.size()) {
        focused_index_ = filtered_entries_.size() - 1;
    } else {
        focused_index_ = index;
    }
}

std::optional<GameEntry> UiShell::focused_entry() const {
    if (filtered_entries_.empty() || focused_index_ >= filtered_entries_.size()) {
        return std::nullopt;
    }
    return filtered_entries_[focused_index_];
}

void UiShell::open_inspector(const GameEntry& entry) {
    inspector_ = std::make_unique<GameInspector>(entry);
    if (installer_) {
        inspector_->sync_with_pipeline(*installer_);
    }
}

void UiShell::close_inspector() {
    inspector_.reset();
}

void UiShell::show_shadowmount_lock() {
    dialog_model_.type = DialogType::ShadowMountLock;
    dialog_model_.title = "Feature Locked";
    dialog_model_.message = "ShadowMount Plus is required to unlock these features.";
    dialog_model_.secondary_text = "Ensure the ShadowMountPlus daemon is running on port 10101 to enable remote library browsing.";
    dialog_model_.options = {"Retry Connection", "Return to Installed Games"};
    dialog_model_.selected_option = 0;
    dialog_model_.extra_payload.clear();
}

void UiShell::show_recovery_guidance(const std::string& title,
                                     const std::string& message,
                                     const std::string& steps) {
    {
        // Every message shown to the user lands in Error History once, with a severity.
        const auto last = events_.recent(1);
        if (last.empty() || (last[0].what != title && last[0].why != message)) {
            std::string lower = title;
            for (auto& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            auto has = [&](const char* w) { return lower.find(w) != std::string::npos; };
            const char* severity = (has("fail") || has("not ") || has("cannot") || has("unavailable") || has("refused") || has("invalid") || has("missing"))
                ? "error" : (has("saved") || has("exported") || has("confirmed") || has("restored") || has("queued") || has("updated") || has("launched") || has("backed up"))
                ? "info" : "warning";
            record_event(severity, "app", title.empty() ? "Installation problem" : title, message, steps);
        }
    }
    dialog_model_.type = DialogType::RecoveryGuidance;
    dialog_model_.title = title.empty() ? "Installation Troubleshooting" : title;
    dialog_model_.message = message;
    dialog_model_.secondary_text = steps;
    dialog_model_.options = {"Dismiss", "Retry Install"};
    dialog_model_.selected_option = 0;
    dialog_model_.extra_payload = steps;
}

void UiShell::show_install_history() {
    dialog_model_.type = DialogType::InstallHistory;
    dialog_model_.title = "Install History & Diagnostics";
    history_selected_index_ = 0;
    if (installer_) {
        history_filter_options_ = installer_->saved_history_filter_options();
    }
    update_history_view();
}

void UiShell::show_retry_confirmation(const InstallTask& task) {
    dialog_model_.type = DialogType::RetryConfirmation;
    dialog_model_.title = "Confirm Retry: " + task.display_name;

    std::string target_dir = installer_ ? installer_->target_storage_dir() : "/user/app";
    const auto preflight = run_package_preflight(task.source_path, target_dir);

    std::ostringstream ss;
    ss << "Original Failure Details:\n";
    ss << "- Task ID: " << task.task_id << " (Attempt #" << (task.retry_count + 1) << ")\n";
    ss << "- Failure Code: [" << install_error_name(task.error_code) << "] " << to_string(task.error_code) << "\n";
    if (!task.error_message.empty()) {
        ss << "- Error Message: " << task.error_message << "\n";
    }
    if (!task.completed_timestamp.empty()) {
        ss << "- Failed Timestamp: " << task.completed_timestamp << "\n";
    }
    ss << "\n" << preflight.summary.formatted_summary();

    dialog_model_.message = preflight.passed
        ? "Preflight checks passed on re-run. Proceed with job retry?"
        : "Preflight checks failed on re-run: " + preflight.error_message;
    dialog_model_.secondary_text = ss.str();

    if (preflight.passed) {
        dialog_model_.options = {"Confirm Retry", "Cancel"};
    } else {
        dialog_model_.options = {"Dismiss"};
    }
    dialog_model_.selected_option = 0;

    json payload;
    payload["task_id"] = task.task_id;
    payload["source_path"] = task.source_path;
    payload["passed"] = preflight.passed;
    json sj;
    sj["all_passed"] = preflight.summary.all_passed;
    sj["readability"] = {{"passed", preflight.summary.readability.passed}, {"details", preflight.summary.readability.details}, {"failure_reason", preflight.summary.readability.failure_reason}, {"remediation", preflight.summary.readability.remediation}};
    sj["metadata"] = {{"passed", preflight.summary.metadata.passed}, {"details", preflight.summary.metadata.details}, {"failure_reason", preflight.summary.metadata.failure_reason}, {"remediation", preflight.summary.metadata.remediation}};
    sj["storage"] = {{"passed", preflight.summary.storage.passed}, {"details", preflight.summary.storage.details}, {"failure_reason", preflight.summary.storage.failure_reason}, {"remediation", preflight.summary.storage.remediation}};
    payload["summary_snapshot"] = sj;
    dialog_model_.extra_payload = payload.dump();
}

void UiShell::show_preflight_summary(const GameItem& item, const std::string& title_id) {
    dialog_model_.type = DialogType::PreflightSummary;
    dialog_model_.title = "Preflight Inspection: " + item.filename;

    std::string target_dir = installer_ ? installer_->target_storage_dir() : "/user/app";
    const auto preflight = run_package_preflight(item.absolute_path, target_dir);

    dialog_model_.message = preflight.passed
        ? "All preflight checks passed. Package is ready for installation."
        : "Preflight check failed: " + preflight.error_message;
    dialog_model_.secondary_text = preflight.summary.formatted_summary();

    if (preflight.passed) {
        dialog_model_.options = {"Proceed with Install", "Cancel"};
    } else {
        dialog_model_.options = {"Dismiss"};
    }
    dialog_model_.selected_option = 0;

    json payload;
    payload["title_id"] = title_id;
    payload["filename"] = item.filename;
    payload["absolute_path"] = item.absolute_path;
    payload["format"] = item.format;
    payload["size_bytes"] = item.size_bytes;
    payload["display_name"] = item.display_name;
    payload["version"] = item.version;
    payload["kind"] = to_string(item.kind);
    payload["passed"] = preflight.passed;
    json sj;
    sj["all_passed"] = preflight.summary.all_passed;
    sj["readability"] = {{"passed", preflight.summary.readability.passed}, {"details", preflight.summary.readability.details}, {"failure_reason", preflight.summary.readability.failure_reason}, {"remediation", preflight.summary.readability.remediation}};
    sj["metadata"] = {{"passed", preflight.summary.metadata.passed}, {"details", preflight.summary.metadata.details}, {"failure_reason", preflight.summary.metadata.failure_reason}, {"remediation", preflight.summary.metadata.remediation}};
    sj["storage"] = {{"passed", preflight.summary.storage.passed}, {"details", preflight.summary.storage.details}, {"failure_reason", preflight.summary.storage.failure_reason}, {"remediation", preflight.summary.storage.remediation}};
    payload["summary_snapshot"] = sj;
    dialog_model_.extra_payload = payload.dump();
}

void UiShell::show_preflight_summary(const std::string& package_path, const std::string& title_id, const std::string& display_name) {
    GameItem item;
    item.filename = package_path.substr(package_path.find_last_of("/\\") + 1);
    item.absolute_path = package_path;
    item.display_name = display_name.empty() ? item.filename : display_name;
    auto dot = item.filename.find_last_of('.');
    if (dot != std::string::npos) item.format = item.filename.substr(dot + 1);
    show_preflight_summary(item, title_id);
}

void UiShell::update_history_view() {
    if (!installer_) {
        history_items_.clear();
        dialog_model_.message = "No installer pipeline connected";
        dialog_model_.secondary_text.clear();
        return;
    }

    history_filter_options_.query = history_search_query_;
    history_items_ = installer_->filter_and_sort_history(history_filter_options_);

    std::string badge = "[Status: " + (history_filter_options_.outcome_filter ? std::string(to_string(*history_filter_options_.outcome_filter)) : "All") +
                        " | Format: " + (history_filter_options_.format_filter.empty() ? "All" : history_filter_options_.format_filter) +
                        " | Sort: " + to_string(history_filter_options_.sort_field) + " " + to_string(history_filter_options_.sort_order) + "]";

    if (history_items_.empty()) {
        history_selected_index_ = 0;
        dialog_model_.message = history_search_query_.empty() ? "No install jobs recorded" : "No jobs matching query: " + history_search_query_;
        dialog_model_.secondary_text = badge + "\n△ Filter Status | □ Filter Format | L1/R1 Sort | ○ Close";
        return;
    }

    if (history_selected_index_ >= history_items_.size()) {
        history_selected_index_ = history_items_.size() - 1;
    }

    const auto& selected = history_items_[history_selected_index_];
    dialog_model_.message = "[" + std::to_string(history_selected_index_ + 1) + "/" + std::to_string(history_items_.size()) + "] " +
                            selected.display_name + " (" + selected.title_id + ") - " + to_string(selected.status);

    std::string sec = badge + "\nFile: " + selected.package_filename + " | Format: " + selected.format + "\n";
    sec += "Created: " + (selected.created_timestamp.empty() ? "Unknown" : selected.created_timestamp);
    if (!selected.completed_timestamp.empty()) {
        sec += " | Completed: " + selected.completed_timestamp;
    }
    if (selected.retry_count > 0) {
        sec += "\nRetry #" + std::to_string(selected.retry_count) + " (Orig Task: " + selected.original_task_id +
               ", Orig Error: [" + install_error_name(selected.original_error_code) + "])";
    }
    if (!selected.error_message.empty()) {
        sec += "\nError: " + selected.error_message;
    }
    if (!selected.recovery_guidance.empty()) {
        sec += "\nRecovery: " + selected.recovery_guidance;
    }
    dialog_model_.secondary_text = sec;
}

void UiShell::history_select_next() {
    if (history_items_.empty()) return;
    history_selected_index_ = (history_selected_index_ + 1) % history_items_.size();
    update_history_view();
}

void UiShell::history_select_prev() {
    if (history_items_.empty()) return;
    history_selected_index_ = (history_selected_index_ + history_items_.size() - 1) % history_items_.size();
    update_history_view();
}

void UiShell::set_history_search_query(const std::string& query) {
    history_search_query_ = query;
    history_selected_index_ = 0;
    update_history_view();
}

std::optional<InstallTask> UiShell::selected_history_item() const {
    if (history_selected_index_ < history_items_.size()) {
        return history_items_[history_selected_index_];
    }
    return std::nullopt;
}

void UiShell::set_history_filter_options(const HistoryFilterOptions& options) {
    history_filter_options_ = options;
    history_selected_index_ = 0;
    if (installer_) {
        installer_->set_saved_history_filter_options(history_filter_options_);
    }
    update_history_view();
}

void UiShell::set_history_outcome_filter(std::optional<InstallStatus> outcome) {
    history_filter_options_.outcome_filter = outcome;
    history_selected_index_ = 0;
    if (installer_) {
        installer_->set_saved_history_filter_options(history_filter_options_);
    }
    update_history_view();
}

void UiShell::set_history_format_filter(const std::string& format) {
    history_filter_options_.format_filter = format;
    history_selected_index_ = 0;
    if (installer_) {
        installer_->set_saved_history_filter_options(history_filter_options_);
    }
    update_history_view();
}

void UiShell::set_history_date_range(const std::string& from, const std::string& to) {
    history_filter_options_.date_from = from;
    history_filter_options_.date_to = to;
    history_selected_index_ = 0;
    if (installer_) {
        installer_->set_saved_history_filter_options(history_filter_options_);
    }
    update_history_view();
}

void UiShell::set_history_sort(HistorySortField field, HistorySortOrder order) {
    history_filter_options_.sort_field = field;
    history_filter_options_.sort_order = order;
    history_selected_index_ = 0;
    if (installer_) {
        installer_->set_saved_history_filter_options(history_filter_options_);
    }
    update_history_view();
}

void UiShell::cycle_history_outcome_filter() {
    if (!history_filter_options_.outcome_filter.has_value()) {
        history_filter_options_.outcome_filter = InstallStatus::Failed;
    } else if (*history_filter_options_.outcome_filter == InstallStatus::Failed) {
        history_filter_options_.outcome_filter = InstallStatus::Interrupted;
    } else if (*history_filter_options_.outcome_filter == InstallStatus::Interrupted) {
        history_filter_options_.outcome_filter = InstallStatus::Completed;
    } else if (*history_filter_options_.outcome_filter == InstallStatus::Completed) {
        history_filter_options_.outcome_filter = InstallStatus::Queued;
    } else {
        history_filter_options_.outcome_filter = std::nullopt;
    }
    history_selected_index_ = 0;
    if (installer_) {
        installer_->set_saved_history_filter_options(history_filter_options_);
    }
    update_history_view();
}

void UiShell::cycle_history_format_filter() {
    if (history_filter_options_.format_filter.empty()) {
        history_filter_options_.format_filter = "pkg";
    } else if (history_filter_options_.format_filter == "pkg") {
        history_filter_options_.format_filter = "ffpkg";
    } else if (history_filter_options_.format_filter == "ffpkg") {
        history_filter_options_.format_filter = "exfat";
    } else {
        history_filter_options_.format_filter.clear();
    }
    history_selected_index_ = 0;
    if (installer_) {
        installer_->set_saved_history_filter_options(history_filter_options_);
    }
    update_history_view();
}

void UiShell::cycle_history_sort() {
    if (history_filter_options_.sort_field == HistorySortField::Timestamp &&
        history_filter_options_.sort_order == HistorySortOrder::Descending) {
        history_filter_options_.sort_order = HistorySortOrder::Ascending;
    } else if (history_filter_options_.sort_field == HistorySortField::Timestamp &&
               history_filter_options_.sort_order == HistorySortOrder::Ascending) {
        history_filter_options_.sort_field = HistorySortField::PackageName;
        history_filter_options_.sort_order = HistorySortOrder::Ascending;
    } else if (history_filter_options_.sort_field == HistorySortField::PackageName &&
               history_filter_options_.sort_order == HistorySortOrder::Ascending) {
        history_filter_options_.sort_order = HistorySortOrder::Descending;
    } else {
        history_filter_options_.sort_field = HistorySortField::Timestamp;
        history_filter_options_.sort_order = HistorySortOrder::Descending;
    }
    history_selected_index_ = 0;
    if (installer_) {
        installer_->set_saved_history_filter_options(history_filter_options_);
    }
    update_history_view();
}

std::string UiShell::export_history_json(int indent) const {
    if (!installer_) return "[]";
    return installer_->export_history_to_json(history_items_, indent);
}

std::string UiShell::export_history_csv() const {
    if (!installer_) return "";
    return installer_->export_history_to_csv(history_items_);
}

bool UiShell::export_history_to_file(const std::string& filepath, bool as_json) const {
    if (!installer_) return false;
    return installer_->export_history_to_file(filepath, history_items_, as_json);
}

namespace {
const std::vector<std::string> kPresetExportDirs = {
    "/data/exports",
    "/user/home",
    "/usb0",
    "/usb1",
    "/hostapp",
    "/tmp"
};

const std::vector<std::string> kPresetExportNames = {
    "install_history_export",
    "ps5_install_history",
    "install_audit_log",
    "install_history_filtered"
};
}

std::string UiShell::export_full_path() const {
    std::string ext = export_as_json_ ? ".json" : ".csv";
    std::filesystem::path p(export_directory_);
    std::string fn = export_filename_;
    if (fn.size() >= ext.size() && fn.compare(fn.size() - ext.size(), ext.size(), ext) == 0) {
        p /= fn;
    } else {
        p /= (fn + ext);
    }
    return p.string();
}

void UiShell::open_export_file_picker(bool as_json) {
    export_as_json_ = as_json;
    dialog_model_.type = DialogType::ExportFilePicker;
    update_history_view();
    update_export_picker_dialog();
}

void UiShell::set_export_directory(const std::string& dir) {
    export_directory_ = dir;
    if (dialog_model_.type == DialogType::ExportFilePicker) {
        update_export_picker_dialog();
    }
}

void UiShell::set_export_filename(const std::string& filename) {
    export_filename_ = filename;
    if (dialog_model_.type == DialogType::ExportFilePicker) {
        update_export_picker_dialog();
    }
}

void UiShell::set_export_as_json(bool as_json) {
    export_as_json_ = as_json;
    if (dialog_model_.type == DialogType::ExportFilePicker) {
        update_export_picker_dialog();
    }
}

void UiShell::cycle_export_directory() {
    export_dir_index_ = (export_dir_index_ + 1) % kPresetExportDirs.size();
    export_directory_ = kPresetExportDirs[export_dir_index_];
    if (dialog_model_.type == DialogType::ExportFilePicker) {
        update_export_picker_dialog();
    }
}

void UiShell::cycle_export_filename_preset() {
    export_name_index_ = (export_name_index_ + 1) % kPresetExportNames.size();
    export_filename_ = kPresetExportNames[export_name_index_];
    if (dialog_model_.type == DialogType::ExportFilePicker) {
        update_export_picker_dialog();
    }
}

void UiShell::toggle_export_format() {
    export_as_json_ = !export_as_json_;
    if (dialog_model_.type == DialogType::ExportFilePicker) {
        update_export_picker_dialog();
    }
}

void UiShell::update_export_picker_dialog() {
    dialog_model_.title = "Export Install History";
    dialog_model_.message = "Configure destination directory, filename, and format for history export:";
    
    std::string path = export_full_path();
    std::ostringstream ss;
    ss << "Format: " << (export_as_json_ ? "JSON (Structured Audit Log)" : "CSV (RFC 4180 Table)") << "\n";
    ss << "Destination Path: " << path << "\n";
    ss << "Records to Export: " << history_items_.size() << " items\n";
    ss << "Controls: [X] Action | [Sqr] Format | [Tri] Directory | [L1/R1] Filename | [O] Back";
    dialog_model_.secondary_text = ss.str();

    dialog_model_.options = {
        "Export Now",
        export_as_json_ ? "Format: Switch to CSV" : "Format: Switch to JSON",
        "Directory: " + export_directory_,
        "Filename: " + export_filename_,
        "Cancel"
    };
    if (dialog_model_.selected_option >= static_cast<int>(dialog_model_.options.size())) {
        dialog_model_.selected_option = 0;
    }
}

bool UiShell::execute_export() {
    update_history_view();
    const std::string path = export_full_path();
    bool success = export_history_to_file(path, export_as_json_);

    dialog_model_.type = DialogType::Notification;
    dialog_model_.options = {"Dismiss"};
    dialog_model_.selected_option = 0;

    if (success) {
        dialog_model_.title = "Export Succeeded";
        dialog_model_.message = "Install history successfully exported!";
        std::ostringstream ss;
        ss << "File: " << path << "\n";
        ss << "Format: " << (export_as_json_ ? "JSON" : "CSV") << "\n";
        ss << "Records: " << history_items_.size() << " items written";
        dialog_model_.secondary_text = ss.str();
    } else {
        dialog_model_.title = "Export Failed";
        dialog_model_.message = "Unable to write export file to destination: " + path;
        dialog_model_.secondary_text = "Remediation: Verify directory exists and has write permissions. Check available storage.";
    }
    return success;
}

void UiShell::show_startup_diagnostics(const StartupDiagnosticsReport& report) {
    startup_diagnostics_report_ = report;
    dialog_model_.type = DialogType::StartupDiagnostics;
    dialog_model_.title = "Startup Diagnostics & Recovery";
    dialog_model_.message = report.format_summary();
    dialog_model_.secondary_text = report.format_on_screen_guidance();
    dialog_model_.options = {"Dismiss / Continue", "View Detailed Report", "Retry All"};
    for (const auto& issue : report.failed_or_warning_items()) dialog_model_.options.push_back("Retry: " + issue.name);
    dialog_model_.selected_option = 0;
    dialog_model_.extra_payload = report.format_detailed_report();
}

StartupDiagnosticsConfig UiShell::startup_config(StartupDiagnosticsConfig base) const {
    if (base.mount_path.empty() || base.mount_path == "/hostapp") base.mount_path = current_scan_path_.empty() ? base.mount_path : current_scan_path_;
    if (daemon_client_) { auto t = daemon_client_->target(); base.daemon_host = t.first; base.daemon_port = t.second; }
    base.require_elf_loader = native_runtime_available();
    if (!native_runtime_available()) base.helper_names.clear(); // helpers only ship in the console package
    base.shares.clear();
    for (const auto& cfg : sources_.list()) {
        if (!cfg.enabled) continue;
        const std::string secret = sources_.session_secret(cfg.id);
        base.shares.push_back({cfg.id, cfg.display_name.empty() ? cfg.id : cfg.display_name,
            [cfg, secret](std::string& detail) {
                auto src = make_source(cfg, true);
                if (!secret.empty()) src->set_session_secret(secret);
                const auto state = src->refresh();
                detail = std::string(to_string(cfg.kind)) + " " + (cfg.host.empty() ? cfg.mount_point : cfg.host + ":" + cfg.remote_path) +
                         " - " + to_string(state);
                return state == SourceState::Mounted;
            }});
    }
    return base;
}

bool UiShell::retry_startup_check(size_t issue_index) {
    const auto issues = startup_diagnostics_report_.failed_or_warning_items();
    if (issue_index >= issues.size()) return false;
    StartupDiagnosticsEngine engine(startup_config());
    const auto fresh = engine.retry(issues[issue_index]);
    startup_diagnostics_report_.replace_item(fresh);
    record_event(fresh.is_ok() ? "info" : "error", "startup", fresh.summary, fresh.details, fresh.recovery_guidance);
    const auto report = startup_diagnostics_report_;
    show_startup_diagnostics(report);
    return fresh.is_ok();
}

void UiShell::dismiss_dialog() {
    dialog_model_.type = DialogType::None;
    dialog_model_.title.clear();
    dialog_model_.message.clear();
    dialog_model_.secondary_text.clear();
    dialog_model_.options.clear();
    dialog_model_.selected_option = 0;
    dialog_model_.extra_payload.clear();
}

bool UiShell::is_title_installed(const std::string& title_id) const {
    if (title_id.empty()) return false;
    for (const auto& entry : all_entries_) {
        if (entry.title_id == title_id && entry.installed) {
            return true;
        }
    }
    for(const auto& entry:installed_entries_) if(entry.title_id==title_id) return true;
    return false;
}

void UiShell::refresh_after_install(const std::string& title_id, const std::string& installed_path) {
    (void)installed_path;
    for (auto& entry : all_entries_) {
        if (entry.title_id == title_id) {
            entry.confidence = MetadataConfidence::Verified;
            entry.calculate_totals();

            if (inspector_ && inspector_->title_id() == title_id) {
                inspector_->update_entry(entry);
                if (installer_) {
                    inspector_->sync_with_pipeline(*installer_);
                }
            }
            break;
        }
    }

    cache_.save_atomic(all_entries_, current_scan_path_, nullptr, cached_roots_);
    update_filtered_entries();

    if (inspector_ && installer_) {
        inspector_->sync_with_pipeline(*installer_);
    }
}

void UiShell::update_filtered_entries() {
    std::string fmt_str;
    switch (format_filter_) {
        case FormatFilter::Pkg:    fmt_str = "pkg"; break;
        case FormatFilter::ExFat:  fmt_str = "exfat"; break;
        case FormatFilter::FfPkg:  fmt_str = "ffpkg"; break;
        case FormatFilter::Folder: fmt_str = "folder"; break;
        default:                   fmt_str.clear(); break;
    }

    std::vector<GameEntry> temp;
    auto visible=all_entries_;
    if(navigation_.active()==NavTab::Installed) {
        std::erase_if(visible,[](const GameEntry& entry){return !entry.installed;});
        for(const auto& entry:installed_entries_) {
            if(std::none_of(visible.begin(),visible.end(),[&](const auto& e){return e.title_id==entry.title_id;})) visible.push_back(entry);
        }
    }
    // Merge the same title found on several sources into one card listing every copy.
    // The cache keeps one entry per source; this merge is display-only.
    {
        std::vector<GameEntry> merged;
        std::map<std::string, size_t> by_key;
        for (auto& e : visible) {
            GameCopy copy{e.source_id, e.storage_type, e.source_root, e.base_game ? e.base_game->absolute_path : e.source_root};
            const std::string key = e.title_id.empty() ? std::string() :
                e.title_id + "|" + (e.platform ? std::string(to_string(*e.platform)) : "");
            const auto it = key.empty() ? by_key.end() : by_key.find(key);
            if (it == by_key.end()) {
                e.copies = {copy};
                if (!key.empty()) by_key[key] = merged.size();
                merged.push_back(std::move(e));
            } else {
                auto& kept = merged[it->second];
                const bool same = std::any_of(kept.copies.begin(), kept.copies.end(),
                    [&](const GameCopy& c) { return c.source_id == copy.source_id && c.path == copy.path; });
                if (!same) kept.copies.push_back(copy);
                if (!kept.installed && e.installed) kept.installed = true;
            }
        }
        visible = std::move(merged);
    }
    if (fmt_str.empty()) {
        temp = visible;
    } else {
        temp = IndexCache::filter_by_format(visible, fmt_str);
    }

    if (!search_query_.empty()) {
        filtered_entries_ = IndexCache::search(temp, search_query_);
    } else {
        filtered_entries_ = std::move(temp);
    }

    const auto tab = navigation_.active();
    if (tab != NavTab::Installed) {
        std::erase_if(filtered_entries_, [tab](const GameEntry& entry) {
            if (entry.platform) return *entry.platform != tab;
            if (tab == NavTab::PS5) return !entry.title_id.starts_with("PPSA");
            if (tab == NavTab::PS4) return !entry.title_id.starts_with("CUSA");
            if (tab == NavTab::PS3) {
                return !(entry.title_id.starts_with("BLUS") || entry.title_id.starts_with("BLES") ||
                         entry.title_id.starts_with("NPUB") || entry.title_id.starts_with("NPEB"));
            }
            return true; // PS1 and PS2 share serial prefixes; never guess a platform.
        });
    }

    std::stable_sort(filtered_entries_.begin(), filtered_entries_.end(), [this](const auto& a,const auto& b){
        const auto ka=library_key(a), kb=library_key(b);
        const bool fa=library_state_.favorites.count(ka)>0, fb=library_state_.favorites.count(kb)>0;
        if(fa!=fb) return fa; // favorites first
        auto at=[](const std::map<std::string,int64_t>& m,const std::string& k){auto it=m.find(k);return it==m.end()?int64_t(0):it->second;};
        switch(library_state_.sort) {
            case LibrarySort::Size: if(a.total_size_bytes!=b.total_size_bytes) return a.total_size_bytes>b.total_size_bytes; break;
            case LibrarySort::RecentlyAdded: {const auto x=at(library_state_.first_seen,ka),y=at(library_state_.first_seen,kb); if(x!=y) return x>y; break;}
            case LibrarySort::RecentlyPlayed: {const auto x=at(library_state_.last_played,ka),y=at(library_state_.last_played,kb); if(x!=y) return x>y; break;}
            default: break;
        }
        return a.title < b.title;
    });

    if (filtered_entries_.empty()) {
        focused_index_ = 0;
    } else if (focused_index_ >= filtered_entries_.size()) {
        focused_index_ = filtered_entries_.size() - 1;
    }
}

bool UiShell::handle_pad_input(const InputManager& input) {
    tick();
    if(handle_game_workflow(input)) return true;
    if(handle_source_workflow(input)) return true;
    if (dialog_model_.type != DialogType::None) {
        if (dialog_model_.type == DialogType::Settings || dialog_model_.type == DialogType::Sources ||
            dialog_model_.type == DialogType::MountingDiagnostics || dialog_model_.type == DialogType::StorageUsage ||
            dialog_model_.type == DialogType::LauncherSettings || dialog_model_.type == DialogType::LauncherOptions ||
            dialog_model_.type == DialogType::LauncherPicker || dialog_model_.type == DialogType::ErrorHistory ||
            dialog_model_.type == DialogType::InterruptedInstalls || dialog_model_.type == DialogType::GameDetails ||
            dialog_model_.type == DialogType::InstallCheck || dialog_model_.type == DialogType::BulkActions ||
            dialog_model_.type == DialogType::InstallQueue || dialog_model_.type == DialogType::QueueItem) {
            if (input.action_triggered(LogicalAction::Cancel)) { dismiss_dialog(); return true; }
            const auto count = static_cast<int>(dialog_model_.options.size());
            if (count > 0 && (input.action_triggered(LogicalAction::NavDown) || input.action_triggered(LogicalAction::NavUp))) {
                const int step = input.action_triggered(LogicalAction::NavDown) ? 1 : -1;
                dialog_model_.selected_option = (dialog_model_.selected_option + step + count) % count;
                return true;
            }
            if (count > 0 && input.action_triggered(LogicalAction::Confirm)) {
                const auto choice = dialog_model_.options[static_cast<size_t>(dialog_model_.selected_option)];
                if (handle_recovery_choice(choice)) return true;
                if (choice == "Emulators & Launchers" || choice == "Back to Launchers") show_launcher_settings();
                else if (dialog_model_.type == DialogType::LauncherSettings && choice.size() > 5 && choice[0] == 'P' && choice[1] == 'S' && choice[3] == ':')
                    show_launcher_options(choice[2] == '1' ? NavTab::PS1 : choice[2] == '2' ? NavTab::PS2 : NavTab::PS3);
                else if (dialog_model_.type == DialogType::LauncherSettings && choice == "Refresh Installed") {if(is_shadowmount_available()){refresh_installed_titles();show_launcher_settings();} else refresh_installed_titles();}
                else if (dialog_model_.type == DialogType::LauncherOptions && choice.starts_with("Use ")) {set_launcher(launcher_platform_, "");show_launcher_options(launcher_platform_);}
                else if (choice == "Choose Installed App") {launcher_search_.clear();show_launcher_picker();}
                else if (dialog_model_.type == DialogType::LauncherPicker) {
                    const auto row = static_cast<size_t>(dialog_model_.selected_option);
                    if (choice.starts_with("Search: ")) show_text_entry(launcher_search_, "launcher_search");
                    else if (choice == "Refresh Installed") {if(is_shadowmount_available()){refresh_installed_titles();show_launcher_picker();} else refresh_installed_titles();}
                    else if (choice == "Back to Launcher") show_launcher_options(launcher_platform_);
                    else if (row >= picker_header_rows_ && row - picker_header_rows_ < picker_ids_.size()) {
                        set_launcher(launcher_platform_, picker_ids_[row - picker_header_rows_]);
                        show_launcher_options(launcher_platform_);
                    }
                }
                else if (choice == "Enter App ID") {
                    const auto it = library_state_.launchers.find(to_string(launcher_platform_));
                    show_text_entry(it == library_state_.launchers.end() || it->second == "none" ? "" : it->second, "launcher_id");
                }
                else if (choice == "Disable Launching") {set_launcher(launcher_platform_, "none");show_launcher_options(launcher_platform_);}
                else if (choice == "Validate") {
                    const auto v = validate_launcher_for(launcher_platform_);
                    show_recovery_guidance(v.ok ? "Launcher ready" : "Launcher not ready", v.status, v.steps);
                }
                else if (choice == "Mounting" || choice == "Refresh Sources") show_sources();
                else if (choice == "Add Source") show_source_editor();
                else if (choice.starts_with("Source: ")) {
                    source_edit_id_=choice.substr(8);
                    dialog_model_={};dialog_model_.type=DialogType::SourceActions;
                    dialog_model_.title=source_edit_id_;dialog_model_.options={"Edit", "Enable / Disable", "Remove", "Back"};
                }
                else if (choice == "Search Library") show_text_entry(search_query_, "search");
                else if (choice == "Refresh Installed") {dismiss_dialog();refresh_installed_titles();}
                else if (choice.starts_with("Format: ")) {cycle_next_filter();show_settings();}
                else if (choice.starts_with("Sort: ")) {set_library_sort(next_library_sort(library_state_.sort));show_settings();}
                else if (choice == "Storage Usage" || choice == "Refresh Storage") show_storage_usage();
                else if (choice == "Back") show_settings();
                else if (choice == "Scan Diagnostics") show_mounting_diagnostics();
                else if (choice == "Back to Mounting") show_sources();
                else if (choice.starts_with("- ")) {
                    const auto row = static_cast<size_t>(dialog_model_.selected_option);
                    if (row >= diag_header_rows_ && row - diag_header_rows_ < diag_rows_.size()) {
                        const size_t index = diag_rows_[row - diag_header_rows_];
                        if (!last_scan_stats_.packages[index].reason.empty()) {
                            if (retry_package(index)) show_mounting_diagnostics();
                            else show_recovery_guidance("Retry unavailable", last_scan_stats_.packages[index].path,
                                                        "Check that the share is still configured and no other retry is running.");
                        }
                    }
                }
                else if (choice.starts_with("Search: ")) show_text_entry(diag_search_, "diag_search");
                else if (choice.starts_with("Show: ")) {
                    diag_filter_ = diag_filter_ == DiagFilter::All ? DiagFilter::Unreadable
                        : diag_filter_ == DiagFilter::Unreadable ? DiagFilter::FilenameFallback : DiagFilter::All;
                    show_mounting_diagnostics();
                }
                else if (choice.starts_with("Share: ")) {
                    std::vector<std::string> shares;
                    for (const auto& p : last_scan_stats_.packages)
                        if (std::find(shares.begin(), shares.end(), p.source_id) == shares.end()) shares.push_back(p.source_id);
                    auto it = std::find(shares.begin(), shares.end(), diag_share_);
                    diag_share_ = diag_share_.empty() ? (shares.empty() ? "" : shares.front())
                        : (it == shares.end() || it + 1 == shares.end() ? "" : *(it + 1));
                    show_mounting_diagnostics();
                }
                else if (choice.starts_with("Retry Unreadable Packages")) {
                    if (retry_unreadable_packages()) show_mounting_diagnostics();
                    else show_recovery_guidance("Nothing to retry", "No unreadable packages in the selected shares.", "");
                }
                else if (choice == "Save Diagnostics Report") {
                    std::string error;
                    if (save_scan_diagnostics(diagnostics_report_path_, &error))
                        show_recovery_guidance("Diagnostics saved", diagnostics_report_path_, "");
                    else show_recovery_guidance("Diagnostics not saved", error, "Check that the folder is writable.");
                }
                else if (choice == "Export Filtered Report") show_text_entry(diagnostics_export_path_, "diag_export_path");
                else if (choice == "Rescan Library" || choice == "Scan Library") { dismiss_dialog(); trigger_scan(true); }
                else if (choice == "Install History") show_install_history();
                else if (choice == "Diagnostics") {
                    show_startup_diagnostics(StartupDiagnosticsEngine(startup_config()).run_all_checks());
                } else if (dialog_model_.type != DialogType::StorageUsage) dismiss_dialog();
                return true;
            }
            return false;
        } else if (dialog_model_.type == DialogType::ShadowMountLock) {
            if (input.action_triggered(LogicalAction::Confirm)) {
                if (daemon_client_) {
                    daemon_client_->probe();
                }
                if (is_shadowmount_available()) {
                    dismiss_dialog();
                    library_mode_ = LibraryMode::LibraryBrowser;
                }
                return true;
            }
            if (input.action_triggered(LogicalAction::Cancel)) {
                dismiss_dialog();
                library_mode_ = LibraryMode::InstalledGames;
                return true;
            }
        } else if (dialog_model_.type == DialogType::RecoveryGuidance) {
            if (input.action_triggered(LogicalAction::Confirm) || input.action_triggered(LogicalAction::Cancel)) {
                dismiss_dialog();
                return true;
            }
        } else if (dialog_model_.type == DialogType::InstallHistory) {
            if (input.action_triggered(LogicalAction::Cancel)) {
                dismiss_dialog();
                return true;
            }
            if (input.action_triggered(LogicalAction::SwitchLayout) || input.action_triggered(LogicalAction::OpenSearch)) {
                open_export_file_picker(false);
                return true;
            }
            if (input.action_triggered(LogicalAction::NavDown)) {
                history_select_next();
                return true;
            }
            if (input.action_triggered(LogicalAction::NavUp)) {
                history_select_prev();
                return true;
            }
            if (input.action_triggered(LogicalAction::ToggleMode)) {
                cycle_history_outcome_filter();
                return true;
            }
            if (input.action_triggered(LogicalAction::Inspect)) {
                cycle_history_format_filter();
                return true;
            }
            if (input.action_triggered(LogicalAction::PrevFilter) || input.action_triggered(LogicalAction::NextFilter)) {
                cycle_history_sort();
                return true;
            }
            if (input.action_triggered(LogicalAction::Confirm)) {
                const auto selected = selected_history_item();
                if (selected && (selected->status == InstallStatus::Failed || selected->status == InstallStatus::Interrupted)) {
                    show_retry_confirmation(*selected);
                    return true;
                }
                return true;
            }
        } else if (dialog_model_.type == DialogType::RetryConfirmation) {
            if (input.action_triggered(LogicalAction::Cancel)) {
                dismiss_dialog();
                return true;
            }
            if (input.action_triggered(LogicalAction::NavLeft) || input.action_triggered(LogicalAction::NavRight)) {
                if (!dialog_model_.options.empty()) {
                    dialog_model_.selected_option = (dialog_model_.selected_option + 1) % dialog_model_.options.size();
                }
                return true;
            }
            if (input.action_triggered(LogicalAction::Confirm)) {
                if (!dialog_model_.options.empty() && dialog_model_.options[dialog_model_.selected_option] == "Confirm Retry") {
                    try {
                        json payload = json::parse(dialog_model_.extra_payload);
                        std::string task_id = payload.value("task_id", "");
                        std::string source_path = payload.value("source_path", "");
                        std::string target_dir = installer_ ? installer_->target_storage_dir() : "/user/app";

                        const auto fresh_preflight = run_package_preflight(source_path, target_dir);

                        PreflightCheckSummary prev_summary;
                        if (payload.contains("summary_snapshot")) {
                            const auto& sj = payload["summary_snapshot"];
                            prev_summary.all_passed = sj.value("all_passed", true);
                            if (sj.contains("readability")) {
                                prev_summary.readability.passed = sj["readability"].value("passed", false);
                                prev_summary.readability.details = sj["readability"].value("details", "");
                                prev_summary.readability.failure_reason = sj["readability"].value("failure_reason", "");
                                prev_summary.readability.remediation = sj["readability"].value("remediation", "");
                            }
                            if (sj.contains("metadata")) {
                                prev_summary.metadata.passed = sj["metadata"].value("passed", false);
                                prev_summary.metadata.details = sj["metadata"].value("details", "");
                                prev_summary.metadata.failure_reason = sj["metadata"].value("failure_reason", "");
                                prev_summary.metadata.remediation = sj["metadata"].value("remediation", "");
                            }
                            if (sj.contains("storage")) {
                                prev_summary.storage.passed = sj["storage"].value("passed", false);
                                prev_summary.storage.details = sj["storage"].value("details", "");
                                prev_summary.storage.failure_reason = sj["storage"].value("failure_reason", "");
                                prev_summary.storage.remediation = sj["storage"].value("remediation", "");
                            }
                        }

                        const auto diffs = diff_preflight_summaries(prev_summary, fresh_preflight.summary);
                        if (!diffs.empty()) {
                            dialog_model_.title = "Preflight Results Changed Before Retry";
                            dialog_model_.message = fresh_preflight.passed
                                ? "Preflight conditions changed since the initial summary. Review changes below:"
                                : "Preflight check failed on immediate recheck before retry:";
                            dialog_model_.secondary_text = format_preflight_diffs(diffs) + "\n\n" + fresh_preflight.summary.formatted_summary();

                            if (fresh_preflight.passed) {
                                dialog_model_.options = {"Proceed with Retry", "Cancel"};
                            } else {
                                dialog_model_.options = {"Dismiss"};
                            }
                            dialog_model_.selected_option = 0;

                            json fresh_sj;
                            fresh_sj["all_passed"] = fresh_preflight.summary.all_passed;
                            fresh_sj["readability"] = {{"passed", fresh_preflight.summary.readability.passed}, {"details", fresh_preflight.summary.readability.details}, {"failure_reason", fresh_preflight.summary.readability.failure_reason}, {"remediation", fresh_preflight.summary.readability.remediation}};
                            fresh_sj["metadata"] = {{"passed", fresh_preflight.summary.metadata.passed}, {"details", fresh_preflight.summary.metadata.details}, {"failure_reason", fresh_preflight.summary.metadata.failure_reason}, {"remediation", fresh_preflight.summary.metadata.remediation}};
                            fresh_sj["storage"] = {{"passed", fresh_preflight.summary.storage.passed}, {"details", fresh_preflight.summary.storage.details}, {"failure_reason", fresh_preflight.summary.storage.failure_reason}, {"remediation", fresh_preflight.summary.storage.remediation}};
                            payload["summary_snapshot"] = fresh_sj;
                            dialog_model_.extra_payload = payload.dump();
                            return true;
                        }

                        if (installer_ && !task_id.empty()) {
                            installer_->retry_task(task_id);
                            update_history_view();
                            if (is_inspector_open()) {
                                inspector_->sync_with_pipeline(*installer_);
                            }
                        }
                    } catch (...) {}
                    dismiss_dialog();
                    return true;
                } else if (!dialog_model_.options.empty() && dialog_model_.options[dialog_model_.selected_option] == "Proceed with Retry") {
                    try {
                        json payload = json::parse(dialog_model_.extra_payload);
                        std::string task_id = payload.value("task_id", "");
                        if (installer_ && !task_id.empty()) {
                            installer_->retry_task(task_id);
                            update_history_view();
                            if (is_inspector_open()) {
                                inspector_->sync_with_pipeline(*installer_);
                            }
                        }
                    } catch (...) {}
                    dismiss_dialog();
                    return true;
                }
                dismiss_dialog();
                return true;
            }
        } else if (dialog_model_.type == DialogType::PreflightSummary) {
            if (input.action_triggered(LogicalAction::Cancel)) {
                dismiss_dialog();
                return true;
            }
            if (input.action_triggered(LogicalAction::NavLeft) || input.action_triggered(LogicalAction::NavRight)) {
                if (!dialog_model_.options.empty()) {
                    dialog_model_.selected_option = (dialog_model_.selected_option + 1) % dialog_model_.options.size();
                }
                return true;
            }
            if (input.action_triggered(LogicalAction::Confirm)) {
                if (!dialog_model_.options.empty() && dialog_model_.options[dialog_model_.selected_option] == "Proceed with Install") {
                    try {
                        json payload = json::parse(dialog_model_.extra_payload);
                        std::string absolute_path = payload.value("absolute_path", "");
                        std::string target_dir = installer_ ? installer_->target_storage_dir() : "/user/app";

                        const auto fresh_preflight = run_package_preflight(absolute_path, target_dir);

                        PreflightCheckSummary prev_summary;
                        if (payload.contains("summary_snapshot")) {
                            const auto& sj = payload["summary_snapshot"];
                            prev_summary.all_passed = sj.value("all_passed", true);
                            if (sj.contains("readability")) {
                                prev_summary.readability.passed = sj["readability"].value("passed", false);
                                prev_summary.readability.details = sj["readability"].value("details", "");
                                prev_summary.readability.failure_reason = sj["readability"].value("failure_reason", "");
                                prev_summary.readability.remediation = sj["readability"].value("remediation", "");
                            }
                            if (sj.contains("metadata")) {
                                prev_summary.metadata.passed = sj["metadata"].value("passed", false);
                                prev_summary.metadata.details = sj["metadata"].value("details", "");
                                prev_summary.metadata.failure_reason = sj["metadata"].value("failure_reason", "");
                                prev_summary.metadata.remediation = sj["metadata"].value("remediation", "");
                            }
                            if (sj.contains("storage")) {
                                prev_summary.storage.passed = sj["storage"].value("passed", false);
                                prev_summary.storage.details = sj["storage"].value("details", "");
                                prev_summary.storage.failure_reason = sj["storage"].value("failure_reason", "");
                                prev_summary.storage.remediation = sj["storage"].value("remediation", "");
                            }
                        }

                        const auto diffs = diff_preflight_summaries(prev_summary, fresh_preflight.summary);
                        if (!diffs.empty()) {
                            dialog_model_.title = "Preflight Results Changed Before Dispatch";
                            dialog_model_.message = fresh_preflight.passed
                                ? "Preflight conditions changed since the initial summary. Review changes below:"
                                : "Preflight check failed on immediate recheck before dispatch:";
                            dialog_model_.secondary_text = format_preflight_diffs(diffs) + "\n\n" + fresh_preflight.summary.formatted_summary();

                            if (fresh_preflight.passed) {
                                dialog_model_.options = {"Proceed Anyway", "Cancel"};
                            } else {
                                dialog_model_.options = {"Dismiss"};
                            }
                            dialog_model_.selected_option = 0;

                            payload["passed"] = fresh_preflight.passed;
                            json fresh_sj;
                            fresh_sj["all_passed"] = fresh_preflight.summary.all_passed;
                            fresh_sj["readability"] = {{"passed", fresh_preflight.summary.readability.passed}, {"details", fresh_preflight.summary.readability.details}, {"failure_reason", fresh_preflight.summary.readability.failure_reason}, {"remediation", fresh_preflight.summary.readability.remediation}};
                            fresh_sj["metadata"] = {{"passed", fresh_preflight.summary.metadata.passed}, {"details", fresh_preflight.summary.metadata.details}, {"failure_reason", fresh_preflight.summary.metadata.failure_reason}, {"remediation", fresh_preflight.summary.metadata.remediation}};
                            fresh_sj["storage"] = {{"passed", fresh_preflight.summary.storage.passed}, {"details", fresh_preflight.summary.storage.details}, {"failure_reason", fresh_preflight.summary.storage.failure_reason}, {"remediation", fresh_preflight.summary.storage.remediation}};
                            payload["summary_snapshot"] = fresh_sj;
                            dialog_model_.extra_payload = payload.dump();
                            return true;
                        }

                        GameItem item;
                        item.filename = payload.value("filename", "");
                        item.absolute_path = payload.value("absolute_path", "");
                        item.format = payload.value("format", "");
                        item.display_name = payload.value("display_name", "");
                        item.size_bytes = payload.value("size_bytes", uint64_t{0});
                        item.size_known = item.size_bytes > 0;
                        item.formatted_size = format_file_size(item.size_bytes);
                        item.version = payload.value("version", "");
                        item.kind = parse_item_kind(payload.value("kind", "Unknown"));
                        std::string title_id = payload.value("title_id", "");
                        if (installer_) {
                            installer_->queue_item(item, title_id, fresh_preflight.summary);
                            if (is_inspector_open()) {
                                inspector_->sync_with_pipeline(*installer_);
                            }
                        }
                    } catch (...) {}
                    dismiss_dialog();
                    return true;
                } else if (!dialog_model_.options.empty() && dialog_model_.options[dialog_model_.selected_option] == "Proceed Anyway") {
                    try {
                        json payload = json::parse(dialog_model_.extra_payload);
                        GameItem item;
                        item.filename = payload.value("filename", "");
                        item.absolute_path = payload.value("absolute_path", "");
                        item.format = payload.value("format", "");
                        item.display_name = payload.value("display_name", "");
                        item.size_bytes = payload.value("size_bytes", uint64_t{0});
                        item.size_known = item.size_bytes > 0;
                        item.formatted_size = format_file_size(item.size_bytes);
                        item.version = payload.value("version", "");
                        item.kind = parse_item_kind(payload.value("kind", "Unknown"));
                        std::string title_id = payload.value("title_id", "");
                        if (installer_) {
                            std::string target_dir = installer_->target_storage_dir();
                            const auto immediate_p = run_package_preflight(item.absolute_path, target_dir);
                            installer_->queue_item(item, title_id, immediate_p.summary);
                            if (is_inspector_open()) {
                                inspector_->sync_with_pipeline(*installer_);
                            }
                        }
                    } catch (...) {}
                    dismiss_dialog();
                    return true;
                }
                dismiss_dialog();
                return true;
            }
        } else if (dialog_model_.type == DialogType::StartupDiagnostics) {
            if (input.action_triggered(LogicalAction::Cancel)) {
                dismiss_dialog();
                return true;
            }
            if (input.action_triggered(LogicalAction::NavUp)) {
                if (!dialog_model_.options.empty()) {
                    if (dialog_model_.selected_option > 0) {
                        dialog_model_.selected_option--;
                    } else {
                        dialog_model_.selected_option = static_cast<int>(dialog_model_.options.size() - 1);
                    }
                }
                return true;
            }
            if (input.action_triggered(LogicalAction::NavDown)) {
                if (!dialog_model_.options.empty()) {
                    dialog_model_.selected_option = (dialog_model_.selected_option + 1) % dialog_model_.options.size();
                }
                return true;
            }
            if (input.action_triggered(LogicalAction::ToggleMode) || input.action_triggered(LogicalAction::Inspect)) {
                if (dialog_model_.message == startup_diagnostics_report_.format_summary()) {
                    dialog_model_.message = startup_diagnostics_report_.format_detailed_report();
                } else {
                    dialog_model_.message = startup_diagnostics_report_.format_summary();
                }
                return true;
            }
            if (input.action_triggered(LogicalAction::Confirm)) {
                if (dialog_model_.selected_option == 0) {
                    dismiss_dialog();
                    return true;
                } else if (dialog_model_.selected_option == 1) {
                    if (dialog_model_.message == startup_diagnostics_report_.format_summary()) {
                        dialog_model_.message = startup_diagnostics_report_.format_detailed_report();
                    } else {
                        dialog_model_.message = startup_diagnostics_report_.format_summary();
                    }
                    return true;
                } else if (dialog_model_.selected_option == 2) {
                    show_startup_diagnostics(StartupDiagnosticsEngine(startup_config()).run_all_checks());
                    return true;
                } else if (dialog_model_.selected_option > 2) {
                    retry_startup_check(static_cast<size_t>(dialog_model_.selected_option - 3));
                    return true;
                }
                return true;
            }
        } else if (dialog_model_.type == DialogType::ExportFilePicker) {
            if (input.action_triggered(LogicalAction::Cancel)) {
                show_install_history();
                return true;
            }
            if (input.action_triggered(LogicalAction::NavUp)) {
                if (!dialog_model_.options.empty()) {
                    if (dialog_model_.selected_option > 0) {
                        dialog_model_.selected_option--;
                    } else {
                        dialog_model_.selected_option = static_cast<int>(dialog_model_.options.size() - 1);
                    }
                }
                return true;
            }
            if (input.action_triggered(LogicalAction::NavDown)) {
                if (!dialog_model_.options.empty()) {
                    dialog_model_.selected_option = (dialog_model_.selected_option + 1) % dialog_model_.options.size();
                }
                return true;
            }
            if (input.action_triggered(LogicalAction::Inspect)) {
                toggle_export_format();
                return true;
            }
            if (input.action_triggered(LogicalAction::ToggleMode)) {
                cycle_export_directory();
                return true;
            }
            if (input.action_triggered(LogicalAction::PrevFilter) || input.action_triggered(LogicalAction::NextFilter)) {
                cycle_export_filename_preset();
                return true;
            }
            if (input.action_triggered(LogicalAction::Confirm)) {
                if (!dialog_model_.options.empty()) {
                    const std::string& opt = dialog_model_.options[dialog_model_.selected_option];
                    if (opt == "Export Now") {
                        execute_export();
                        return true;
                    } else if (opt.rfind("Format:", 0) == 0) {
                        toggle_export_format();
                        return true;
                    } else if (opt.rfind("Directory:", 0) == 0) {
                        cycle_export_directory();
                        return true;
                    } else if (opt.rfind("Filename:", 0) == 0) {
                        cycle_export_filename_preset();
                        return true;
                    } else if (opt == "Cancel") {
                        show_install_history();
                        return true;
                    }
                }
                return true;
            }
        } else {
            if (input.action_triggered(LogicalAction::Cancel)) {
                dismiss_dialog();
                return true;
            }
        }
        return true;
    }

    if (is_inspector_open()) {
        if (input.action_triggered(LogicalAction::Cancel)) {
            close_inspector();
            return true;
        }

        if (input.action_triggered(LogicalAction::Inspect)) {
            const auto item = inspector_->selected_item();
            if (item.has_value() && (item->install_status == InstallStatus::Failed ||
                                     item->install_status == InstallStatus::Interrupted)) {
                show_recovery_guidance("Troubleshooting: " + item->filename,
                                       item->error_message,
                                       item->recovery_guidance);
                return true;
            }
        }

        if (preflight_summary_before_dispatch_ && input.action_triggered(LogicalAction::Confirm)) {
            if (inspector_->focus_section() == InspectorFocusSection::Actions) {
                const auto labels = inspector_->available_action_labels();
                if (inspector_->selected_action_button() < labels.size()) {
                    const std::string& label = labels[inspector_->selected_action_button()];
                    if (label == "Retry Install") {
                        const auto selected = inspector_->selected_item();
                        if (selected && selected->can_retry && installer_) {
                            const auto task = installer_->get_task_by_path(selected->absolute_path);
                            if (task) {
                                show_retry_confirmation(*task);
                                return true;
                            }
                        }
                    } else if (label == "Install Selected") {
                        const auto selected = inspector_->selected_item();
                        if (selected && !selected->has_active_task) {
                            GameItem item;
                            item.kind = selected->kind;
                            item.filename = selected->filename;
                            item.absolute_path = selected->absolute_path;
                            item.format = selected->extension.empty() ? "" : (selected->extension[0] == '.' ? selected->extension.substr(1) : selected->extension);
                            item.size_bytes = selected->raw_size_bytes;
                            item.size_known = selected->formatted_size != "Unknown";
                            item.formatted_size = selected->formatted_size;
                            item.version = selected->version;
                            item.display_name = selected->display_name;
                            show_preflight_summary(item, inspector_->title_id());
                            return true;
                        }
                    }
                }
            }
        }

        bool handled = inspector_->handle_pad_input(input, *installer_);
        if (installer_) inspector_->sync_with_pipeline(*installer_);
        return handled;
    }

    if (input.action_triggered(LogicalAction::ToggleMode)) {
        toggle_library_mode();
        return true;
    }

    if (input.action_triggered(LogicalAction::OpenSettings)) { show_settings(); return true; }

    if (input.action_triggered(LogicalAction::SwitchLayout)) {
        toggle_view_layout();
        return true;
    }

    if (input.action_triggered(LogicalAction::PrevFilter)) {
        navigation_.prev();
        select_tab(navigation_.active());
        return true;
    }
    if (input.action_triggered(LogicalAction::NextFilter)) {
        navigation_.next();
        select_tab(navigation_.active());
        return true;
    }

    if (input.action_triggered(LogicalAction::OpenSearch)) {
        if (!search_query_.empty()) {
            set_search_query("");
        } else {
            show_text_entry("", "search");
        }
        return true;
    }

    if (filtered_entries_.empty()) {
        return false;
    }

    if (view_layout_ == ViewLayout::Coverflow) {
        if (input.action_triggered(LogicalAction::NavLeft)) {
            if (focused_index_ > 0) {
                focused_index_--;
            }
            return true;
        }
        if (input.action_triggered(LogicalAction::NavRight)) {
            if (focused_index_ + 1 < filtered_entries_.size()) {
                focused_index_++;
            }
            return true;
        }
    } else if (view_layout_ == ViewLayout::BlockGrid) {
        int dx = 0;
        int dy = 0;
        if (input.action_triggered(LogicalAction::NavLeft)) dx = -1;
        else if (input.action_triggered(LogicalAction::NavRight)) dx = 1;
        else if (input.action_triggered(LogicalAction::NavUp)) dy = -1;
        else if (input.action_triggered(LogicalAction::NavDown)) dy = 1;

        if (dx != 0 || dy != 0) {
            focused_index_ = ViewLayoutEngine::move_grid_focus(focused_index_, dx, dy, filtered_entries_.size(), 5);
            return true;
        }
    }

    if (input.action_triggered(LogicalAction::Inspect)) {
        if (auto entry = focused_entry()) {
            open_inspector(*entry);
            return true;
        }
    }

    if (input.action_triggered(LogicalAction::Confirm)) {
        if (auto entry = focused_entry()) {
            show_game_actions();
            return true;
        }
    }

    return false;
}

std::vector<CardVisual> UiShell::get_current_view_cards() const {
    auto cards = view_layout_ == ViewLayout::Coverflow
        ? layout_engine_.compute_coverflow(filtered_entries_, focused_index_)
        : layout_engine_.compute_block_grid(filtered_entries_, focused_index_, 5);
    for (auto& card : cards)
        if (card.index < filtered_entries_.size()) card.favorite = is_favorite(filtered_entries_[card.index]);
    return cards;
}

std::vector<ButtonPrompt> UiShell::get_contextual_prompts() const {
    if (is_inspector_open()) {
        auto prompts = inspector_->get_contextual_prompts();
        const auto item = inspector_->selected_item();
        if (item.has_value() && (item->install_status == InstallStatus::Failed ||
                                 item->install_status == InstallStatus::Interrupted)) {
            prompts.push_back({ButtonGlyph::Square, "Troubleshoot / Recovery", true, false});
        }
        return prompts;
    }

    std::vector<ButtonPrompt> prompts;

    if (dialog_model_.type == DialogType::ShadowMountLock) {
        prompts.push_back({ButtonGlyph::Cross, "Retry Connection", true, true});
        prompts.push_back({ButtonGlyph::Circle, "Installed Games", true, false});
        return prompts;
    }

    if (dialog_model_.type == DialogType::RecoveryGuidance) {
        prompts.push_back({ButtonGlyph::Cross, "Dismiss Guidance", true, true});
        return prompts;
    }

    if (dialog_model_.type == DialogType::ExportFilePicker) {
        prompts.push_back({ButtonGlyph::Cross, "Select / Action", true, true});
        prompts.push_back({ButtonGlyph::Square, "Toggle Format", true, false});
        prompts.push_back({ButtonGlyph::Triangle, "Cycle Dir", true, false});
        prompts.push_back({ButtonGlyph::L1, "Cycle Name", true, false});
        prompts.push_back({ButtonGlyph::Circle, "Back", true, false});
        return prompts;
    }

    if (dialog_model_.type == DialogType::InstallHistory) {
        prompts.push_back({ButtonGlyph::DPadUpDown, "Select Job", true, false});
        prompts.push_back({ButtonGlyph::Cross, "Retry Selected", true, true});
        prompts.push_back({ButtonGlyph::Triangle, "Filter Status", true, false});
        prompts.push_back({ButtonGlyph::Square, "Filter Format", true, false});
        prompts.push_back({ButtonGlyph::L1, "Sort (L1/R1)", true, false});
        prompts.push_back({ButtonGlyph::Touchpad, "Export File...", true, false});
        prompts.push_back({ButtonGlyph::Circle, "Close History", true, false});
        return prompts;
    }

    if (dialog_model_.type == DialogType::RetryConfirmation) {
        if (!dialog_model_.options.empty() && dialog_model_.options[0] == "Confirm Retry") {
            prompts.push_back({ButtonGlyph::Cross, "Confirm Retry", true, true});
            prompts.push_back({ButtonGlyph::Circle, "Cancel", true, false});
        } else {
            prompts.push_back({ButtonGlyph::Cross, "Dismiss", true, true});
            prompts.push_back({ButtonGlyph::Circle, "Close", true, false});
        }
        return prompts;
    }

    if (dialog_model_.type == DialogType::PreflightSummary) {
        if (!dialog_model_.options.empty() && dialog_model_.options[0] == "Proceed with Install") {
            prompts.push_back({ButtonGlyph::Cross, "Proceed Install", true, true});
            prompts.push_back({ButtonGlyph::Circle, "Cancel", true, false});
        } else {
            prompts.push_back({ButtonGlyph::Cross, "Dismiss", true, true});
            prompts.push_back({ButtonGlyph::Circle, "Close", true, false});
        }
        return prompts;
    }

    if (dialog_model_.type == DialogType::StartupDiagnostics) {
        prompts.push_back({ButtonGlyph::Cross, "Select Option", true, true});
        prompts.push_back({ButtonGlyph::Circle, "Dismiss", true, false});
        prompts.push_back({ButtonGlyph::Triangle, "Details", true, false});
        return prompts;
    }

    if (dialog_model_.type != DialogType::None) {
        prompts.push_back({ButtonGlyph::Cross, "Select", true, true});
        prompts.push_back({ButtonGlyph::Circle, "Close", true, false});
        return prompts;
    }

    std::string mode_label = (library_mode_ == LibraryMode::InstalledGames)
                             ? "Library Browser" : "Installed Games";
    prompts.push_back({ButtonGlyph::Triangle, mode_label, true, false});

    std::string layout_label = (view_layout_ == ViewLayout::Coverflow)
                               ? "Grid View" : "Coverflow";
    prompts.push_back({ButtonGlyph::Touchpad, layout_label, true, false});

    prompts.push_back({ButtonGlyph::L1, "Previous Tab", true, false});
    prompts.push_back({ButtonGlyph::R1, "Next Tab", true, false});
    prompts.push_back({ButtonGlyph::Options, "Settings", true, false});

    if (!filtered_entries_.empty()) {
        prompts.push_back({ButtonGlyph::Square, "Inspect Game", true, false});
        prompts.push_back({ButtonGlyph::Cross, "Game Actions", true, true});
    }

    prompts.push_back({ButtonGlyph::L3, search_query_.empty() ? "Search" : "Clear Search", true, false});

    return prompts;
}

void UiShell::trigger_scan(bool force_rescan,
                           std::function<void(bool success)> on_complete) {
    (void)force_rescan;
    if (scanner_.is_scanning()) {if(on_complete) on_complete(false);return;}

    ScanOptions options;
    options.root_path = current_scan_path_;
    options.recursive = true;
    options.cover_cache_dir = cover_cache_dir_;
    sources_.set_daemon_unlocked(is_shadowmount_available());
    sources_.refresh_all();
    auto specs = sources_.active_scan_specs();
    const auto only_sources = std::exchange(retry_sources_, {});
    if (!only_sources.empty()) std::erase_if(specs, [&](const ScanRootSpec& s) { return only_sources.count(s.source_id) == 0; });
    if (sources_.list().empty()) {
        show_startup_diagnostics({});
        if (on_complete) on_complete(false);
        return;
    }
    if (!sources_.list().empty()) {
        auto identify=[&](const ScanRootSpec& spec){
            const auto key=is_remote_root_path(spec.path) ? spec.path : std::filesystem::path(spec.path).lexically_normal().string();
            return options.root_identities.emplace(key,ScanOptions::RootIdentity{spec.source_id,spec.storage_type,spec.platform}).second;
        };
        bool unique=true;
        for(const auto& spec:specs)if(!identify(spec))unique=false;
        if(!unique){show_recovery_guidance("Conflicting library paths","Two devices share the same scan path.","Choose distinct platform paths before scanning.");if(on_complete)on_complete(false);return;}
        for(const auto& [path,identity]:options.root_identities) {
            (void)identity;
            for(const auto& [other,other_identity]:options.root_identities) {
                (void)other_identity;if(path==other)continue;
                bool overlaps=false;
                if(is_remote_root_path(path) && is_remote_root_path(other)) {
                    overlaps=path.size()>other.size() && path.starts_with(other) && (other.back()=='/' || path[other.size()]=='/');
                } else if(!is_remote_root_path(path) && !is_remote_root_path(other)) {
                    const auto relative=std::filesystem::path(path).lexically_relative(other);
                    overlaps=!relative.empty() && *relative.begin()!="..";
                }
                if(overlaps) {
                    show_recovery_guidance("Overlapping library paths",path+" overlaps "+other,"Choose separate folders for each platform.");if(on_complete)on_complete(false);return;
                }
            }
            if(!is_remote_root_path(path)) {
                std::error_code ec;
                if(!std::filesystem::is_directory(path,ec)) {
                    // A configured library folder is missing: surface the
                    // startup diagnostics report instead of a generic notice.
                    StartupDiagnosticsReport report;
                    DiagnosticItem item;
                    item.subsystem = DiagnosticSubsystem::Mount;
                    item.name = "Library folder";
                    item.severity = DiagnosticSeverity::Error;
                    item.summary = path;
                    item.details = "The configured library folder could not be found.";
                    item.recovery_guidance = "Create the folder or correct its platform path in Mounting.";
                    item.error_code = 0;
                     report.items.push_back(item);
                    show_startup_diagnostics(report);
                    if (on_complete) on_complete(false);
                    return;
                }
            }
        }
        if (specs.empty()) {
            if (active_dialog() == DialogType::None) show_sources();
            if (on_complete) on_complete(false);
            return;
        }
        for(const auto& spec:specs) {
            if(spec.address.kind==FileTransportKind::Local)options.roots.push_back(spec.path);
            else options.remote_roots.push_back(spec);
        }
    }

    auto roots=options.roots;
    if(roots.empty()) roots.push_back(options.root_path);
    scan_progress_ = ScanProgress{"", 0, true};
    auto progress = [this](const std::string& item, size_t found, bool done) {
        std::lock_guard lock(event_mutex_);
        pending_progress_ = ScanProgress{item, found, !done};
    };
    scanner_.start_async_scan(options, progress, [this, on_complete, roots, only_sources](std::vector<GameEntry> entries, ScanStatistics stats) {
        std::lock_guard lock(event_mutex_);
        pending_progress_ = ScanProgress{"", entries.size(), false};
        pending_scan_ = ScanResult{std::move(entries), on_complete, roots, std::move(stats), only_sources};
    });
}

} // namespace nexodion
