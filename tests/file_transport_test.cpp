// Milestone 6: native network file access tests.
// Covers the local transport, fake remote transport scans, URI identities,
// per-platform root isolation and secret persistence exclusion.
#include "nexodion/file_transport.hpp"
#include "nexodion/scanner.hpp"
#include "nexodion/source_manager.hpp"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>

using namespace nexodion;

namespace {

class FakeTransport final : public IFileTransport {
public:
    std::map<std::string, std::vector<RemoteFileEntry>> listing;
    std::map<std::string, std::vector<unsigned char>> files;

    bool list_directory(const std::string& path, std::vector<RemoteFileEntry>& out,
                        std::string&) override {
        const auto it = listing.find(path);
        if (it == listing.end()) return false;
        out = it->second;
        return true;
    }

    bool read_file(const std::string& path, size_t max_bytes, std::vector<unsigned char>& out,
                   std::string&) override {
        const auto it = files.find(path);
        if (it == files.end()) return false;
        if (it->second.size() > max_bytes) return false;
        out = it->second;
        return true;
    }

    bool read_range(const std::string& path, uint64_t offset, size_t len,
                    std::vector<unsigned char>& out, std::string&) override {
        const auto it = files.find(path);
        if (it == files.end() || offset > it->second.size() || len > it->second.size() - offset) return false;
        out.assign(it->second.begin() + offset, it->second.begin() + offset + len);
        return true;
    }

    bool stat_path(const std::string& path, RemoteFileEntry& out, std::string&) override {
        if (listing.count(path)) {
            out.name = path;
            out.is_directory = true;
            return true;
        }
        const auto it = files.find(path);
        if (it == files.end()) return false;
        out.name = path;
        out.is_regular_file = true;
        out.size = it->second.size();
        return true;
    }
    const char* kind_name() const override { return "fake"; }
};

std::string json_for(const std::string& title_id, const std::string& title) {
    return std::string(R"({"titleId":")") + title_id + R"(","titleName":")" + title +
                       R"(","category":"gd"})";
}

} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / "nexodion_transport_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "local" / "game" / "sce_sys");
    {
        std::ofstream f(root / "local" / "game" / "sce_sys" / "param.json");
        f << json_for("PPSA90001", "Local Title");
    }

    // 1. Local transport
    {
        auto transport = make_local_transport();
        std::vector<RemoteFileEntry> out;
        std::string error;
        assert(transport->list_directory((root / "local").string(), out, error));
        assert(out.size() == 1 && out[0].name == "game");
        std::vector<unsigned char> bytes;
        assert(transport->read_file((root / "local" / "game" / "sce_sys" / "param.json").string(),
                                    1024 * 1024, bytes, error));
        assert(!bytes.empty());
    }

    // 2. URI helpers and identities
    {
        assert(is_remote_root_path("nfs://nas/PS5"));
        assert(is_remote_root_path("smb://nas/games"));
        assert(!is_remote_root_path("/data/nexodion/libraries"));
        FileServerAddress nfs;
        nfs.kind = FileTransportKind::Nfs;
        nfs.host = "10.0.0.5";
        nfs.remote_path = "/export";
        const auto uri = make_network_root_uri(nfs, "/PS5");
        assert(uri == "nfs://10.0.0.5/export/PS5");
        FileServerAddress smb;
        smb.kind = FileTransportKind::Smb;
        smb.host = "nas";
        smb.remote_path = "games";
        assert(make_network_root_uri(smb, "/PS1") == "smb://nas/games/PS1");
    }

    // 3. Remote scan through a fake transport: PS1 tab only sees /PS1 content
    {
        FakeTransport fake;
        const auto json_bytes = json_for("SLUS-90001", "Remote Title");
        fake.listing = {
            {"/PS1", {{"Games", true, false, 0}}},
            {"/PS1/Games", {{"PS1GAME", true, false, 0}}},
            {"/PS1/Games/PS1GAME", {{"sce_sys", true, false, 0}}},
        };
        fake.files = {
            {"/PS1/Games/PS1GAME/sce_sys/param.json",
             std::vector<unsigned char>(json_bytes.begin(), json_bytes.end())},
        };
        ScanOptions options;
        FileServerAddress address;
        address.kind = FileTransportKind::Nfs;
        address.host = "10.0.0.5";
        address.remote_path = "/export";
        ScanRootSpec spec;
        spec.path = "nfs://10.0.0.5/export/PS1";
        spec.base_path = "/PS1";
        spec.address = address;
        spec.platform = NavTab::PS1;
        spec.source_id = "nas";
        spec.storage_type = "nfs";
        options.remote_roots.push_back(spec);
        options.root_identities[spec.path] = {"nas", "nfs", NavTab::PS1};
        options.transport_factory = [&fake](const FileServerAddress&, std::string&) {
            return std::unique_ptr<IFileTransport>(new FakeTransport(fake));
        };
        ScannerEngine scanner;
        auto entries = scanner.scan(options);
        assert(entries.size() == 1);
        assert(entries[0].title_id == "SLUS-90001");
        assert(entries[0].title == "Remote Title");
        assert(entries[0].source_id == "nas");
        assert(entries[0].storage_type == "nfs");
        assert(entries[0].platform.has_value() && *entries[0].platform == NavTab::PS1);
        assert(!entries[0].source_root.empty());
    }

    // 4. Unreachable network endpoints produce errors, never fabricated success
    {
        FileServerAddress smb;
        smb.kind = FileTransportKind::Smb;
        smb.host = "127.0.0.1";
        smb.remote_path = "share";
        smb.timeout_ms = 1;
        std::string error;
        const auto transport = make_file_transport(smb, error);
        assert(transport == nullptr);
        assert(!error.empty());
    }

    // 5. Session secrets live in memory only
    {
        SourceManager manager(false);
        assert(!manager.add_source({"nas", "NAS", SourceKind::Smb, "nas", "games", "/mnt", "user", true, {}}));
        manager.set_session_secret("nas", "hunter2");
        assert(manager.session_secret("nas") == "hunter2");
        assert(manager.to_json().find("hunter2") == std::string::npos);
        SourceManager reloaded(false);
        assert(reloaded.load_json(manager.to_json()));
        assert(reloaded.session_secret("nas").empty());  // secret not persisted
        reloaded.set_session_secret("nas", "");
        assert(reloaded.session_secret("nas").empty());
    }

    // 6. Per-platform scan specs from a network source
    {
        SourceManager manager(false);
        SourceConfig config;
        config.id = "nas";
        config.kind = SourceKind::Nfs;
        config.host = "10.0.0.5";
        config.remote_path = "/export";
        config.mount_point = "/mnt/nas";
        config.platform_paths[NavTab::PS1] = "/PS1";
        config.platform_paths[NavTab::PS5] = "/PS5";
        assert(!manager.add_source(config));
        const auto specs = manager.active_scan_specs();
        assert(specs.size() == 2);
        bool saw_ps1 = false, saw_ps5 = false;
        for (const auto& spec : specs) {
            assert(spec.address.kind == FileTransportKind::Nfs);
            assert(spec.address.host == "10.0.0.5");
            if (spec.platform == NavTab::PS1) {
                saw_ps1 = true;
                assert(spec.base_path == "/PS1");
                assert(spec.path == "nfs://10.0.0.5/export/PS1");
            }
            if (spec.platform == NavTab::PS5) {
                saw_ps5 = true;
                assert(spec.base_path == "/PS5");
            }
        }
        assert(saw_ps1 && saw_ps5);
    }

    std::filesystem::remove_all(root);
    std::cout << "file_transport_test: all assertions passed\n";
    return 0;
}
