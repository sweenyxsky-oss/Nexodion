#include "nexodion/package_identity.hpp"
#include "nexodion/game_metadata.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <regex>

namespace nexodion {

const char* to_string(IdentitySource s) {
    switch (s) {
        case IdentitySource::PackageHeader: return "package_header";
        case IdentitySource::Filename: return "filename";
        default: return "unknown";
    }
}

namespace {
constexpr uint32_t kMaxEntries = 65536;
constexpr uint32_t kMaxMetaBytes = 1024 * 1024;
constexpr uint32_t kMaxIconBytes = 4 * 1024 * 1024;

uint32_t be32(const std::vector<unsigned char>& b, size_t o) {
    return (uint32_t(b[o]) << 24) | (uint32_t(b[o + 1]) << 16) | (uint32_t(b[o + 2]) << 8) | b[o + 3];
}
uint64_t le64(const std::vector<unsigned char>& b, size_t o) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | b[o + i];
    return v;
}
std::string cstr(const std::vector<unsigned char>& b, size_t o, size_t max) {
    std::string s;
    for (size_t i = o; i < b.size() && i < o + max && b[i]; ++i) s.push_back(char(b[i]));
    return s;
}
bool in_bounds(uint64_t off, uint64_t len, uint64_t size) { return off <= size && len <= size - off; }

struct Entry { uint32_t id, name_off, off, size; std::string name; };

PackageIdentity parse_cnt(const PackageReadAt& read, uint64_t size, uint64_t base, const std::string& platform) {
    PackageIdentity id;
    std::vector<unsigned char> hdr;
    if (!in_bounds(base, 0x80, size) || !read(base, 0x80, hdr) || hdr.size() != 0x80) { id.error = "Package header is truncated."; return id; }
    if (!(hdr[0] == 0x7f && hdr[1] == 'C' && hdr[2] == 'N' && hdr[3] == 'T')) { id.error = "Embedded container is not CNT."; return id; }
    const uint32_t count = be32(hdr, 0x10), table = be32(hdr, 0x18);
    id.content_id = cstr(hdr, 0x40, 48);
    if (count == 0 || count > kMaxEntries || !in_bounds(base + table, uint64_t(count) * 32, size)) { id.error = "Entry table is out of range."; return id; }
    std::vector<unsigned char> raw;
    if (!read(base + table, size_t(count) * 32, raw) || raw.size() != size_t(count) * 32) { id.error = "Entry table could not be read."; return id; }
    std::vector<Entry> ents;
    for (uint32_t i = 0; i < count; ++i) ents.push_back({be32(raw, i * 32), be32(raw, i * 32 + 4), be32(raw, i * 32 + 16), be32(raw, i * 32 + 20), {}});
    auto load = [&](const Entry& e, std::vector<unsigned char>& out) {
        return e.size > 0 && e.size <= kMaxMetaBytes && in_bounds(base + e.off, e.size, size) && read(base + e.off, e.size, out) && out.size() == e.size;
    };
    auto names = std::find_if(ents.begin(), ents.end(), [](const Entry& e) { return e.id == 512; });
    std::vector<unsigned char> nt;
    if (names != ents.end() && load(*names, nt))
        for (auto& e : ents) if (e.name_off < nt.size()) e.name = cstr(nt, e.name_off, nt.size() - e.name_off);

    std::vector<unsigned char> data;
    auto pj = std::find_if(ents.begin(), ents.end(), [](const Entry& e) { return e.id == 8192 || e.name == "param.json"; });
    if (pj != ents.end() && load(*pj, data)) {
        auto j = nlohmann::json::parse(data.begin(), data.end(), nullptr, false);
        if (j.is_object()) {
            id.title_id = j.value("titleId", "");
            if (j.contains("contentId") && j["contentId"].is_string()) id.content_id = j["contentId"].get<std::string>();
            if (j.contains("contentVersion") && j["contentVersion"].is_string()) id.version = j["contentVersion"].get<std::string>().substr(0, 32);
            if (auto lp = j.find("localizedParameters"); lp != j.end() && lp->is_object()) {
                std::string lang = lp->value("defaultLanguage", "en-US");
                if (auto t = lp->find(lang); t != lp->end() && t->is_object()) id.title = t->value("titleName", "");
            }
        }
    }
    auto sfo = std::find_if(ents.begin(), ents.end(), [](const Entry& e) { return e.id == 4096 || e.name == "param.sfo"; });
    if (id.title.empty() && sfo != ents.end() && load(*sfo, data)) {
        ParamMetadata m = parse_param_sfo_bytes(data);
        if (m.valid) { id.title = m.title_name; id.title_id = m.title_id; id.category = m.category; if (id.version.empty()) id.version = m.version.substr(0, 32); }
    } else if (id.version.empty() && sfo != ents.end() && load(*sfo, data)) {
        ParamMetadata m = parse_param_sfo_bytes(data);
        if (m.valid) id.version = m.version.substr(0, 32);
    }
    // Cover art: icon0.png entry (id 0x1200). Bounded and signature-checked;
    // a missing or bad icon never fails identification.
    auto icon = std::find_if(ents.begin(), ents.end(), [](const Entry& e) { return e.id == 0x1200 || e.name == "icon0.png"; });
    if (icon != ents.end() && icon->size >= 8 && icon->size <= kMaxIconBytes && in_bounds(base + icon->off, icon->size, size)) {
        std::vector<unsigned char> png;
        static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        if (read(base + icon->off, icon->size, png) && png.size() == icon->size && std::equal(sig, sig + 8, png.begin()))
            id.icon_png = std::move(png);
    }
    if (id.title_id.empty() && id.content_id.size() >= 16) id.title_id = id.content_id.substr(7, 9);
    id.platform = platform;
    id.region = region_from_content_id(id.content_id);
    id.source = IdentitySource::PackageHeader;
    id.ok = !id.content_id.empty() || !id.title_id.empty();
    if (!id.ok) id.error = "No identity fields found in the package.";
    return id;
}
} // namespace

std::string region_from_content_id(const std::string& cid) {
    static const std::map<std::string, std::string> r{{"EP", "Europe"}, {"UP", "Americas"}, {"JP", "Japan"}, {"HP", "Asia"}};
    if (cid.size() < 2) return "";
    std::string p{char(std::toupper((unsigned char)cid[0])), char(std::toupper((unsigned char)cid[1]))};
    auto it = r.find(p);
    return it == r.end() ? "" : it->second;
}

std::string region_from_title_id(const std::string& tid) {
    // PS3/legacy disc and network prefixes (PKG Viewer PS3_TID_REGION, extended for PS1/PS2).
    static const std::map<std::string, std::string> r{
        {"NPEB", "Europe"}, {"BCES", "Europe"}, {"BLES", "Europe"}, {"SLES", "Europe"}, {"SCES", "Europe"},
        {"NPHB", "Asia"}, {"BCAS", "Asia"}, {"BLAS", "Asia"},
        {"NPJB", "Japan"}, {"BCJS", "Japan"}, {"BLJS", "Japan"}, {"SLPS", "Japan"}, {"SCPS", "Japan"}, {"SLPM", "Japan"},
        {"NPUB", "Americas"}, {"BCUS", "Americas"}, {"BLUS", "Americas"}, {"SLUS", "Americas"}, {"SCUS", "Americas"}};
    if (tid.size() < 4) return "";
    auto it = r.find(tid.substr(0, 4));
    return it == r.end() ? "" : it->second;
}

PackageIdentity identify_package(const PackageReadAt& read, uint64_t size) {
    PackageIdentity id;
    std::vector<unsigned char> hdr;
    if (size < 0x80 || !read(0, 0x80, hdr) || hdr.size() != 0x80) { id.error = "File is too small to be a package."; return id; }
    if (hdr[0] != 0x7f) { id.error = "Not a PS4/PS5 package."; return id; }
    const std::string magic(hdr.begin() + 1, hdr.begin() + 4);
    if (magic == "CNT") return parse_cnt(read, size, 0, "PS4");
    if (magic == "FIH" || magic == "LIH") {
        const uint64_t emb = le64(hdr, magic == "FIH" ? 0x58 : 0x30);
        if (emb == 0 || emb >= size) { id.error = "PS5 container offset is out of range."; return id; }
        return parse_cnt(read, size, emb, "PS5");
    }
    id.error = "Unsupported package format.";
    return id;
}

PackageIdentity identify_package_file(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) { PackageIdentity id; id.error = "Package file could not be opened."; return id; }
    std::ifstream f(path, std::ios::binary);
    auto read = [&](uint64_t off, size_t len, std::vector<unsigned char>& out) {
        out.assign(len, 0);
        f.clear(); f.seekg(std::streamoff(off));
        f.read(reinterpret_cast<char*>(out.data()), std::streamsize(len));
        return size_t(f.gcount()) == len;
    };
    PackageIdentity id = identify_package(read, size);
    if (!id.ok) {
        PackageIdentity fallback = identify_from_filename(path.filename().string());
        if (fallback.ok) return fallback;
    }
    return id;
}

PackageIdentity identify_from_filename(const std::string& filename) {
    PackageIdentity id;
    std::smatch m;
    static const std::regex cid(R"(([A-Z]{2})(\d{4})-([A-Z]{4}\d{5})_(\d{2})-([A-Z0-9]{16}))", std::regex::icase);
    if (std::regex_search(filename, m, cid)) {
        id.content_id = m.str(0);
        std::transform(id.content_id.begin(), id.content_id.end(), id.content_id.begin(), [](unsigned char c) { return std::toupper(c); });
        id.title_id = id.content_id.substr(7, 9);
    } else {
        id.title_id = extract_title_id(filename);
    }
    if (id.title_id.empty()) { id.error = "No content ID or title ID in the filename."; return id; }
    if (id.title_id.starts_with("PPSA")) id.platform = "PS5";
    else if (id.title_id.starts_with("CUSA")) id.platform = "PS4";
    id.region = !id.content_id.empty() ? region_from_content_id(id.content_id) : region_from_title_id(id.title_id);
    id.source = IdentitySource::Filename;
    id.ok = true;
    return id;
}

} // namespace nexodion
