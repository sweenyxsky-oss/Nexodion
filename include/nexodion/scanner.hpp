#pragma once

#include "file_transport.hpp"
#include "game_metadata.hpp"

#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <memory>
#include <thread>
#include <mutex>
#include <map>
#include <filesystem>
#include <chrono>

namespace nexodion {

struct ScanOptions {
    std::string root_path{"/hostapp"};
    std::vector<std::string> roots; // Optional mounted sources; empty retains root_path.
    // Direct network roots (native file access, no FUSE mount required).
    std::vector<ScanRootSpec> remote_roots;
    struct RootIdentity {std::string source_id,storage_type;std::optional<NavTab> platform;};
    std::map<std::string,RootIdentity> root_identities;
    bool recursive{true};
    int max_depth{4};               // Maximum folder crawl depth to avoid slow NFS hangs
    bool calculate_directory_sizes{true};
    std::vector<std::string> supported_extensions{".pkg", ".exfat", ".ffpkg", ".ffpfs", ".ffpfsc"};
    // Local directory used to cache icon0.png artwork discovered on network
    // roots; empty disables remote artwork caching.
    std::string cover_cache_dir;
    // Test hook; when unset the scanner uses the real transport factory.
    TransportFactory transport_factory;
    // Network share connection retries: attempts and first wait (doubles each time).
    int connect_attempts{3};
    int connect_backoff_ms{500};
};

// Per-package identification outcome, shown in Mounting diagnostics.
struct PackageDiagnostic {
    std::string path;
    std::string source_id;
    std::string identity_source; // "file_contents", "filename_fallback" or "unidentified"
    std::string title_id;
    std::string title;
    std::string root;            // Scan root identity (local path or network URI)
    std::string reason;          // Why file contents could not be used; empty on success.
};

struct ScanStatistics {
    std::vector<PackageDiagnostic> packages;
    size_t packages_from_contents{0};
    size_t packages_from_filename{0};
    size_t packages_unidentified{0};
    size_t directories_scanned{0};
    size_t files_scanned{0};
    size_t games_found{0};
    size_t updates_found{0};
    size_t dlcs_found{0};
    uint64_t total_library_bytes{0};
    std::chrono::milliseconds elapsed_time{0};
};

using ScanProgressCallback = std::function<void(const std::string& current_item, size_t items_found, bool is_done)>;

class ScannerEngine {
public:
    ScannerEngine();
    ~ScannerEngine();

    // Non-copyable; move quiesces workers and produces a fresh idle scanner.
    ScannerEngine(const ScannerEngine&) = delete;
    ScannerEngine& operator=(const ScannerEngine&) = delete;
    ScannerEngine(ScannerEngine&&) noexcept;
    ScannerEngine& operator=(ScannerEngine&&) noexcept;

    // Synchronous scan
    std::vector<GameEntry> scan(const ScanOptions& options,
                                ScanProgressCallback progress = nullptr,
                                ScanStatistics* stats = nullptr);

    // Asynchronous scan on background thread
    void start_async_scan(const ScanOptions& options,
                          ScanProgressCallback progress,
                          std::function<void(std::vector<GameEntry> entries, ScanStatistics stats)> on_complete);

    void cancel();
    void stop(); // Cancel and join before owner state is destroyed.
    bool is_scanning() const;

    // Helper: checks if default /hostapp is available and accessible
    static bool is_default_hostapp_available(std::string* path_out = nullptr);

    // Folder browser helper: lists subdirectories at a given path for folder picker UI
    static std::vector<std::string> list_subdirectories(const std::string& path);

    // Fast calculation of directory size (skips nested sce_sys once counted)
    static uint64_t calculate_directory_size(const std::filesystem::path& dir, int max_depth = 8);

private:
    std::atomic<bool> cancel_requested_{false};
    std::atomic<bool> is_scanning_{false};
    std::thread worker_thread_;
    mutable std::mutex mutex_;
    std::string cover_cache_dir_;

    void scan_directory_internal(FileVfs& vfs,
                                 const std::string& dir,
                                 int current_depth,
                                 const ScanOptions& options,
                                 const std::string& root,
                                 const FileServerAddress& address,
                                 std::map<std::string, GameEntry>& entries_map,
                                 ScanStatistics& stats,
                                 const ScanProgressCallback& progress);

    void register_item(const std::string& item_path,
                       const ParamMetadata& meta,
                       const std::string& ext,
                       uint64_t size,
                       const std::string& root,
                       std::map<std::string, GameEntry>& entries_map,
                       ScanStatistics& stats,
                       const std::string& icon_path);

    std::string cache_remote_artwork(const FileServerAddress& address,
                                     const std::vector<unsigned char>& bytes,
                                     const std::string& title_id,
                                     const std::string& item_path);
};

} // namespace nexodion
