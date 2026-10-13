// Error history, interrupted-install review, game details, pre-install checks,
// bulk actions, settings backup and background refresh for UiShell.
// Everything here runs on the UI thread; slow work stays in existing workers.
#include "nexodion/ui_shell.hpp"
#include "nexodion/native_services.hpp"
#include "nexodion/library_state.hpp"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace nexodion {

void UiShell::record_event(const std::string& severity, const std::string& area, const std::string& what,
                           const std::string& why, const std::string& fix) {
    EventRecord e;
    e.severity = severity;
    e.area = area;
    e.what = what;
    e.why = why;
    e.fix = fix;
    events_.record(std::move(e));
}

void UiShell::show_error_history() {
    dialog_model_ = {};
    dialog_model_.type = DialogType::ErrorHistory;
    dialog_model_.title = "Error History";
    const auto list = events_.recent(50);
    dialog_model_.message = list.empty() ? "Nothing has gone wrong this session." :
        std::to_string(events_.error_count()) + " error(s) recorded. Choose one to see why it happened and what to do.";
    for (const auto& e : list) dialog_model_.options.push_back(e.line());
    dialog_model_.options.insert(dialog_model_.options.end(), {"Export Logs", "Clear History", "Back"});
}

bool UiShell::show_interrupted_installs() {
    if (!installer_) return false;
    const auto tasks = installer_->interrupted_tasks();
    if (tasks.empty()) return false;
    interrupted_ids_.clear();
    dialog_model_ = {};
    dialog_model_.type = DialogType::InterruptedInstalls;
    dialog_model_.title = "Interrupted Installs";
    dialog_model_.message = "Nexodion closed while these were installing. Verify checks whether the game landed on the console; Retry sends it again.";
    for (const auto& t : tasks) {
        const auto pct = t.total_bytes ? std::to_string(t.processed_bytes * 100 / t.total_bytes) + "%" : std::string("?");
        interrupted_ids_.push_back(t.task_id);
        dialog_model_.options.push_back("Verify: " + t.display_name + " (" + pct + ")");
        interrupted_ids_.push_back(t.task_id);
        dialog_model_.options.push_back("Retry: " + t.display_name);
    }
    dialog_model_.options.push_back("Later");
    return true;
}

bool UiShell::verify_interrupted_install(const std::string& task_id) {
    if (!installer_) return false;
    const auto task = installer_->get_task(task_id);
    if (!task) return false;
    std::string why;
    const bool ok = verify_installed_title(apps_root_, task->title_id, why);
    installer_->resolve_interrupted(task_id, ok, ok ? "Confirmed installed after restart" : why);
    if (ok) {
        record_event("info", "install", task->display_name + " was installed", "Title folder found after restart");
        show_recovery_guidance("Install confirmed", task->display_name + " (" + task->title_id + ") is on the console.", "");
    } else {
        record_event("error", "install", task->display_name + " is not installed", why, "Choose Retry to send it again.");
        show_recovery_guidance("Install not found", why, "Check Notifications > Downloads; if nothing is installing, choose Retry Install.");
    }
    return ok;
}

bool UiShell::retry_interrupted_install(const std::string& task_id) {
    if (!installer_) return false;
    std::string error;
    const auto id = installer_->retry_task(task_id, &error);
    installer_->resolve_interrupted(task_id, false, id.empty() ? error : "Retried as " + id);
    if (id.empty()) {
        record_event("error", "install", "Retry could not start", error, "Check the share is connected, then retry from Install History.");
        show_recovery_guidance("Retry not started", error, "Check the share is connected, then retry from Install History.");
        return false;
    }
    record_event("info", "install", "Interrupted install queued again", task_id);
    if (!show_interrupted_installs()) show_install_history();
    return true;
}

std::string UiShell::newest_package_version(const GameEntry& entry) const {
    std::string best = entry.base_game ? entry.base_game->version : std::string();
    for (const auto& u : entry.updates)
        if (!u.version.empty() && (best.empty() || compare_versions(u.version, best) > 0)) best = u.version;
    return best;
}

std::string UiShell::update_note(const GameEntry& entry) const {
    if (!valid_native_title(entry.title_id)) return {};
    const auto pkg = newest_package_version(entry);
    const auto installed = installed_title_version(apps_root_, entry.title_id);
    if (pkg.empty() || installed.empty()) return {};
    if (compare_versions(pkg, installed) > 0) return "Update available: v" + pkg + " (installed v" + installed + ")";
    return "Up to date (v" + installed + ")";
}

void UiShell::show_game_details() {
    const auto entry = focused_entry();
    if (!entry) return;
    dialog_model_ = {};
    dialog_model_.type = DialogType::GameDetails;
    dialog_model_.title = entry->title.empty() ? entry->title_id : entry->title;
    std::string m = "Title ID: " + (entry->title_id.empty() ? std::string("unknown") : entry->title_id);
    if (entry->platform) m += "\nPlatform: " + std::string(to_string(*entry->platform));
    if (!entry->region.empty()) m += "\nRegion: " + entry->region;
    if (!entry->content_id.empty()) m += "\nContent ID: " + entry->content_id;
    m += "\nSize: " + (entry->formatted_total_size.empty() ? format_file_size(entry->total_size_bytes) : entry->formatted_total_size);
    if (const auto v = newest_package_version(*entry); !v.empty()) m += "\nPackage version: v" + v;
    m += "\nIdentified from: " + std::string(entry->confidence == MetadataConfidence::Verified ? "file contents" : "filename (unconfirmed)");
    m += "\nInstall status: " + std::string(is_title_installed(entry->title_id) ? "installed" : "not installed");
    if (const auto note = update_note(*entry); !note.empty()) m += "\n" + note;
    m += "\nUpdates: " + std::to_string(entry->updates.size()) + "   DLC: " + std::to_string(entry->dlcs.size());
    dialog_model_.message = m;
    std::string where;
    for (const auto& c : entry->copies) where += (where.empty() ? "" : "\n") + (c.source_id.empty() ? c.source_root : c.source_id) + " - " + c.source_root;
    dialog_model_.secondary_text = where.empty() ? "Found on: " + (entry->source_id.empty() ? entry->source_root : entry->source_id) : "Found on:\n" + where;
    dialog_model_.options = {"Check Before Installing", "Back to Game Menu", "Close"};
}

void UiShell::show_install_check() {
    const auto entry = focused_entry();
    if (!entry) return;
    dialog_model_ = {};
    dialog_model_.type = DialogType::InstallCheck;
    dialog_model_.title = "Check Before Installing";
    if (!entry->base_game || entry->base_game->absolute_path.empty()) {
        dialog_model_.message = "This entry has no package file to check.";
        dialog_model_.options = {"Back"};
        return;
    }
    const auto r = check_install_readiness(entry->base_game->absolute_path, apps_root_, entry->title_id);
    dialog_model_.message = r.summary();
    std::string fixes;
    for (const auto& c : r.checks) if (!c.passed && !c.fix.empty()) fixes += "- " + c.fix + "\n";
    dialog_model_.secondary_text = r.ok ? (r.same_or_older ? "Ready, but the console already has this version or newer.\n" + fixes : "Ready to install.")
                                        : "Not ready:\n" + fixes;
    if (!r.ok) record_event("warning", "install", entry->title + " is not ready to install", r.summary(), fixes);
    dialog_model_.options = {r.ok ? (r.same_or_older ? "Install Anyway" : "Install Now") : "Check Again", "Back"};
}

void UiShell::show_bulk_actions() {
    dialog_model_ = {};
    dialog_model_.type = DialogType::BulkActions;
    dialog_model_.title = "Bulk Actions";
    dialog_model_.message = "Applies to the " + std::to_string(filtered_entries_.size()) + " game(s) shown in this tab (search and filters apply).";
    dialog_model_.options = {"Add All to Favorites", "Remove All from Favorites", "Install All Shown", "Retry All Failed Installs", "Back"};
}

int UiShell::bulk_set_favorites(bool favorite) {
    int changed = 0;
    for (const auto& e : filtered_entries_) {
        const auto key = library_key(e);
        if (favorite ? library_state_.favorites.insert(key).second : library_state_.favorites.erase(key) > 0) ++changed;
    }
    if (changed) { update_filtered_entries(); save_library_state(); }
    return changed;
}

int UiShell::bulk_queue_installs() {
    if (!installer_) return 0;
    int queued = 0;
    for (const auto& e : filtered_entries_) {
        if (!e.base_game || is_title_installed(e.title_id)) continue;
        if (!installer_->queue_item(*e.base_game, e.title_id).empty()) ++queued;
    }
    if (queued) installer_->start_worker();
    return queued;
}

int UiShell::bulk_retry_failed_installs() {
    if (!installer_) return 0;
    int retried = 0;
    for (const auto& t : installer_->failed_tasks())
        if (!installer_->retry_task(t.task_id).empty()) ++retried;
    if (retried) installer_->start_worker();
    return retried;
}

bool UiShell::export_settings_backup(const std::string& path, std::string* error) const {
    auto fail = [&](const std::string& m) { if (error) *error = m; return false; };
    nlohmann::json root;
    root["nexodion_settings_backup"] = 1;
    root["sources"] = nlohmann::json::parse(sources_.to_json(), nullptr, false);
    if (!ui_state_path_.empty()) {
        std::ifstream in(ui_state_path_);
        auto j = nlohmann::json::parse(in, nullptr, false);
        if (j.is_object()) root["library"] = j;
    }
    std::error_code ec;
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    if (ec) return fail("Cannot create " + parent.string() + ": " + ec.message());
    const auto tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return fail("Cannot write " + tmp);
        out << root.dump(2);
        if (!out) return fail("Write failed (storage full?) for " + tmp);
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) { std::filesystem::remove(tmp, ec); return fail("Cannot replace " + path); }
    return true;
}

bool UiShell::import_settings_backup(const std::string& path, std::string* error) {
    auto fail = [&](const std::string& m) { if (error) *error = m; return false; };
    std::ifstream in(path);
    if (!in) return fail("Cannot open " + path);
    auto root = nlohmann::json::parse(in, nullptr, false);
    if (!root.is_object() || root.value("nexodion_settings_backup", 0) != 1) return fail("This file is not a Nexodion settings backup");
    if (root.contains("sources")) {
        if (!root["sources"].is_object() && !root["sources"].is_array()) return fail("Backup mounting section is damaged");
        SourceManager probe;
        if (!probe.load_json(root["sources"].dump())) return fail("Backup mounting settings are not valid");
        if (!sources_.load_json(root["sources"].dump())) return fail("Mounting settings could not be applied");
        if (!sources_path_.empty()) {
            std::string e;
            if (!sources_.save_file(sources_path_, &e)) return fail("Mounting restored for this session but not saved: " + e);
        }
    }
    if (root.contains("library") && root["library"].is_object() && !ui_state_path_.empty()) {
        keep_backup(ui_state_path_);
        std::ofstream out(ui_state_path_, std::ios::trunc);
        if (!out) return fail("Cannot write " + ui_state_path_);
        out << root["library"].dump(2);
        out.close();
        set_ui_state_path(ui_state_path_);
    }
    return true;
}

void UiShell::apply_settings_path(const std::string& purpose, const std::string& value) {
    std::string error;
    if (purpose == "backup_path") {
        const auto path = value.empty() ? settings_backup_path_ : value;
        if (export_settings_backup(path, &error)) { settings_backup_path_ = path; show_recovery_guidance("Settings backed up", path, "Passwords are never included."); }
        else { record_event("error", "settings", "Backup not saved", error, "Pick a writable folder such as a USB drive."); show_recovery_guidance("Backup not saved", error, "Pick a writable folder such as a USB drive."); }
    } else if (purpose == "restore_path") {
        const auto path = value.empty() ? settings_backup_path_ : value;
        if (import_settings_backup(path, &error)) show_recovery_guidance("Settings restored", path, "Enter share passwords again in Mounting; they are never stored.");
        else { record_event("error", "settings", "Restore failed", error, "Choose a file made with Back Up Settings."); show_recovery_guidance("Restore failed", error, "Choose a file made with Back Up Settings."); }
    } else if (purpose == "logs_path") {
        const auto dir = value.empty() ? logs_export_dir_ : value;
        const int n = events_.export_to(dir, &error);
        if (n > 0) show_recovery_guidance("Logs exported", std::to_string(n) + " file(s) copied to " + dir, "");
        else show_recovery_guidance("Logs not exported", error, "Pick a writable folder such as a USB drive.");
    }
}

bool UiShell::start_background_refresh() {
    if (scanner_.is_scanning() || sources_.list().empty() || all_entries_.empty()) return false;
    background_refresh_ = true;
    trigger_scan(false, [this](bool) { background_refresh_ = false; });
    return true;
}

void UiShell::show_install_queue() {
    dialog_model_ = {};
    dialog_model_.type = DialogType::InstallQueue;
    dialog_model_.title = "Install Queue";
    queue_ids_.clear();
    if (!installer_) { dialog_model_.message = "No installer connected."; dialog_model_.options = {"Back"}; return; }
    const bool step = installer_->one_at_a_time();
    std::string m;
    if (const auto active = installer_->current_active_task()) {
        const auto pct = active->total_bytes ? std::to_string(active->processed_bytes * 100 / active->total_bytes) + "%" : std::string("starting");
        m = "Installing now: " + active->display_name + " (" + pct + ")\n";
    }
    const auto queued = installer_->queued_tasks();
    m += queued.empty() ? "Nothing is waiting." : std::to_string(queued.size()) + " waiting, " +
         std::to_string(installer_->runnable_queued_count()) + " ready. Choose a game to move or pause it.";
    m += step ? "\nOne at a time: press Continue Next to start each install." : "\nAutomatic: installs run in this order.";
    const auto failed = installer_->failed_tasks();
    if (!failed.empty()) m += "\n" + std::to_string(failed.size()) + " failed: choose one to retry it without adding it again.";
    dialog_model_.message = m;
    for (size_t i = 0; i < queued.size(); ++i) {
        queue_ids_.push_back(queued[i].task_id);
        dialog_model_.options.push_back(std::to_string(i + 1) + ". " + queued[i].display_name + (queued[i].paused ? " [Paused]" : ""));
    }
    for (const auto& f : failed) {
        queue_ids_.push_back(f.task_id);
        dialog_model_.options.push_back("[Failed] " + f.display_name);
    }
    if (step) dialog_model_.options.push_back("Continue Next");
    dialog_model_.options.push_back(step ? "Mode: One at a time" : "Mode: Automatic");
    dialog_model_.options.push_back("Back");
}

void UiShell::show_queue_item(const std::string& task_id) {
    const auto task = installer_ ? installer_->get_task(task_id) : std::nullopt;
    if (!task || (task->status != InstallStatus::Queued && task->status != InstallStatus::Failed &&
                  task->status != InstallStatus::Interrupted)) { show_install_queue(); return; }
    queue_item_id_ = task_id;
    dialog_model_ = {};
    dialog_model_.type = DialogType::QueueItem;
    dialog_model_.title = task->display_name;
    const std::string base = (task->title_id.empty() ? std::string() : task->title_id + "  ") + format_file_size(task->total_bytes);
    if (task->status == InstallStatus::Queued) {
        dialog_model_.message = base + (task->paused ? "\nPaused: it stays in the queue and is skipped until you resume it." : "");
        dialog_model_.options = {"Move Up", "Move Down", "Move to Top", task->paused ? "Resume" : "Pause",
                                 "Remove from Queue", "Back to Queue"};
    } else {
        dialog_model_.message = base + "\nFailed: " + (task->error_message.empty() ? std::string("unknown reason") : task->error_message) +
                                "\nRetry puts it back in the queue as the same entry; nothing is added twice.";
        dialog_model_.options = {"Retry Install", "Remove from History", "Back to Queue"};
    }
}

// Confirm handler for the dialogs above and the new Settings entries.
bool UiShell::handle_recovery_choice(const std::string& choice) {
    const auto type = dialog_model_.type;
    if (type == DialogType::Settings) {
        if (choice == "Error History") { show_error_history(); return true; }
        if (choice == "Bulk Actions") { show_bulk_actions(); return true; }
        if (choice == "Install Queue") { show_install_queue(); return true; }
        if (choice == "Back Up Settings") { show_text_entry(settings_backup_path_, "backup_path"); return true; }
        if (choice == "Restore Settings") { show_text_entry(settings_backup_path_, "restore_path"); return true; }
        if (choice == "Interrupted Installs") { if (!show_interrupted_installs()) show_recovery_guidance("No interrupted installs", "Every install finished or was already reviewed.", ""); return true; }
        return false;
    }
    if (type == DialogType::ErrorHistory) {
        if (choice == "Export Logs") show_text_entry(logs_export_dir_, "logs_path");
        else if (choice == "Clear History") { events_.clear_memory(); show_error_history(); }
        else if (choice == "Back") show_settings();
        else {
            const auto list = events_.recent(50);
            const auto i = static_cast<size_t>(dialog_model_.selected_option);
            if (i < list.size()) {
                const auto& e = list[i];
                show_recovery_guidance(e.code + ": " + e.what, e.why.empty() ? e.what : e.why, e.fix);
                dialog_model_.options = {"Dismiss"};
            }
        }
        return true;
    }
    if (type == DialogType::InterruptedInstalls) {
        const auto i = static_cast<size_t>(dialog_model_.selected_option);
        if (choice == "Later" || i >= interrupted_ids_.size()) { dismiss_dialog(); return true; }
        if (choice.starts_with("Verify: ")) verify_interrupted_install(interrupted_ids_[i]);
        else retry_interrupted_install(interrupted_ids_[i]);
        return true;
    }
    if (type == DialogType::GameDetails) {
        if (choice == "Check Before Installing") show_install_check();
        else if (choice == "Back to Game Menu") show_game_actions();
        else dismiss_dialog();
        return true;
    }
    if (type == DialogType::InstallCheck) {
        if (choice == "Check Again") show_install_check();
        else if (choice == "Install Now" || choice == "Install Anyway") {
            const auto entry = focused_entry();
            if (entry && entry->base_game && installer_ && !installer_->queue_item(*entry->base_game, entry->title_id).empty()) {
                installer_->start_worker();
                show_install_history();
            } else show_recovery_guidance("Not queued", "This package is already queued or installing.", "Open Install History to follow it.");
        } else show_game_details();
        return true;
    }
    if (type == DialogType::InstallQueue) {
        const auto i = static_cast<size_t>(dialog_model_.selected_option);
        if (i < queue_ids_.size()) show_queue_item(queue_ids_[i]);
        else if (choice == "Continue Next") {
            if (installer_->current_active_task()) show_recovery_guidance("Install in progress", "Wait for the current install to finish, then continue.", "");
            else if (installer_->continue_next()) { installer_->start_worker(); show_install_queue(); }
            else show_recovery_guidance("Nothing to continue", "Every waiting install is paused, or the queue is empty.", "Resume a game in the queue first.");
        } else if (choice.starts_with("Mode: ")) { installer_->set_one_at_a_time(!installer_->one_at_a_time()); installer_->start_worker(); show_install_queue(); }
        else show_settings();
        return true;
    }
    if (type == DialogType::QueueItem) {
        const auto& id = queue_item_id_;
        if (choice == "Move Up") installer_->move_queued(id, -1);
        else if (choice == "Move Down") installer_->move_queued(id, 1);
        else if (choice == "Move to Top") installer_->move_queued(id, -1000000);
        else if (choice == "Pause") installer_->set_queued_paused(id, true);
        else if (choice == "Resume") installer_->set_queued_paused(id, false);
        else if (choice == "Remove from Queue") {
            installer_->remove_queued(id);
            show_install_queue();
            return true;
        } else if (choice == "Retry Install") {
            std::string error;
            if (installer_->requeue_failed(id, &error)) { installer_->start_worker(); show_install_queue(); }
            else show_recovery_guidance("Could not retry", error, "Open Install History for details.");
            return true;
        } else if (choice == "Remove from History") {
            installer_->remove_history_entry(id);
            show_install_queue();
            return true;
        }
        if (choice == "Back to Queue") show_install_queue(); else show_queue_item(id);
        return true;
    }
    if (type == DialogType::BulkActions) {
        if (choice == "Add All to Favorites") show_recovery_guidance("Favorites updated", std::to_string(bulk_set_favorites(true)) + " game(s) added.", "");
        else if (choice == "Remove All from Favorites") show_recovery_guidance("Favorites updated", std::to_string(bulk_set_favorites(false)) + " game(s) removed.", "");
        else if (choice == "Install All Shown") {
            const int n = bulk_queue_installs();
            show_recovery_guidance("Installs queued", std::to_string(n) + " package(s) queued. Each one is checked for space and readability before it is sent.",
                                   n ? "" : "Nothing new to install: games are already installed, queued or have no package file.");
        } else if (choice == "Retry All Failed Installs") show_recovery_guidance("Retries queued", std::to_string(bulk_retry_failed_installs()) + " failed install(s) queued again.", "");
        else show_settings();
        return true;
    }
    return false;
}

} // namespace nexodion
