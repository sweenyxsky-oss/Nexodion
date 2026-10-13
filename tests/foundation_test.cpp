#include "nexodion/navigation.hpp"
#include "nexodion/source_manager.hpp"
#include "nexodion/theme.hpp"

#include <cassert>
#include <filesystem>
#include <iostream>

using namespace nexodion;

int main() {
    // Theme
    static_assert(theme::kObsidian.packed() == 0x090B12FF);
    static_assert(theme::kNexusViolet.r == 0x8B && theme::kNexusViolet.b == 0xF6);
    static_assert(theme::kModalOverlay.a == 0xCC);

    // Navigation
    NavigationBar nav;
    assert(nav.active() == NavTab::Installed);
    nav.prev();
    assert(nav.active() == NavTab::PS1);
    nav.next(); nav.next();
    assert(nav.active() == NavTab::PS5);
    for (int i = 0; i < 6; ++i) nav.next();
    assert(nav.active() == NavTab::PS5);
    assert(std::string(to_string(NavTab::PS3)) == "PS3");

    // Sources
    auto tmp = std::filesystem::temp_directory_path() / "nexodion_src_test";
    std::filesystem::create_directories(tmp);
    SourceManager sm(false);
    assert(!sm.add_source({"local", "Local", SourceKind::Local, "", "", tmp.string(), "", true,{}}));
    assert(sm.add_source({"local", "Dup", SourceKind::Local, "", "", tmp.string(), "", true,{}}));
    assert(sm.add_source({"bad", "Bad", SourceKind::Local, "", "", "relative", "", true,{}}));
    assert(sm.add_source({"nfs", "NFS", SourceKind::Nfs, "", "/export", "/mnt/nfs", "", true,{}}));
    assert(!sm.add_source({"nfs", "NFS", SourceKind::Nfs, "10.0.0.2", "/export", tmp.string(), "", true,{}}));
    assert(!sm.add_source({"smb", "SMB", SourceKind::Smb, "nas", "games", "/no/such/dir", "u", true,{}}));
    assert(sm.state_of("local") == SourceState::Mounted);
    // Network states are probed directly by SourceManager; the ShadowMountPlus
    // daemon no longer gates network browsing (milestone 6).
    assert(sm.state_of("nfs") == SourceState::Unreachable);
    assert(sm.active_scan_roots().size() == 1);  // remote roots scan via transports, not paths
    sm.set_daemon_unlocked(true);
    assert(sm.state_of("nfs") == SourceState::Unreachable);  // daemon unlock does not affect network probes
    assert(sm.state_of("smb") == SourceState::Unreachable);
    assert(sm.active_scan_roots().size() == 1);

    SourceManager copy(true);
    assert(copy.load_json(sm.to_json()));
    assert(copy.list().size() == 3);
    assert(!copy.load_json("{not json"));
    assert(!copy.load_json(R"({"sources":[{"id":"x","kind":"ftp","mount_point":"/a"}]})"));
    assert(copy.list().size() == 3);  // failed load leaves state intact
    assert(copy.remove_source("smb") && !copy.remove_source("smb"));

    std::filesystem::remove_all(tmp);
    std::cout << "foundation_test: all assertions passed\n";
    return 0;
}
