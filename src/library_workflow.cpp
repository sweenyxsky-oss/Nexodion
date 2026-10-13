// Library browsing preferences, favorites, recently played, storage usage and
// launch status for UiShell. Background results are applied in tick().
#include "nexodion/ui_shell.hpp"

#include <chrono>
#include <algorithm>
#include <cctype>

namespace nexodion {
namespace {
int64_t now_unix() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
}

void UiShell::save_library_state() {
    if (ui_state_path_.empty()) return;
    library_state_.view_layout = view_layout_ == ViewLayout::Coverflow ? "coverflow" : "grid";
    library_state_.format_filter = to_string(format_filter_);
    std::string error;
    if (!library_state_.save(ui_state_path_, &error))
        show_recovery_guidance("Preferences not saved", error, "Free storage on the console; your choices stay active for this session.");
}

void UiShell::set_ui_state_path(std::string path) {
    ui_state_path_ = std::move(path);
    if (const auto note = recover_json_file(ui_state_path_); !note.empty())
        record_event("warning", "settings", "Library preferences were damaged", note, "Check sort, favorites and emulator choices.");
    LibraryState loaded;
    if (loaded.load(ui_state_path_)) {
        library_state_ = std::move(loaded);
        view_layout_ = library_state_.view_layout == "grid" ? ViewLayout::BlockGrid : ViewLayout::Coverflow;
        for (int i = 0; i < 8 && std::string(to_string(format_filter_)) != library_state_.format_filter
                        && !library_state_.format_filter.empty(); ++i)
            format_filter_ = next_filter(format_filter_);
    }
    update_filtered_entries();
}

void UiShell::set_library_sort(LibrarySort sort) {
    library_state_.sort = sort;
    update_filtered_entries();
    save_library_state();
}

bool UiShell::is_favorite(const GameEntry& entry) const {
    return library_state_.favorites.count(library_key(entry)) > 0;
}

void UiShell::toggle_favorite(const GameEntry& entry) {
    const auto key = library_key(entry);
    if (!library_state_.favorites.erase(key)) library_state_.favorites.insert(key);
    update_filtered_entries();
    save_library_state();
}

void UiShell::record_played(const GameEntry& entry, int64_t unix_seconds) {
    library_state_.last_played[library_key(entry)] = unix_seconds ? unix_seconds : now_unix();
    update_filtered_entries();
    save_library_state();
}

std::optional<LaunchCheck> UiShell::focused_launch_check() const {
    const auto entry = focused_entry();
    if (!entry) return std::nullopt;
    const bool runtime = launch_runtime_override_.value_or(native_runtime_available());
    return evaluate_launch(*entry, [this](const std::string& id) { return is_title_installed(id); }, runtime, library_state_.launchers);
}

std::vector<UiShell::StorageRow> UiShell::query_storage_usage() {
    std::vector<StorageRow> rows;
    for (const auto& spec : sources_.active_scan_specs()) {
        StorageRow row{spec.source_id, spec.storage_type, spec.path, {}};
        if (spec.address.kind == FileTransportKind::Local) {
            row.usage = FileVfs().storage_usage(spec.path);
        } else {
            std::string error;
            auto transport = make_file_transport(spec.address, error);
            if (!transport) { row.usage.error = "Share unreachable: " + error; }
            else row.usage = transport->storage_usage(spec.base_path);
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

void UiShell::render_storage_usage() {
    dialog_model_ = {};
    dialog_model_.type = DialogType::StorageUsage;
    dialog_model_.title = "Storage Usage";
    dialog_model_.message = storage_busy_ ? "Reading storage space..." :
        storage_rows_.empty() ? "No enabled sources" : std::to_string(storage_rows_.size()) + " paths";
    for (const auto& r : storage_rows_) {
        std::string line = r.source_id + " (" + r.storage_type + ") " + r.path + ": ";
        if (r.usage.ok) {
            const uint64_t used = r.usage.total_bytes > r.usage.free_bytes ? r.usage.total_bytes - r.usage.free_bytes : 0;
            const int pct = r.usage.total_bytes ? int(used * 100 / r.usage.total_bytes) : 0;
            line += format_file_size(r.usage.free_bytes) + " free of " + format_file_size(r.usage.total_bytes) +
                    " (" + std::to_string(pct) + "% used)";
        } else line += "unavailable - " + r.usage.error;
        dialog_model_.options.push_back(line);
    }
    dialog_model_.options.push_back("Refresh Storage");
    dialog_model_.options.push_back("Back to Mounting");
}

void UiShell::show_storage_usage() {
    if (storage_busy_) { render_storage_usage(); return; }
    if (storage_worker_.joinable()) storage_worker_.join();
    storage_busy_ = true;
    render_storage_usage();
    storage_worker_ = std::thread([this] {
        auto rows = query_storage_usage();
        std::lock_guard lock(event_mutex_);
        pending_storage_ = std::move(rows);
    });
}

void UiShell::show_launcher_settings() {
    const int selected = dialog_model_.type == DialogType::LauncherSettings ? dialog_model_.selected_option : 0;
    dialog_model_ = {};
    dialog_model_.type = DialogType::LauncherSettings;
    dialog_model_.title = "Emulators & Launchers";
    dialog_model_.message = "Choose and validate the app that opens PS1, PS2 and PS3 games";
    auto installed = [this](const std::string& id) { return is_title_installed(id); };
    for (auto p : {NavTab::PS1, NavTab::PS2, NavTab::PS3})
        dialog_model_.options.push_back(std::string(to_string(p)) + ": " + validate_launcher(p, library_state_.launchers, installed).status);
    dialog_model_.options.push_back("Refresh Installed");
    dialog_model_.options.push_back("Back");
    dialog_model_.selected_option = std::min<int>(selected, int(dialog_model_.options.size()) - 1);
}

void UiShell::show_launcher_options(NavTab platform) {
    launcher_platform_ = platform;
    const auto r = resolve_launcher(platform, library_state_.launchers);
    const auto* builtin = emulator_for(platform);
    dialog_model_ = {};
    dialog_model_.type = DialogType::LauncherOptions;
    dialog_model_.title = std::string(to_string(platform)) + " launcher";
    dialog_model_.message = r.disabled ? "Disabled" : r.name + (r.title_id.empty() ? " (link pending)" : " (" + r.title_id + ")");
    dialog_model_.secondary_text = r.repository;
    if (builtin) dialog_model_.options.push_back("Use " + builtin->name + (builtin->title_id.empty() ? " (link pending)" : ""));
    dialog_model_.options.insert(dialog_model_.options.end(), {"Choose Installed App", "Enter App ID", "Disable Launching", "Validate", "Back to Launchers"});
}

bool UiShell::set_launcher(NavTab platform, const std::string& choice, std::string* error) {
    std::string value = choice;
    for (auto& ch : value) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    if (value == "NONE") value = "none";
    if (!value.empty() && value != "none" && !valid_native_title(value)) {
        if (error) *error = "App ID must look like PPSA12345 or CUSA12345";
        return false;
    }
    if (value.empty()) library_state_.launchers.erase(to_string(platform));
    else library_state_.launchers[to_string(platform)] = value;
    save_library_state();
    return true;
}

std::vector<UiShell::InstalledApp> UiShell::installed_apps(const std::string& query) const {
    auto lower = [](std::string s) { for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; };
    const auto needle = lower(query);
    std::vector<InstalledApp> out;
    auto add = [&](const GameEntry& e) {
        if (!e.installed || !valid_native_title(e.title_id)) return;
        if (std::any_of(out.begin(), out.end(), [&](const InstalledApp& a) { return a.title_id == e.title_id; })) return;
        const std::string name = e.title.empty() ? e.title_id : e.title;
        if (!needle.empty() && lower(name).find(needle) == std::string::npos && lower(e.title_id).find(needle) == std::string::npos) return;
        out.push_back({name, e.title_id});
    };
    for (const auto& e : installed_entries_) add(e);
    for (const auto& e : all_entries_) add(e);
    std::sort(out.begin(), out.end(), [&](const auto& a, const auto& b) { return lower(a.title) < lower(b.title); });
    return out;
}

void UiShell::show_launcher_picker() {
    const auto apps = installed_apps(launcher_search_);
    dialog_model_ = {};
    dialog_model_.type = DialogType::LauncherPicker;
    dialog_model_.title = std::string("Choose ") + to_string(launcher_platform_) + " launcher";
    dialog_model_.message = std::to_string(apps.size()) + " installed apps" + (launcher_search_.empty() ? "" : " matching \"" + launcher_search_ + "\"");
    if (apps.empty()) dialog_model_.secondary_text = "No installed apps found. Choose Refresh Installed, or clear the search.";
    dialog_model_.options = {"Search: " + launcher_search_, "Refresh Installed"};
    picker_header_rows_ = dialog_model_.options.size();
    picker_ids_.clear();
    for (const auto& a : apps) { dialog_model_.options.push_back("> " + a.title + " (" + a.title_id + ")"); picker_ids_.push_back(a.title_id); }
    dialog_model_.options.push_back("Back to Launcher");
}

LauncherValidation UiShell::validate_launcher_for(NavTab platform) const {
    return validate_launcher(platform, library_state_.launchers, [this](const std::string& id) { return is_title_installed(id); });
}

} // namespace nexodion
