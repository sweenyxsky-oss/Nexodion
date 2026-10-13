// Covers cover-art extraction, sorting, favorites, recently played, saved
// preferences across a restart, storage usage, launch status and real installs.
#include "nexodion/ui_shell.hpp"
#include "nexodion/native_services.hpp"
#include "nexodion/package_identity.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace nexodion;
namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::cerr << "FAIL " << __LINE__ << ": " #c "\n"; ++failures; } } while (0)

static void be32(std::vector<unsigned char>& b, size_t o, uint32_t v) {
    b[o] = v >> 24; b[o + 1] = v >> 16; b[o + 2] = v >> 8; b[o + 3] = v;
}

static std::vector<unsigned char> make_pkg_with_icon(const std::vector<unsigned char>& png) {
    std::vector<unsigned char> b(0x200 + png.size(), 0);
    b[0] = 0x7f; b[1] = 'C'; b[2] = 'N'; b[3] = 'T';
    be32(b, 0x10, 1); be32(b, 0x18, 0x100);
    const char* cid = "EP0001-CUSA00001_00-ABCDEFGHIJKLMNOP";
    std::memcpy(&b[0x40], cid, std::strlen(cid));
    be32(b, 0x100, 0x1200); be32(b, 0x110, 0x200); be32(b, 0x114, uint32_t(png.size()));
    std::copy(png.begin(), png.end(), b.begin() + 0x200);
    return b;
}

static GameEntry entry(const std::string& id, const std::string& title, uint64_t size) {
    GameEntry e; e.title_id = id; e.title = title; e.platform = NavTab::PS4;
    e.total_size_bytes = size; e.confidence = MetadataConfidence::Verified; return e;
}

int main() {
    const fs::path dir = fs::temp_directory_path() / "nexodion_library_features";
    fs::remove_all(dir); fs::create_directories(dir);

    // Cover art from the package icon0.png entry.
    std::vector<unsigned char> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 1, 2, 3, 4};
    auto pkg = make_pkg_with_icon(png);
    auto read = [&](uint64_t off, size_t len, std::vector<unsigned char>& out) {
        if (off + len > pkg.size()) return false;
        out.assign(pkg.begin() + off, pkg.begin() + off + len); return true;
    };
    auto id = identify_package(read, pkg.size());
    CHECK(id.ok); CHECK(id.title_id == "CUSA00001"); CHECK(id.icon_png == png);
    auto bad = pkg; bad[0x200] = 0; // broken PNG signature: no icon, identity still fine
    auto read_bad = [&](uint64_t off, size_t len, std::vector<unsigned char>& out) {
        out.assign(bad.begin() + off, bad.begin() + off + len); return true; };
    auto id2 = identify_package(read_bad, bad.size());
    CHECK(id2.ok); CHECK(id2.icon_png.empty());

    // Card metadata line.
    auto e1 = entry("CUSA00001", "Bravo", 10); e1.region = "Europe";
    CHECK(card_metadata_line(e1) == "CUSA00001  Europe");
    e1.confidence = MetadataConfidence::Filename;
    CHECK(card_metadata_line(e1).find("Unconfirmed") != std::string::npos);

    const std::string state = (dir / "ui_state.json").string();
    {
        UiShell shell(nullptr, (dir / "cache.json").string());
        shell.set_ui_state_path(state);
        shell.select_tab(NavTab::PS4);
        shell.set_entries({entry("CUSA00001", "Bravo", 10), entry("CUSA00002", "Alpha", 30), entry("CUSA00003", "Charlie", 20)});
        CHECK(shell.filtered_entries().size() == 3);
        CHECK(shell.filtered_entries()[0].title == "Alpha");
        shell.set_library_sort(LibrarySort::Size);
        CHECK(shell.filtered_entries()[0].title == "Alpha" && shell.filtered_entries()[1].title == "Charlie");
        shell.record_played(shell.filtered_entries()[2], 1000); // Bravo
        shell.record_played(shell.filtered_entries()[1], 500);  // Charlie
        shell.set_library_sort(LibrarySort::RecentlyPlayed);
        CHECK(shell.filtered_entries()[0].title == "Bravo");
        CHECK(shell.filtered_entries()[1].title == "Charlie");
        shell.toggle_favorite(entry("CUSA00003", "Charlie", 20));
        CHECK(shell.filtered_entries()[0].title == "Charlie"); // favorites first
        auto cards = shell.get_current_view_cards();
        bool fav_card = false; for (auto& c : cards) if (c.title == "Charlie") fav_card = c.favorite;
        CHECK(fav_card);
        shell.toggle_view_layout();
    }
    {
        // Simulated restart: preferences come back from disk.
        UiShell shell(nullptr, (dir / "cache.json").string());
        shell.set_ui_state_path(state);
        CHECK(shell.library_sort() == LibrarySort::RecentlyPlayed);
        CHECK(shell.view_layout() == ViewLayout::BlockGrid);
        CHECK(shell.is_favorite(entry("CUSA00003", "Charlie", 20)));
        CHECK(shell.library_state().last_played.size() == 2);
        CHECK(shell.library_state().first_seen.size() == 3);

        // Storage usage for a local source.
        SourceConfig src; src.id = "internal"; src.display_name = "Internal"; src.mount_point = dir.string();
        CHECK(!shell.sources().add_source(src));
        auto rows = shell.query_storage_usage();
        CHECK(rows.size() == 1);
        CHECK(rows[0].usage.ok && rows[0].usage.total_bytes > 0);
    }

    // Launch status per format.
    GameEntry ps2 = entry("SLUS20001", "Legacy", 1); ps2.platform = NavTab::PS2;
    auto yes = [](const std::string&) { return true; };
    auto no = [](const std::string&) { return false; };
    auto c = evaluate_launch(ps2, no, true);
    CHECK(!c.can_launch && c.title == "PS5SX2 not installed");
    c = evaluate_launch(ps2, [](const std::string& id) { return id == "PPSA99203"; }, true);
    CHECK(c.can_launch && c.launch_title_id == "PPSA99203");
    GameEntry ps1 = entry("SCES00344", "Crash", 1); ps1.platform = NavTab::PS1;
    c = evaluate_launch(ps1, yes, true);
    CHECK(c.can_launch && c.launch_title_id == "PPSA98510");
    GameEntry ps3 = entry("BLES00001", "Ps3", 1); ps3.platform = NavTab::PS3;
    c = evaluate_launch(ps3, yes, true);
    CHECK(!c.can_launch && c.title == "PS3 launch unavailable");
    // Launcher settings: override, disable, validate, persist.
    {
        const std::string ls = (dir / "launchers.json").string();
        {
            UiShell shell(nullptr, (dir / "cache2.json").string());
            shell.set_ui_state_path(ls);
            CHECK(!shell.validate_launcher_for(NavTab::PS1).ok); // not installed on host
            CHECK(shell.validate_launcher_for(NavTab::PS3).status.find("not available") != std::string::npos);
            std::string e;
            CHECK(!shell.set_launcher(NavTab::PS3, "bad", &e) && !e.empty());
            CHECK(shell.set_launcher(NavTab::PS3, "ppsa12345"));
            CHECK(shell.set_launcher(NavTab::PS2, "none"));
            shell.show_launcher_settings();
            CHECK(shell.active_dialog() == DialogType::LauncherSettings);
            CHECK(shell.dialog_model().options[1] == "PS2: Disabled");
            shell.show_launcher_options(NavTab::PS1);
            CHECK(shell.dialog_model().options[0] == "Use SwanStationPS5");
        }
        UiShell shell(nullptr, (dir / "cache2.json").string());
        shell.set_ui_state_path(ls);
        CHECK(shell.library_state().launchers.at("PS3") == "PPSA12345");
        {
            // Searchable installed-app picker.
            auto app = [](const char* id, const char* name) { GameEntry e; e.title_id = id; e.title = name; e.installed = true; return e; };
            shell.set_installed_titles({app("PPSA99203", "PS5SX2"), app("PPSA98510", "SwanStationPS5"), app("CUSA00007", "Retro Classics")});
            CHECK(shell.installed_apps().size() == 3);
            CHECK(shell.installed_apps("swan").size() == 1);
            CHECK(shell.installed_apps("cusa00007").at(0).title == "Retro Classics");
            CHECK(shell.installed_apps("zzz").empty());
            shell.show_launcher_options(NavTab::PS1);
            shell.set_launcher_search("retro");
            shell.show_launcher_picker();
            CHECK(shell.active_dialog() == DialogType::LauncherPicker);
            CHECK(shell.dialog_model().options.size() == 4);
            CHECK(shell.dialog_model().options[2] == "> Retro Classics (CUSA00007)");
            CHECK(shell.validate_launcher_for(NavTab::PS1).ok); // SwanStationPS5 now confirmed installed
        }
        const LauncherOverrides o = shell.library_state().launchers;
        CHECK(evaluate_launch(ps3, yes, true, o).launch_title_id == "PPSA12345");
        CHECK(evaluate_launch(ps2, yes, true, o).title == "PS2 launch turned off");
        CHECK(validate_launcher(NavTab::PS3, o, yes).ok);
        CHECK(shell.set_launcher(NavTab::PS2, ""));
        CHECK(evaluate_launch(ps2, yes, true, shell.library_state().launchers).launch_title_id == "PPSA99203");
    }
    auto ps4 = entry("CUSA00001", "Game", 1);
    CHECK(evaluate_launch(ps4, no, true).title == "Not installed");
    CHECK(evaluate_launch(ps4, yes, false).title == "Console runtime required");
    c = evaluate_launch(ps4, yes, true);
    CHECK(c.can_launch && c.launch_title_id == "CUSA00001");
    CHECK(!explain_launch_failure("ELF loader unavailable on port 9021").steps.empty());
    std::string err;
    CHECK(!native_launch_title("CUSA00001", err)); // host never fakes a launch

    // Installer failure classification and install verification.
    CHECK(classify_installer_failure("fail 80b2116f refused").code == InstallErrorCode::InvalidPackage);
    CHECK(classify_installer_failure("fail 80990001 not enough space").code == InstallErrorCode::InsufficientSpace);
    CHECK(classify_installer_failure("fail 80990002 already installed").code == InstallErrorCode::DuplicateInstallBlocked);
    CHECK(classify_installer_failure("fail 80990003 weird").message.find("0x80990003") != std::string::npos);
    const auto apps = dir / "apps";
    CHECK(!verify_installed_title(apps.string(), "CUSA00001", err));
    fs::create_directories(apps / "CUSA00001");
    CHECK(!verify_installed_title(apps.string(), "CUSA00001", err)); // empty folder is not an install
    std::ofstream(apps / "CUSA00001" / "app.pkg") << "x";
    CHECK(verify_installed_title(apps.string(), "CUSA00001", err));
    CHECK(!verify_installed_title(apps.string(), "", err));

    // Host package delegate refuses rather than simulating success.
    NativePackageDelegate native; InstallTask t; t.source_path = "/data/a.pkg"; t.display_name = "Game";
    InstallErrorCode code; CHECK(!native.install_package(t, nullptr, code, err));

    // Local delegate performs a real, verified copy.
    const auto pkg_path = dir / "game.pkg";
    { std::ofstream f(pkg_path, std::ios::binary); for (int i = 0; i < 3 * 1024 * 1024 + 7; ++i) f.put(char(i)); }
    LocalInstallDelegate local((dir / "installed").string());
    InstallTask lt; lt.source_path = pkg_path.string(); lt.title_id = "CUSA00009";
    uint64_t last = 0;
    CHECK(local.install_package(lt, [&](uint64_t w, uint64_t) { last = w; return true; }, code, err));
    CHECK(fs::file_size(dir / "installed" / "CUSA00009" / "game.pkg") == fs::file_size(pkg_path));
    CHECK(last == fs::file_size(pkg_path));
    lt.source_path = (dir / "missing.pkg").string();
    CHECK(!local.install_package(lt, nullptr, code, err)); CHECK(code == InstallErrorCode::FileNotFound);
    lt.source_path = pkg_path.string(); lt.title_id = "CUSA00010";
    CHECK(!local.install_package(lt, [](uint64_t, uint64_t) { return false; }, code, err));
    CHECK(code == InstallErrorCode::Cancelled);
    CHECK(!fs::exists(dir / "installed" / "CUSA00010" / "game.pkg.part"));

    fs::remove_all(dir);
    if (failures) { std::cerr << failures << " checks failed\n"; return 1; }
    std::cout << "library_features_test passed\n";
    return 0;
}
