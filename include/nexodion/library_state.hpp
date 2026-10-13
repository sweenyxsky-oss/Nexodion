#pragma once
// Persistent per-user library preferences: sort order, view, filter,
// favorites, recently played and first-seen times. Stored as JSON so the
// choices survive a console restart. Contains no credentials.

#include "game_metadata.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <string>

namespace nexodion {

enum class LibrarySort { Title, Size, RecentlyAdded, RecentlyPlayed };
const char* to_string(LibrarySort sort);
LibrarySort library_sort_from_string(const std::string& value);
LibrarySort next_library_sort(LibrarySort sort);

// Stable key for a title across sources: title ID + platform, or the source
// root + title when no title ID is known.
std::string library_key(const GameEntry& entry);

// Saved-file safety: keep_backup copies a valid file to <path>.bak before it is
// replaced. recover_json_file moves an unreadable file to <path>.corrupt and
// restores <path>.bak when that is valid. Returns a message describing what was
// done, or empty when the file was fine or absent.
void keep_backup(const std::string& path);
std::string recover_json_file(const std::string& path);

struct LibraryState {
    LibrarySort sort{LibrarySort::Title};
    std::string view_layout;   // "coverflow" / "grid"
    std::string format_filter; // to_string(FormatFilter)
    std::set<std::string> favorites;
    std::map<std::string, int64_t> last_played; // key -> unix seconds
    std::map<std::string, int64_t> first_seen;  // key -> unix seconds
    // Legacy launcher per platform ("PS1"/"PS2"/"PS3"): absent = built-in emulator,
    // "none" = launching disabled, otherwise a custom installed app title ID.
    std::map<std::string, std::string> launchers;

    bool load(const std::string& path, std::string* error = nullptr);
    // Atomic write (temp file + rename).
    bool save(const std::string& path, std::string* error = nullptr) const;
};

} // namespace nexodion
