#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace nexodion {

// Where an identification came from. Header data is authoritative; filename
// data is a pattern match only and must be presented as unconfirmed.
enum class IdentitySource { PackageHeader, Filename, Unknown };
const char* to_string(IdentitySource source);

struct PackageIdentity {
    bool ok{false};
    IdentitySource source{IdentitySource::Unknown};
    std::string title;
    std::string title_id;
    std::string content_id;
    std::string platform;   // "PS5", "PS4" or empty when not determinable
    std::string region;     // "Europe", "Americas", "Japan", "Asia" or empty
    std::string category;   // "gd", "gp", "ac" ...
    std::string version;    // APP_VER / contentVersion, e.g. "01.020", may be empty
    std::vector<unsigned char> icon_png; // icon0.png from the entry table (PNG-signature checked), may be empty
    std::string error;
};

// Random-access reader: fill `out` with exactly `len` bytes at `offset`.
using PackageReadAt = std::function<bool(uint64_t offset, size_t len, std::vector<unsigned char>& out)>;

// Ported from PKG Viewer (Loopayeh, MIT, sweenyxsky-oss/pkg-viewer):
// CNT (PS4) and FIH/LIH (PS5) headers, entry table, name table, param.sfo /
// param.json. Every offset and size is bounds-checked against `file_size`.
PackageIdentity identify_package(const PackageReadAt& read_at, uint64_t file_size);
PackageIdentity identify_package_file(const std::filesystem::path& path);

// Filename-only identification (content ID / title ID patterns).
PackageIdentity identify_from_filename(const std::string& filename);

std::string region_from_content_id(const std::string& content_id);
std::string region_from_title_id(const std::string& title_id);

} // namespace nexodion
