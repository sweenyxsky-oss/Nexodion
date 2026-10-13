#include <algorithm>
#include "nexodion/package_identity.hpp"
#include "nexodion/scanner.hpp"
#include "nexodion/ui_shell.hpp"
#include <filesystem>
#include <thread>
#include <chrono>
#include <nlohmann/json.hpp>
#include <cstring>
#include <fstream>
#include <iostream>

using namespace nexodion;

// Every check is recorded (not aborted on) so a console run produces a complete
// report file that can be reviewed afterwards.
static nlohmann::json g_checks = nlohmann::json::array();
static nlohmann::json g_identities = nlohmann::json::array();
static int g_failed = 0;
static void check(const char* expr, bool ok, int line) {
    g_checks.push_back({{"check", expr}, {"line", line}, {"passed", ok}});
    if (!ok) { ++g_failed; std::cerr << "FAILED line " << line << ": " << expr << "\n"; }
}
#define CHECK(x) check(#x, static_cast<bool>(x), __LINE__)
static void record(const std::string& name, const PackageIdentity& id) {
    g_identities.push_back({{"case", name}, {"ok", id.ok}, {"source", to_string(id.source)}, {"title", id.title},
        {"title_id", id.title_id}, {"content_id", id.content_id}, {"platform", id.platform},
        {"region", id.region}, {"category", id.category}, {"error", id.error}});
}

static std::string default_report_path() {
    std::error_code ec;
    if (std::filesystem::is_directory("/data", ec)) return "/data/nexodion/diagnostics/package_identity_report.json";
    return (std::filesystem::temp_directory_path() / "package_identity_report.json").string();
}

static void put32(std::vector<unsigned char>& b, size_t o, uint32_t v) {
    b[o] = v >> 24; b[o + 1] = v >> 16; b[o + 2] = v >> 8; b[o + 3] = v;
}

int main(int argc, char** argv) {
    const std::string report_path = argc > 1 ? argv[1] : default_report_path();
    // Synthetic PS4 CNT: header, 2 entries (name table + param.sfo).
    std::vector<unsigned char> pkg(0x400, 0);
    pkg[0] = 0x7f; pkg[1] = 'C'; pkg[2] = 'N'; pkg[3] = 'T';
    put32(pkg, 0x10, 2); put32(pkg, 0x18, 0x100);
    const char* cid = "EP9000-CUSA00001_00-ABCDEFGHIJKLMNOP";
    std::memcpy(&pkg[0x40], cid, std::strlen(cid));
    const char names[] = "\0param.sfo\0";
    std::memcpy(&pkg[0x200], names, sizeof(names));
    put32(pkg, 0x100, 512); put32(pkg, 0x110, 0x200); put32(pkg, 0x114, sizeof(names));
    // SFO with TITLE_ID, TITLE, CATEGORY
    std::vector<unsigned char> sfo(0x100, 0);
    sfo[1] = 'P'; sfo[2] = 'S'; sfo[3] = 'F';
    auto le = [&](size_t o, uint32_t v, int n) { for (int i = 0; i < n; ++i) sfo[o + i] = (v >> (8 * i)) & 0xff; };
    const char keys[] = "CATEGORY\0TITLE\0TITLE_ID\0";
    const size_t koff = 20 + 3 * 16, doff = koff + sizeof(keys);
    le(8, koff, 4); le(12, doff, 4); le(16, 3, 4);
    std::memcpy(&sfo[koff], keys, sizeof(keys));
    const char* vals[] = {"gd", "Test Game", "CUSA00001"}; uint16_t ko[] = {0, 9, 15}; uint32_t vo = 0;
    for (int i = 0; i < 3; ++i) {
        size_t p = 20 + i * 16, len = std::strlen(vals[i]) + 1;
        le(p, ko[i], 2); le(p + 2, 0x0204, 2); le(p + 4, len, 4); le(p + 8, 16, 4); le(p + 12, vo, 4);
        std::memcpy(&sfo[doff + vo], vals[i], len); vo += 16;
    }
    std::memcpy(&pkg[0x300], sfo.data(), sfo.size());
    put32(pkg, 0x120, 4096); put32(pkg, 0x124, 1); put32(pkg, 0x130, 0x300); put32(pkg, 0x134, sfo.size());

    auto read = [&](uint64_t off, size_t len, std::vector<unsigned char>& out) {
        if (off + len > pkg.size()) return false;
        out.assign(pkg.begin() + off, pkg.begin() + off + len); return true;
    };
    auto id = identify_package(read, pkg.size());
    record("synthetic_ps4_cnt", id);
    CHECK(id.ok && id.source == IdentitySource::PackageHeader);
    CHECK(id.title == "Test Game" && id.title_id == "CUSA00001");
    CHECK(id.platform == "PS4" && id.region == "Europe" && id.content_id == cid);

    // Corrupt entry table offset must fail safely, not read out of range.
    auto bad = pkg; put32(bad, 0x18, 0xFFFFFF00);
    auto badread = [&](uint64_t off, size_t len, std::vector<unsigned char>& out) {
        if (off + len > bad.size()) return false;
        out.assign(bad.begin() + off, bad.begin() + off + len); return true;
    };
    auto badid = identify_package(badread, bad.size());
    record("corrupt_entry_table", badid);
    CHECK(!badid.ok);

    auto f = identify_from_filename("UP0001-PPSA01234_00-GAMEGAMEGAMEGAME-A0100-V0100.pkg");
    record("ps5_content_id_filename", f);
    CHECK(f.ok && f.source == IdentitySource::Filename && f.platform == "PS5" && f.region == "Americas" && f.title_id == "PPSA01234");
    auto legacy = identify_from_filename("Crash (SCES-00344).bin");
    record("legacy_serial_filename", legacy);
    CHECK(legacy.ok && legacy.title_id == "SCES-00344" && legacy.region == "Europe" && legacy.platform.empty());
    CHECK(!identify_from_filename("random.pkg").ok);

    // Automatic identification during a scan of a mounted local path.
    const auto dir = std::filesystem::temp_directory_path() / "nexodion_pkgid_scan";
    std::filesystem::remove_all(dir); std::filesystem::create_directories(dir);
    { std::ofstream o(dir / "unnamed.pkg", std::ios::binary); o.write(reinterpret_cast<const char*>(pkg.data()), pkg.size()); }
    ScannerEngine scanner; ScanOptions options; options.root_path = dir.string();
    { std::ofstream o(dir / "UP0001-PPSA01234_00-GAMEGAMEGAMEGAME.pkg", std::ios::binary); o << "not a package"; }
    ScanStatistics stats;
    auto entries = scanner.scan(options, nullptr, &stats);
    CHECK(stats.packages_from_contents == 1 && stats.packages_from_filename == 1 && stats.packages.size() == 2);
    for (const auto& d : stats.packages)
        if (d.identity_source == "filename_fallback") CHECK(!d.reason.empty() && d.title_id == "PPSA01234");
    std::erase_if(entries, [](const GameEntry& e) { return e.title_id != "CUSA00001"; });
    CHECK(entries.size() == 1);
    if (!entries.empty()) CHECK(entries[0].title == "Test Game" && entries[0].title_id == "CUSA00001");
    if (!entries.empty()) CHECK(entries[0].region == "Europe" && entries[0].content_id == cid);
    std::filesystem::remove_all(dir);

    // Diagnostics filters and retry of unreadable packages after the share is fixed.
    {
        const auto share = std::filesystem::temp_directory_path() / "nexodion_retry_share";
        std::filesystem::remove_all(share); std::filesystem::create_directories(share / "PS4");
        { std::ofstream o(share / "PS4" / "broken.pkg", std::ios::binary); o << "truncated"; }
        UiShell shell(nullptr, (std::filesystem::temp_directory_path() / "nx_retry_cache.json").string());
        CHECK(!shell.sources().add_source({"share", "Share", SourceKind::Local, "", "", share.string(), "", true,
                                           {{NavTab::PS4, (share / "PS4").string()}}}));
        auto wait = [&] { for (int i = 0; i < 500 && shell.is_scanning(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10)); shell.tick(); };
        shell.trigger_scan(true); wait();
        const auto& st = shell.last_scan_statistics();
        CHECK(st.packages.size() == 1 && st.packages_unidentified == 1 && !st.packages[0].reason.empty());
        g_identities.push_back({{"case", "retry_before_fix"}, {"reason", st.packages.empty() ? "" : st.packages[0].reason}});
        shell.show_mounting_diagnostics();
        CHECK(shell.dialog_model().options[0] == "Show: All packages");
        CHECK(shell.dialog_model().options[3] == "Retry Unreadable Packages (1)");
        // Replace the broken file with a valid package, then retry.
        { std::ofstream o(share / "PS4" / "broken.pkg", std::ios::binary | std::ios::trunc); o.write(reinterpret_cast<const char*>(pkg.data()), pkg.size()); }
        // Single-package retry: only this file is re-read, no share rescan.
        CHECK(shell.retry_package(0));
        for (int i = 0; i < 500 && shell.is_retrying_package(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); shell.tick(); }
        CHECK(!shell.is_scanning());
        CHECK(shell.last_scan_statistics().packages[0].identity_source == "file_contents");
        CHECK(shell.last_scan_statistics().packages[0].reason.empty());
        CHECK(shell.all_entries().size() == 1 && shell.all_entries()[0].title_id == "CUSA00001" && shell.all_entries()[0].region == "Europe");
        CHECK(!shell.retry_package(0)); // nothing left to retry
        // Search by filename, title and title ID.
        auto rows = [&](const std::string& q) { shell.set_diagnostics_search(q); shell.show_mounting_diagnostics();
            return std::count_if(shell.dialog_model().options.begin(), shell.dialog_model().options.end(), [](const std::string& o) { return o.starts_with("- "); }); };
        CHECK(rows("BROKEN") == 1); CHECK(rows("test game") == 1); CHECK(rows("cusa00001") == 1); CHECK(rows("missing") == 0); CHECK(rows("") == 1);
        // Export the currently filtered diagnostics as a shareable report.
        shell.show_mounting_diagnostics();
        CHECK(shell.dialog_model().options[5] == "Export Filtered Report");
        const auto export_path = std::filesystem::temp_directory_path() / "nexodion_filtered_report.json";
        std::filesystem::remove(export_path);
        auto export_of = [&](const std::string& q, UiShell::DiagFilter filter, const std::string& share) {
            shell.set_diagnostics_search(q);
            shell.set_diagnostics_filter(filter);
            shell.set_diagnostics_share(share);
            std::string error;
            const bool ok = shell.export_filtered_diagnostics(export_path.string(), &error);
            nlohmann::json ex;
            if (ok) { std::ifstream in(export_path); in >> ex; }
            return nlohmann::json{{"ok", ok}, {"error", error}, {"report", ok ? ex : nlohmann::json()}};
        };
        auto ex1 = export_of("BROKEN", UiShell::DiagFilter::All, "");
        CHECK(ex1["ok"] == true);
        CHECK(ex1["report"]["filters"]["search"] == "BROKEN" && ex1["report"]["filters"]["show"] == "all");
        CHECK(ex1["report"]["matched"] == 1 && ex1["report"]["total_packages"] == 1);
        CHECK(ex1["report"]["packages"][0]["identity_source"] == "file_contents");
        CHECK(std::string(ex1["report"]["packages"][0]["path"]).ends_with("broken.pkg"));
        auto ex2 = export_of("missing", UiShell::DiagFilter::All, "");
        CHECK(ex2["ok"] == true && ex2["report"]["matched"] == 0 && ex2["report"]["packages"].empty());
        auto ex3 = export_of("", UiShell::DiagFilter::Unreadable, "");
        CHECK(ex3["ok"] == true && ex3["report"]["matched"] == 0 && ex3["report"]["filters"]["show"] == "unreadable");
        auto ex4 = export_of("", UiShell::DiagFilter::All, "share");
        CHECK(ex4["ok"] == true && ex4["report"]["matched"] == 1 && ex4["report"]["filters"]["share"] == "share");
        auto ex5 = export_of("", UiShell::DiagFilter::All, "other");
        CHECK(ex5["ok"] == true && ex5["report"]["matched"] == 0);
        std::filesystem::remove(export_path);
        // Break it again, then use the share-level retry.
        { std::ofstream o(share / "PS4" / "broken.pkg", std::ios::binary | std::ios::trunc); o << "x"; }
        shell.trigger_scan(true); wait();
        { std::ofstream o(share / "PS4" / "broken.pkg", std::ios::binary | std::ios::trunc); o.write(reinterpret_cast<const char*>(pkg.data()), pkg.size()); }
        CHECK(shell.retry_unreadable_packages()); wait();
        CHECK(shell.last_scan_statistics().packages_from_contents == 1 && shell.last_scan_statistics().packages_unidentified == 0);
        CHECK(!shell.retry_unreadable_packages());
        std::filesystem::remove_all(share);
    }

    // Same title on two shares appears once and lists both copies.
    {
        UiShell shell(nullptr, (std::filesystem::temp_directory_path() / "nx_pkgid_cache.json").string());
        shell.select_tab(NavTab::PS4);
        GameEntry a; a.title_id = "CUSA00001"; a.title = "Test Game"; a.source_id = "nas"; a.storage_type = "nfs"; a.platform = NavTab::PS4;
        GameEntry b = a; b.source_id = "internal"; b.storage_type = "local";
        shell.set_entries({a, b});
        CHECK(shell.filtered_entries().size() == 1);
        if (!shell.filtered_entries().empty()) CHECK(shell.filtered_entries()[0].copies.size() == 2);
        CHECK(shell.all_entries().size() == 2);
    }

    nlohmann::json report;
    report["test"] = "package_identity_test";
    report["generated_at_unix"] = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
#if defined(__PROSPERO__) || defined(PS5)
    report["target"] = "ps5";
#else
    report["target"] = "host";
#endif
    report["passed"] = g_failed == 0;
    report["failed_checks"] = g_failed;
    report["total_checks"] = g_checks.size();
    report["checks"] = g_checks;
    report["identities"] = g_identities;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(report_path).parent_path(), ec);
    std::ofstream out(report_path, std::ios::trunc);
    if (out) { out << report.dump(2) << "\n"; std::cout << "Report saved to " << report_path << "\n"; }
    else std::cerr << "Could not write report to " << report_path << "\n";

    if (g_failed) { std::cout << "package_identity_test FAILED (" << g_failed << " checks)\n"; return 1; }
    std::cout << "package_identity_test passed\n";
}
