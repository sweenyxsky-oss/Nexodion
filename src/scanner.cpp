#include <chrono>
#include <thread>
#include "nexodion/package_identity.hpp"
#include "nexodion/scanner.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <system_error>

namespace nexodion {

namespace {

// Bounded read limits shared by all transports.
constexpr size_t kParamReadLimit = 1024 * 1024;   // param.json / param.sfo
constexpr size_t kIconReadLimit = 8u * 1024 * 1024; // icon0.png

} // namespace

ScannerEngine::ScannerEngine() = default;

ScannerEngine::~ScannerEngine() {
    cancel();
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

ScannerEngine::ScannerEngine(ScannerEngine&& other) noexcept {
    other.cancel();
    if (other.worker_thread_.joinable()) {
        other.worker_thread_.join();
    }
    cancel_requested_.store(false);
    is_scanning_.store(false);
}

ScannerEngine& ScannerEngine::operator=(ScannerEngine&& other) noexcept {
    if (this != &other) {
        cancel();
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }
        other.cancel();
        if (other.worker_thread_.joinable()) {
            other.worker_thread_.join();
        }
        cancel_requested_.store(false);
        is_scanning_.store(false);
    }
    return *this;
}

void ScannerEngine::cancel() {
    cancel_requested_.store(true);
}

void ScannerEngine::stop() {
    cancel();
    if (worker_thread_.joinable()) worker_thread_.join();
}

bool ScannerEngine::is_scanning() const {
    return is_scanning_.load();
}

bool ScannerEngine::is_default_hostapp_available(std::string* path_out) {
    std::error_code ec;
    const std::string hostapp_path = "/hostapp";
    if (std::filesystem::exists(hostapp_path, ec) && std::filesystem::is_directory(hostapp_path, ec)) {
        if (path_out) *path_out = hostapp_path;
        return true;
    }
    return false;
}

std::vector<std::string> ScannerEngine::list_subdirectories(const std::string& path) {
    std::vector<std::string> dirs;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || !std::filesystem::is_directory(path, ec)) {
        return dirs;
    }

    for (const auto& entry : std::filesystem::directory_iterator(path, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (ec) break;
        if (entry.is_directory(ec)) {
            dirs.push_back(entry.path().string());
        }
    }
    std::sort(dirs.begin(), dirs.end());
    return dirs;
}

uint64_t ScannerEngine::calculate_directory_size(const std::filesystem::path& dir, int max_depth) {
    uint64_t total = 0;
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) {
        return 0;
    }

    try {
        auto it = std::filesystem::recursive_directory_iterator(
            dir, std::filesystem::directory_options::skip_permission_denied, ec);
        auto end = std::filesystem::recursive_directory_iterator();

        while (it != end && !ec) {
            if (it.depth() > max_depth) {
                it.pop();
                continue;
            }
            if (it->is_regular_file(ec)) {
                total += it->file_size(ec);
            }
            it.increment(ec);
        }
    } catch (...) {
        // Fallback gracefully on traversal errors
    }
    return total;
}

std::string ScannerEngine::cache_remote_artwork(const FileServerAddress& address,
                                                const std::vector<unsigned char>& bytes,
                                                const std::string& title_id,
                                                const std::string& item_path) {
    // Network artwork must land in a local cache file: the renderer expects a
    // real path. Writes happen here, on the scanner worker, never on the UI
    // thread, and failure only means missing artwork.
    std::error_code ec;
    std::filesystem::create_directories(cover_cache_dir_, ec);
    if (ec) return {};

    std::string base = !title_id.empty() ? title_id : std::filesystem::path(item_path).filename().string();
    std::string sanitized;
    for (unsigned char ch : base) {
        sanitized.push_back(std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.' ? static_cast<char>(ch) : '_');
    }
    if (sanitized.empty()) sanitized = "title";
    // Include transport identity so two sources with identical title IDs keep
    // separate cached artwork instead of silently overwriting each other.
    std::string scope;
    if (address.kind == FileTransportKind::Nfs) scope = "nfs_" + address.host;
    else if (address.kind == FileTransportKind::Smb) scope = "smb_" + address.host;
    if (!scope.empty()) sanitized = scope + "_" + sanitized;

    const std::string cache_path = (std::filesystem::path(cover_cache_dir_) / (sanitized + ".png")).string();
    std::ofstream out(cache_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return {};
    if (!bytes.empty()) out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) return {};
    return cache_path;
}

void ScannerEngine::register_item(const std::string& item_path,
                                  const ParamMetadata& meta,
                                  const std::string& ext,
                                  uint64_t size,
                                  const std::string& root,
                                  std::map<std::string, GameEntry>& entries_map,
                                  ScanStatistics& stats,
                                  const std::string& icon_path) {
    // Lexical relative path (works for both filesystem paths and network URIs).
    std::string rel_path = item_path;
    if (!root.empty() && rel_path.starts_with(root)) {
        rel_path = rel_path.substr(root.size());
    }
    while (!rel_path.empty() && rel_path.front() == '/') rel_path.erase(rel_path.begin());
    if (rel_path.empty()) {
        const auto slash = item_path.find_last_of('/');
        rel_path = slash == std::string::npos ? item_path : item_path.substr(slash + 1);
    }

    const auto stem_pos = item_path.find_last_of('/');
    std::string stem = stem_pos == std::string::npos ? item_path : item_path.substr(stem_pos + 1);
    // Match the previous std::filesystem::stem() behavior: drop the extension.
    const auto dot = stem.find_last_of('.');
    if (dot != std::string::npos && dot != 0) stem.erase(dot);

    const std::string key = root + "@" + (!meta.title_id.empty() ? meta.title_id : stem);

    auto& entry = entries_map[key];
    entry.source_root = root;
    if (entry.title_id.empty() && !meta.title_id.empty()) {
        entry.title_id = meta.title_id;
    }

    if (entry.title.empty() || (meta.confidence == MetadataConfidence::Verified && entry.confidence != MetadataConfidence::Verified)) {
        entry.title = !meta.title_name.empty() ? meta.title_name : stem;
        entry.confidence = meta.confidence;
    }

    if (entry.content_id.empty()) entry.content_id = meta.content_id;
    if (entry.region.empty()) entry.region = meta.region;

    if (entry.cover_path.empty() && !icon_path.empty()) {
        entry.cover_path = icon_path;
    }

    GameItem item;
    item.kind = meta.kind;
    item.filename = stem;
    item.relative_path = rel_path;
    item.absolute_path = item_path;
    item.format = ext.empty() ? "folder" : (ext[0] == '.' ? ext.substr(1) : ext);
    item.version = meta.version;
    item.size_bytes = size;
    item.size_known = true;
    item.formatted_size = format_file_size(size);

    if (meta.kind == ItemKind::Update) {
        item.display_name = !meta.version.empty() ? ("Update v" + meta.version) : item.filename;
        entry.updates.push_back(std::move(item));
        stats.updates_found++;
    } else if (meta.kind == ItemKind::Dlc) {
        item.display_name = !meta.title_name.empty() ? meta.title_name : item.filename;
        entry.dlcs.push_back(std::move(item));
        stats.dlcs_found++;
    } else {
        // Default base game
        item.display_name = !meta.title_name.empty() ? meta.title_name : entry.title;
        if (!entry.base_game.has_value()) {
            entry.base_game = std::move(item);
            entry.primary_format = ext.empty() ? "folder" : (ext[0] == '.' ? ext.substr(1) : ext);
            stats.games_found++;
        } else {
            // Additional package under same title
            entry.dlcs.push_back(std::move(item));
            stats.dlcs_found++;
        }
    }
}

void ScannerEngine::scan_directory_internal(FileVfs& vfs,
                                           const std::string& dir,
                                           int current_depth,
                                           const ScanOptions& options,
                                           const std::string& root,
                                           const FileServerAddress& address,
                                           std::map<std::string, GameEntry>& entries_map,
                                           ScanStatistics& stats,
                                           const ScanProgressCallback& progress) {
    if (cancel_requested_.load()) return;

    std::string error;
    if (!vfs.directory_exists(dir, error)) {
        return;
    }

    // Artwork resolver: local icons keep their real path; network icons are
    // copied once into the local cover cache (bounded read) so the renderer
    // can display them.
    auto resolve_icon = [&](const std::string& parent_dir, const std::string& title_id) -> std::string {
        const std::string icon = FileVfs::join(parent_dir, "icon0.png");
        RemoteFileEntry icon_stat;
        if (!vfs.regular_file_size(icon, icon_stat.size, error)) return {};
        if (!vfs.is_remote()) return icon;
        if (options.cover_cache_dir.empty()) return {};
        std::vector<unsigned char> icon_bytes;
        if (!vfs.read_file(icon, kIconReadLimit, icon_bytes, error)) return {};
        return cache_remote_artwork(address, icon_bytes, title_id, parent_dir);
    };

    stats.directories_scanned++;

    // 1. Check if this directory itself is an unpacked game folder
    //    (contains sce_sys/param.json or sce_sys/param.sfo). Reads are
    //    bounded; a missing metadata file is an ordinary miss, not an error.
    const std::string param_sfo = FileVfs::join(dir, "sce_sys/param.sfo");
    const std::string param_json = FileVfs::join(dir, "sce_sys/param.json");
    std::vector<unsigned char> bytes;
    bool is_sfo = false;
    if (!vfs.read_file(param_sfo, kParamReadLimit, bytes, error)) {
        bytes.clear();
        if (!vfs.read_file(param_json, kParamReadLimit, bytes, error)) bytes.clear();
    } else {
        is_sfo = true;
    }

    if (!bytes.empty()) {
        ParamMetadata meta = is_sfo ? parse_param_sfo_bytes(bytes)
                                    : parse_param_json_string(std::string(bytes.begin(), bytes.end()));
        if (meta.valid) {
            const uint64_t dir_size =
                options.calculate_directory_sizes
                    ? vfs.directory_size(dir, 8, [this] { return cancel_requested_.load(); })
                    : 0;
            // Artwork: sibling icon0.png next to the metadata file.
            const std::string icon_path = resolve_icon(dir + "/sce_sys", meta.title_id);
            register_item(dir, meta, "folder", dir_size, root, entries_map, stats, icon_path);

            if (progress) {
                progress(dir, entries_map.size(), false);
            }
            // Do not recurse into game binaries/assets
            return;
        }
    }

    // 2. Iterate entries in the directory
    std::vector<RemoteFileEntry> entries;
    if (!vfs.list_directory(dir, entries, error)) return;

    for (const auto& entry : entries) {
        if (cancel_requested_.load()) break;

        const std::string child = FileVfs::join(dir, entry.name);

        if (entry.is_directory) {
            // Check if subdirectory is a game
            std::vector<unsigned char> sub_bytes;
            bool sub_is_sfo = false;
            if (!vfs.read_file(FileVfs::join(child, "sce_sys/param.sfo"), kParamReadLimit, sub_bytes, error)) {
                sub_bytes.clear();
                if (!vfs.read_file(FileVfs::join(child, "sce_sys/param.json"), kParamReadLimit, sub_bytes, error)) sub_bytes.clear();
            } else {
                sub_is_sfo = true;
            }

            if (!sub_bytes.empty()) {
                ParamMetadata meta = sub_is_sfo ? parse_param_sfo_bytes(sub_bytes)
                                                : parse_param_json_string(std::string(sub_bytes.begin(), sub_bytes.end()));
                if (meta.valid) {
                    const uint64_t dir_size =
                        options.calculate_directory_sizes
                            ? vfs.directory_size(child, 8, [this] { return cancel_requested_.load(); })
                            : 0;
                    const std::string icon_path = resolve_icon(child + "/sce_sys", meta.title_id);
                    register_item(child, meta, "folder", dir_size, root, entries_map, stats, icon_path);
                    if (progress) {
                        progress(child, entries_map.size(), false);
                    }
                    continue;
                }
            }

            // Recurse if below max depth
            if (options.recursive && current_depth < options.max_depth) {
                scan_directory_internal(vfs, child, current_depth + 1, options, root, address, entries_map, stats, progress);
            }
        } else if (entry.is_regular_file) {
            stats.files_scanned++;
            std::string ext = entry.name.find_last_of('.') == std::string::npos
                                  ? std::string{}
                                  : entry.name.substr(entry.name.find_last_of('.'));
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

            bool supported = false;
            for (const auto& sup : options.supported_extensions) {
                if (ext == sup) {
                    supported = true;
                    break;
                }
            }

            if (supported) {
                ParamMetadata meta = parse_filename_heuristics(entry.name);
                std::vector<unsigned char> package_icon;
                // Automatic identification on every scan of a mounted path:
                // package header first (PKG Viewer port), filename patterns second.
                if (ext == ".pkg") {
                    std::string read_error;
                    auto read = [&](uint64_t off, size_t len, std::vector<unsigned char>& out) {
                        std::string err;
                        const bool ok = vfs.read_range(child, off, len, out, err);
                        if (!ok && read_error.empty()) read_error = err;
                        return ok;
                    };
                    PackageIdentity id = identify_package(read, entry.size);
                    PackageDiagnostic diag;
                    diag.path = child;
                    diag.root = root;
                    const auto rid = options.root_identities.find(root);
                    diag.source_id = rid != options.root_identities.end() ? rid->second.source_id : root;
                    if (!id.ok) {
                        diag.reason = read_error.empty() ? id.error : id.error + " (" + read_error + ")";
                        id = identify_from_filename(entry.name);
                    }
                    diag.identity_source = !id.ok ? "unidentified"
                        : id.source == IdentitySource::PackageHeader ? "file_contents" : "filename_fallback";
                    diag.title_id = id.title_id;
                    diag.title = id.title;
                    if (diag.identity_source == "file_contents") stats.packages_from_contents++;
                    else if (diag.identity_source == "filename_fallback") stats.packages_from_filename++;
                    else stats.packages_unidentified++;
                    if (stats.packages.size() < 5000) stats.packages.push_back(std::move(diag));
                    if (!id.icon_png.empty()) package_icon = std::move(id.icon_png);
                    if (id.ok) {
                        if (!id.title_id.empty()) meta.title_id = id.title_id;
                        if (!id.title.empty()) meta.title_name = id.title;
                        if (!id.category.empty()) { meta.category = id.category; meta.kind = parse_item_kind(id.category); }
                        meta.content_id = id.content_id;
                        meta.region = id.region;
                        meta.valid = true;
                        meta.confidence = id.source == IdentitySource::PackageHeader ? MetadataConfidence::Verified
                                                                                    : MetadataConfidence::Filename;
                    }
                } else if (meta.region.empty()) {
                    meta.region = region_from_title_id(meta.title_id);
                }
                // Cover art embedded in the package (icon0.png entry) is preferred and
                // cached locally; a sibling icon0.png is the fallback.
                std::string icon_path;
                if (!package_icon.empty() && !options.cover_cache_dir.empty())
                    icon_path = cache_remote_artwork(address, package_icon, meta.title_id, child);
                if (icon_path.empty()) icon_path = resolve_icon(dir, meta.title_id);
                register_item(child, meta, ext, entry.size, root, entries_map, stats, icon_path);

                if (progress) {
                    progress(child, entries_map.size(), false);
                }
            }
        }
    }
}

std::vector<GameEntry> ScannerEngine::scan(const ScanOptions& options,
                                          ScanProgressCallback progress,
                                          ScanStatistics* stats) {
    cancel_requested_.store(false);
    is_scanning_.store(true);

    auto t0 = std::chrono::steady_clock::now();
    ScanStatistics local_stats;
    std::map<std::string, GameEntry> entries_map;

    // Assemble roots: local paths and network specs, deduplicated by identity.
    struct RootStart {
        std::string path;   // identity key (local path or network URI)
        std::string start;  // transport-relative start path
        FileServerAddress address;
    };
    std::vector<RootStart> all_roots;
    {
        auto roots = options.roots;
        if (roots.empty()) roots.push_back(options.root_path);
        for (auto& path : roots) path = std::filesystem::path(path).lexically_normal().string();
        for (const auto& path : roots) {
            all_roots.push_back({path, path, FileServerAddress{}});
        }
        for (const auto& spec : options.remote_roots) {
            all_roots.push_back({spec.path, spec.base_path, spec.address});
        }
    }
    std::sort(all_roots.begin(), all_roots.end(),
              [](const RootStart& a, const RootStart& b) { return a.path < b.path; });
    all_roots.erase(std::unique(all_roots.begin(), all_roots.end(),
                                [](const RootStart& a, const RootStart& b) { return a.path == b.path; }),
                    all_roots.end());

    cover_cache_dir_ = options.cover_cache_dir;

    for (const auto& root_start : all_roots) {
        if (cancel_requested_.load()) break;

        std::unique_ptr<IFileTransport> transport;
        if (root_start.address.kind != FileTransportKind::Local) {
            std::string error;
            const int attempts = std::max(1, options.connect_attempts);
            int wait_ms = std::max(0, options.connect_backoff_ms);
            for (int attempt = 1; attempt <= attempts && !transport && !cancel_requested_.load(); ++attempt) {
                error.clear();
                transport = options.transport_factory ? options.transport_factory(root_start.address, error)
                                                      : make_file_transport(root_start.address, error);
                if (transport || attempt == attempts) break;
                if (progress) progress(root_start.path + ": offline - retrying (" + std::to_string(attempt) + "/" +
                                       std::to_string(attempts - 1) + ")", entries_map.size(), false);
                std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
                wait_ms = std::min(wait_ms * 2, 8000);
            }
            if (!transport) {
                if (progress) progress(root_start.path + ": offline after " + std::to_string(attempts) + " attempts - " + error,
                                       entries_map.size(), false);
                continue;
            }
        }

        FileVfs vfs(std::move(transport),
                    root_start.address.kind == FileTransportKind::Local ? root_start.path : root_start.start);
        scan_directory_internal(vfs, root_start.start, 0, options, root_start.path, root_start.address,
                                entries_map, local_stats, progress);
    }

    std::vector<GameEntry> result;
    result.reserve(entries_map.size());
    uint64_t total_bytes = 0;

    for (auto& [key, entry] : entries_map) {
        const auto identity = options.root_identities.find(entry.source_root);
        if (identity != options.root_identities.end()) {
            entry.source_id = identity->second.source_id;
            entry.storage_type = identity->second.storage_type;
            if (identity->second.platform) entry.platform = identity->second.platform;
        }
        entry.calculate_totals();
        total_bytes += entry.total_size_bytes;
        result.push_back(std::move(entry));
    }

    // Sort alphabetically by Title
    std::sort(result.begin(), result.end(), [](const GameEntry& a, const GameEntry& b) {
        std::string ta = a.title;
        std::string tb = b.title;
        const auto lower = [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); };
        std::transform(ta.begin(), ta.end(), ta.begin(), lower);
        std::transform(tb.begin(), tb.end(), tb.begin(), lower);
        return ta < tb;
    });

    local_stats.total_library_bytes = total_bytes;
    local_stats.elapsed_time = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);

    if (stats) {
        *stats = local_stats;
    }

    if (progress) {
        progress("Complete", result.size(), true);
    }

    is_scanning_.store(false);
    return result;
}

void ScannerEngine::start_async_scan(const ScanOptions& options,
                                    ScanProgressCallback progress,
                                    std::function<void(std::vector<GameEntry>, ScanStatistics)> on_complete) {
    if (is_scanning_.load()) {
        return;
    }

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }

    is_scanning_.store(true); // Publish before launching, not after the worker is scheduled.
    worker_thread_ = std::thread([this, options, progress, on_complete]() {
        ScanStatistics stats;
        auto entries = scan(options, progress, &stats);
        if (on_complete && !cancel_requested_.load()) {
            on_complete(std::move(entries), stats);
        }
    });
}

} // namespace nexodion
