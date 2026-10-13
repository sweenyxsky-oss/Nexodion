#include "nexodion/library_state.hpp"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace nexodion {

const char* to_string(LibrarySort sort) {
    switch (sort) {
        case LibrarySort::Size: return "Size";
        case LibrarySort::RecentlyAdded: return "Recently Added";
        case LibrarySort::RecentlyPlayed: return "Recently Played";
        default: return "Title";
    }
}

LibrarySort library_sort_from_string(const std::string& v) {
    if (v == "Size") return LibrarySort::Size;
    if (v == "Recently Added") return LibrarySort::RecentlyAdded;
    if (v == "Recently Played") return LibrarySort::RecentlyPlayed;
    return LibrarySort::Title;
}

LibrarySort next_library_sort(LibrarySort s) {
    switch (s) {
        case LibrarySort::Title: return LibrarySort::Size;
        case LibrarySort::Size: return LibrarySort::RecentlyAdded;
        case LibrarySort::RecentlyAdded: return LibrarySort::RecentlyPlayed;
        default: return LibrarySort::Title;
    }
}

std::string library_key(const GameEntry& e) {
    if (!e.title_id.empty()) return e.title_id + "|" + (e.platform ? std::string(to_string(*e.platform)) : "");
    return e.source_root + "@" + e.title;
}

namespace {
bool valid_json_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) return false;
    auto j = nlohmann::json::parse(in, nullptr, false);
    return !j.is_discarded() && (j.is_object() || j.is_array());
}
}

void keep_backup(const std::string& path) {
    std::error_code ec;
    if (valid_json_file(path))
        std::filesystem::copy_file(path, path + ".bak", std::filesystem::copy_options::overwrite_existing, ec);
}

std::string recover_json_file(const std::string& path) {
    std::error_code ec;
    if (path.empty() || !std::filesystem::exists(path, ec) || valid_json_file(path)) return {};
    std::filesystem::remove(path + ".corrupt", ec);
    std::filesystem::rename(path, path + ".corrupt", ec);
    std::string msg = std::filesystem::path(path).filename().string() + " was damaged and was set aside as " +
                      std::filesystem::path(path).filename().string() + ".corrupt. ";
    if (valid_json_file(path + ".bak")) {
        std::filesystem::copy_file(path + ".bak", path, std::filesystem::copy_options::overwrite_existing, ec);
        msg += ec ? "The backup could not be restored, so defaults are used." : "Your last good backup was restored.";
    } else msg += "No backup was available, so defaults are used.";
    return msg;
}

bool LibraryState::load(const std::string& path, std::string* error) {
    std::ifstream in(path);
    if (!in) { if (error) *error = "No saved library preferences"; return false; }
    auto j = nlohmann::json::parse(in, nullptr, false);
    if (!j.is_object()) { if (error) *error = "Library preferences file is not valid JSON"; return false; }
    LibraryState s;
    if (j.contains("sort") && j["sort"].is_string()) s.sort = library_sort_from_string(j["sort"].get<std::string>());
    if (j.contains("view_layout") && j["view_layout"].is_string()) s.view_layout = j["view_layout"].get<std::string>();
    if (j.contains("format_filter") && j["format_filter"].is_string()) s.format_filter = j["format_filter"].get<std::string>();
    if (j.contains("favorites") && j["favorites"].is_array())
        for (const auto& f : j["favorites"]) if (f.is_string() && f.get<std::string>().size() < 512) s.favorites.insert(f.get<std::string>());
    auto read_times = [&](const char* name, std::map<std::string, int64_t>& out) {
        if (!j.contains(name) || !j[name].is_object()) return;
        for (auto it = j[name].begin(); it != j[name].end(); ++it)
            if (it.value().is_number_integer() && it.key().size() < 512) out[it.key()] = it.value().get<int64_t>();
    };
    read_times("last_played", s.last_played);
    read_times("first_seen", s.first_seen);
    if (j.contains("launchers") && j["launchers"].is_object())
        for (auto it = j["launchers"].begin(); it != j["launchers"].end(); ++it)
            if (it.value().is_string() && it.value().get<std::string>().size() <= 16 &&
                (it.key() == "PS1" || it.key() == "PS2" || it.key() == "PS3"))
                s.launchers[it.key()] = it.value().get<std::string>();
    *this = std::move(s);
    return true;
}

bool LibraryState::save(const std::string& path, std::string* error) const {
    nlohmann::json j;
    j["version"] = 1;
    j["sort"] = to_string(sort);
    j["view_layout"] = view_layout;
    j["format_filter"] = format_filter;
    j["favorites"] = favorites;
    j["last_played"] = last_played;
    j["first_seen"] = first_seen;
    j["launchers"] = launchers;
    std::error_code ec;
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    const std::string tmp = path + ".tmp";
    keep_backup(path);
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) { if (error) *error = "Cannot write " + tmp; return false; }
        out << j.dump(2);
        if (!out) { if (error) *error = "Write failed (storage full?) for " + tmp; return false; }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) { std::filesystem::remove(tmp, ec); if (error) *error = "Cannot replace " + path; return false; }
    return true;
}

} // namespace nexodion
