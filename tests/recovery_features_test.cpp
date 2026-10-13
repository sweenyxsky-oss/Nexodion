// Covers: startup helper/share checks with single-item retry, install readiness
// (space, readability, existing version), interrupted install review, event log
// rotation/export, damaged settings recovery, version comparison, share retry backoff.
#include "nexodion/startup_diagnostics.hpp"
#include "nexodion/native_services.hpp"
#include "nexodion/event_log.hpp"
#include "nexodion/library_state.hpp"
#include "nexodion/installer.hpp"
#include "nexodion/scanner.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace nexodion;
namespace fs = std::filesystem;

int main() {
    const fs::path root = fs::temp_directory_path() / "nexodion_recovery_test";
    fs::remove_all(root);
    fs::create_directories(root / "app0");

    // Startup: missing helper reported, then fixed and retried on its own.
    StartupDiagnosticsConfig cfg;
    cfg.mount_path = root.string();
    cfg.helper_dir = (root / "app0").string();
    cfg.cache_path = (root / "c.json").string();
    cfg.jobs_path = (root / "j.json").string();
    cfg.check_network = false;
    bool share_up = false;
    cfg.shares.push_back({"nas", "NAS", [&](std::string& d) { d = share_up ? "mounted" : "unreachable"; return share_up; }});
    StartupDiagnosticsEngine engine(cfg);
    auto report = engine.run_all_checks();
    DiagnosticItem helpers, share;
    for (const auto& i : report.items) { if (i.check_key == "helpers") helpers = i; if (i.check_key == "share:nas") share = i; }
    assert(helpers.is_failure() && share.is_warning());
    for (const auto& n : cfg.helper_names) std::ofstream(root / "app0" / n) << "ELF";
    share_up = true;
    auto fixed = engine.retry(helpers);
    assert(fixed.is_ok() && report.replace_item(fixed));
    assert(engine.retry(share).is_ok());

    // Versions.
    assert(compare_versions("01.020", "1.020") == 0);
    assert(compare_versions("01.000", "01.050") < 0);
    assert(compare_versions("2.00", "1.99") > 0);

    // Readiness: unreadable file blocks; installed same version is flagged, not blocking.
    const auto apps = root / "apps";
    fs::create_directories(apps / "CUSA00001" / "sce_sys");
    std::ofstream(apps / "CUSA00001" / "sce_sys" / "param.json") << R"({"contentVersion":"01.050"})";
    assert(installed_title_version(apps.string(), "CUSA00001") == "01.050");
    auto missing = check_install_readiness((root / "none.pkg").string(), apps.string(), "CUSA00001");
    assert(!missing.ok && !missing.checks[0].passed);
    std::ofstream(root / "bad.pkg") << "not a package";
    auto bad = check_install_readiness((root / "bad.pkg").string(), apps.string(), "CUSA00001");
    assert(!bad.ok && bad.already_installed);

    // Event log: rotation keeps bounded files and export copies them.
    EventLog log;
    log.set_directory((root / "logs").string());
    log.set_limits(1024, 2, 10);
    for (int i = 0; i < 200; ++i) log.record({0, "error", "install", "Install failed " + std::to_string(i), "why", "fix", ""});
    assert(log.recent(5).size() == 5 && log.recent(100).size() == 10);
    assert(log.recent(1)[0].code == "NX-INS-E");
    assert(fs::exists(root / "logs" / "nexodion.log.1") && !fs::exists(root / "logs" / "nexodion.log.3"));
    std::string err;
    assert(log.export_to((root / "export").string(), &err) >= 2);

    // Damaged settings: set aside and backup restored.
    const auto prefs = (root / "ui_state.json").string();
    LibraryState st; st.favorites.insert("CUSA00001|PS4");
    assert(st.save(prefs));
    assert(st.save(prefs)); // second save leaves a .bak of the first
    std::ofstream(prefs, std::ios::trunc) << "{broken";
    const auto note = recover_json_file(prefs);
    assert(!note.empty() && fs::exists(prefs + ".corrupt"));
    LibraryState back; assert(back.load(prefs) && back.favorites.count("CUSA00001|PS4"));

    // Interrupted install: saved in progress, reviewed after restart.
    const auto jobs = (root / "jobs.json").string();
    std::ofstream(jobs) << R"({"schema_version":2,"next_id":5,"tasks":[{"task_id":"task_4","title_id":"CUSA00001",
        "display_name":"Game","source_path":"/data/g.pkg","status":"InProgress","total_bytes":100,"processed_bytes":40}]})";
    {
        InstallPipeline p(nullptr, jobs);
        auto list = p.interrupted_tasks();
        assert(list.size() == 1 && list[0].processed_bytes == 40);
        assert(p.resolve_interrupted("task_4", true, "confirmed"));
        assert(p.interrupted_tasks().empty());
    }
    {
        InstallPipeline p(nullptr, jobs);
        assert(p.interrupted_tasks().empty()); // review survives restart
        assert(p.get_task("task_4")->status == InstallStatus::Completed);
    }

    // Share retry: transport fails twice, then connects on the third attempt.
    int calls = 0;
    ScanOptions so;
    so.root_path = "";
    ScanRootSpec spec; spec.path = "nfs://10.0.0.9/games"; spec.base_path = "/games"; spec.address.kind = FileTransportKind::Nfs; spec.address.host = "10.0.0.9";
    so.remote_roots.push_back(spec);
    so.connect_attempts = 3; so.connect_backoff_ms = 1;
    so.transport_factory = [&](const FileServerAddress&, std::string& e) -> std::unique_ptr<IFileTransport> { ++calls; e = "down"; return nullptr; };
    ScannerEngine scanner;
    scanner.scan(so, nullptr);
    assert(calls == 3);

    // Install queue: reorder, pause, one at a time; survives restart.
    {
        const auto qpath = (root / "queue.json").string();
        {
            InstallPipeline q(std::make_shared<MockInstallDelegate>(), qpath);
            std::vector<std::string> ids;
            for (const char* n : {"A", "B", "C"}) {
                GameItem it; it.filename = std::string(n) + ".pkg"; it.absolute_path = "/data/" + it.filename; it.display_name = n; it.size_bytes = 10;
                ids.push_back(q.queue_item(it, "CUSA0000" + std::string(n == std::string("A") ? "1" : n == std::string("B") ? "2" : "3")));
            }
            assert(q.move_queued(ids[2], -5)); // C to top
            assert(q.queued_tasks()[0].display_name == "C");
            assert(q.set_queued_paused(ids[2], true));
            q.set_one_at_a_time(true);
            assert(!q.process_one_step());      // waits for Continue Next
            assert(q.continue_next());
            assert(q.process_one_step());       // runs A (C is paused)
            assert(q.queued_tasks().size() == 2 && !q.process_one_step());
        }
        InstallPipeline q2(std::make_shared<MockInstallDelegate>(), qpath);
        auto left = q2.queued_tasks();
        assert(q2.one_at_a_time() && left.size() == 2 && left[0].display_name == "C" && left[0].paused);
        q2.set_one_at_a_time(false);
        assert(q2.process_one_step() && q2.queued_tasks().size() == 1 && q2.queued_tasks()[0].display_name == "C");
    }

    fs::remove_all(root);
    std::cout << "recovery_features_test passed\n";
    return 0;
}
