#pragma once
// Error history and on-console activity log. Every user-facing failure is
// recorded with the same parts: what happened, why, what to do, and a code.
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace nexodion {

struct EventRecord {
    int64_t unix_seconds{0};
    std::string severity; // "error", "warning", "info"
    std::string area;     // "install", "startup", "scan", "launch", "settings" ...
    std::string what;
    std::string why;
    std::string fix;
    std::string code;     // short stable code, e.g. "NX-INSTALL"
    std::string line() const; // one readable line for the history list
};

// Short code from area + severity, e.g. ("install","error") -> "NX-INS-E".
std::string event_code(const std::string& area, const std::string& severity);

class EventLog {
public:
    // Directory for nexodion.log (JSON lines). Empty disables the file.
    void set_directory(std::string dir);
    const std::string& directory() const { return dir_; }
    void set_limits(size_t max_file_bytes, int kept_files, size_t kept_in_memory);
    void record(EventRecord e);
    std::vector<EventRecord> recent(size_t max = 200) const; // newest first
    size_t error_count() const;
    void clear_memory();
    // Copies the current and rotated log files into dest_dir; returns files copied.
    int export_to(const std::string& dest_dir, std::string* error = nullptr) const;
    std::string file_path() const;

private:
    void rotate_locked();
    mutable std::mutex mu_;
    std::string dir_;
    size_t max_file_bytes_{256 * 1024};
    int kept_files_{3};
    size_t kept_in_memory_{500};
    std::deque<EventRecord> events_;
};

} // namespace nexodion
