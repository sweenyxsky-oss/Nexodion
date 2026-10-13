#pragma once
// SourceManager: abstraction over library sources (Local, NFS, SMB).
//
// Network sources use native file access (libnfs / libsmb2) directly; no
// FUSE mount or ShadowMountPlus dependency is required to list or read a
// share. SMB passwords live in session memory only and are never persisted.
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <map>
#include "file_transport.hpp"
#include "navigation.hpp"

namespace nexodion {

enum class SourceKind { Local, Nfs, Smb };
const char* to_string(SourceKind kind);

enum class SourceState { Unmounted, Mounted, Unreachable, Locked };
const char* to_string(SourceState state);

struct SourceConfig {
    std::string id;           // unique, user-facing key
    std::string display_name;
    SourceKind kind{SourceKind::Local};
    std::string host;         // NFS/SMB only
    std::string remote_path;  // NFS export or SMB share
    std::string mount_point;  // local path where content is visible
    std::string username;     // SMB only
    bool enabled{true};
    std::map<NavTab,std::string> platform_paths;
};

// Validates a config; returns an error message, or nullopt when valid.
std::optional<std::string> validate_source_config(const SourceConfig& cfg);

class ILibrarySource {
public:
    virtual ~ILibrarySource() = default;
    virtual const SourceConfig& config() const = 0;
    virtual SourceState state() const = 0;
    virtual SourceState refresh() = 0;            // re-probe availability
    virtual std::string scan_root() const = 0;    // legacy local path; network roots use address()
    virtual FileServerAddress address() const = 0; // includes the session secret when one is set
    virtual void set_session_secret(const std::string& secret) = 0;
    virtual std::string session_secret() const = 0;
    // Kept for API compatibility; native file access removed the daemon lock.
    virtual bool requires_daemon() const = 0;
    virtual void set_daemon_unlocked(bool unlocked) = 0;
};

std::unique_ptr<ILibrarySource> make_source(const SourceConfig& cfg, bool daemon_unlocked);

class SourceManager {
public:
    explicit SourceManager(bool daemon_unlocked = false) : daemon_unlocked_(daemon_unlocked) {}

    // Returns error message on failure.
    std::optional<std::string> add_source(const SourceConfig& cfg);
    std::optional<std::string> update_source(const std::string& id, const SourceConfig& cfg);
    bool save_file(const std::string& path, std::string* error = nullptr) const;
    bool load_file(const std::string& path, std::string* error = nullptr);
    bool remove_source(const std::string& id);
    std::vector<SourceConfig> list() const;
    std::optional<SourceState> state_of(const std::string& id) const;
    std::vector<std::string> active_scan_roots() const;  // mounted + enabled (local paths)
    // Full per-root specifications for the scanner, including network roots
    // with their host/export/share and the in-session secret, if any.
    std::vector<ScanRootSpec> active_scan_specs() const;

    // SMB passwords: held in memory for this session only; never serialized.
    void set_session_secret(const std::string& id, const std::string& secret);
    std::string session_secret(const std::string& id) const;

    void set_daemon_unlocked(bool unlocked);
    void refresh_all();

    std::string to_json() const;
    bool load_json(const std::string& json);  // replaces current sources

private:
    void apply_session_secrets_locked();

    mutable std::mutex mu_;
    bool daemon_unlocked_;
    // Shared snapshots keep a source alive while an out-of-lock probe runs.
    std::vector<std::shared_ptr<ILibrarySource>> sources_;
    std::map<std::string, std::string> session_secrets_;  // id -> password (memory only)
};

} // namespace nexodion
