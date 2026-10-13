#include "nexodion/event_log.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace nexodion {

std::string event_code(const std::string& area, const std::string& severity) {
    std::string a;
    for (char c : area) { if (std::isalpha(static_cast<unsigned char>(c))) a += char(std::toupper(static_cast<unsigned char>(c))); if (a.size() == 3) break; }
    if (a.empty()) a = "GEN";
    const char s = severity == "error" ? 'E' : severity == "warning" ? 'W' : 'I';
    return "NX-" + a + "-" + s;
}

std::string EventRecord::line() const {
    std::time_t t = static_cast<std::time_t>(unix_seconds);
    char stamp[32] = "";
    if (std::tm* tm = std::gmtime(&t)) std::strftime(stamp, sizeof(stamp), "%m-%d %H:%M", tm);
    std::string out = std::string(stamp) + " [" + code + "] " + what;
    if (!why.empty()) out += " - " + why;
    return out;
}

void EventLog::set_directory(std::string dir) { std::lock_guard l(mu_); dir_ = std::move(dir); }
void EventLog::set_limits(size_t max_file_bytes, int kept_files, size_t kept_in_memory) {
    std::lock_guard l(mu_);
    max_file_bytes_ = std::max<size_t>(1024, max_file_bytes);
    kept_files_ = std::clamp(kept_files, 1, 10);
    kept_in_memory_ = std::max<size_t>(1, kept_in_memory);
}
std::string EventLog::file_path() const { return dir_.empty() ? std::string() : (std::filesystem::path(dir_) / "nexodion.log").string(); }

void EventLog::rotate_locked() {
    std::error_code ec;
    const auto base = std::filesystem::path(dir_) / "nexodion.log";
    if (!std::filesystem::exists(base, ec) || std::filesystem::file_size(base, ec) < max_file_bytes_) return;
    std::filesystem::remove(base.string() + "." + std::to_string(kept_files_), ec);
    for (int i = kept_files_ - 1; i >= 1; --i)
        std::filesystem::rename(base.string() + "." + std::to_string(i), base.string() + "." + std::to_string(i + 1), ec);
    std::filesystem::rename(base, base.string() + ".1", ec);
}

void EventLog::record(EventRecord e) {
    if (e.unix_seconds == 0)
        e.unix_seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    if (e.severity.empty()) e.severity = "info";
    if (e.code.empty()) e.code = event_code(e.area, e.severity);
    auto clip = [](std::string& s) { if (s.size() > 2000) s.resize(2000); };
    clip(e.what); clip(e.why); clip(e.fix);
    std::lock_guard l(mu_);
    events_.push_front(e);
    while (events_.size() > kept_in_memory_) events_.pop_back();
    if (dir_.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(dir_, ec);
    rotate_locked();
    std::ofstream out(std::filesystem::path(dir_) / "nexodion.log", std::ios::app);
    if (!out) return; // logging must never break the app
    out << nlohmann::json{{"t", e.unix_seconds}, {"severity", e.severity}, {"area", e.area}, {"code", e.code},
                          {"what", e.what}, {"why", e.why}, {"fix", e.fix}}.dump() << '\n';
}

std::vector<EventRecord> EventLog::recent(size_t max) const {
    std::lock_guard l(mu_);
    return {events_.begin(), events_.begin() + static_cast<long>(std::min(max, events_.size()))};
}
size_t EventLog::error_count() const {
    std::lock_guard l(mu_);
    return static_cast<size_t>(std::count_if(events_.begin(), events_.end(), [](const auto& e) { return e.severity == "error"; }));
}
void EventLog::clear_memory() { std::lock_guard l(mu_); events_.clear(); }

int EventLog::export_to(const std::string& dest_dir, std::string* error) const {
    std::lock_guard l(mu_);
    if (dir_.empty()) { if (error) *error = "Logging to a file is turned off"; return 0; }
    std::error_code ec;
    std::filesystem::create_directories(dest_dir, ec);
    if (ec) { if (error) *error = "Cannot create " + dest_dir + ": " + ec.message(); return 0; }
    int copied = 0;
    const auto base = std::filesystem::path(dir_) / "nexodion.log";
    for (int i = 0; i <= kept_files_; ++i) {
        const auto src = i == 0 ? base : std::filesystem::path(base.string() + "." + std::to_string(i));
        if (!std::filesystem::exists(src, ec)) continue;
        std::filesystem::copy_file(src, std::filesystem::path(dest_dir) / src.filename(), std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) { if (error) *error = "Copy failed: " + ec.message(); return copied; }
        ++copied;
    }
    if (copied == 0 && error) *error = "No log file has been written yet";
    return copied;
}

} // namespace nexodion
