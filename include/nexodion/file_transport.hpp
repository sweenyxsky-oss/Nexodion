#pragma once
// File transport abstraction for the library scanner.
//
// The scanner must be able to crawl local paths, NFS exports and SMB shares
// through one interface. Network transports (libnfs, libsmb2) are compiled in
// when the vendored libraries are available; the shared factory reports a
// precise error otherwise instead of pretending success.
//
// Secrets: FileServerAddress may carry an SMB password held in memory only.
// It is never persisted, logged, serialized into caches, or copied into
// scanner job files.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "navigation.hpp"

namespace nexodion {

enum class FileTransportKind { Local, Nfs, Smb };
const char* to_string(FileTransportKind kind);

struct RemoteFileEntry {
    std::string name;
    bool is_directory{false};
    bool is_regular_file{false};
    uint64_t size{0};
};

struct FileServerAddress {
    FileTransportKind kind{FileTransportKind::Local};
    std::string host;        // NFS server, or SMB server ("host" or "host:port")
    std::string remote_path; // NFS export path, or SMB share name (no slashes)
    std::string username;    // SMB only; optional
    std::string secret;      // SMB password; in-memory only
    int timeout_ms{4000};    // bounded connect/IO timeout
};

// Capacity of the filesystem holding a path (bytes). `ok` false means the
// backend could not report it; `error` then says why.
struct StorageUsage {
    bool ok{false};
    uint64_t total_bytes{0};
    uint64_t free_bytes{0};
    std::string error;
};

class IFileTransport {
public:
    virtual ~IFileTransport() = default;
    virtual bool list_directory(const std::string& path, std::vector<RemoteFileEntry>& out,
                                std::string& error) = 0;
    virtual bool read_file(const std::string& path, size_t max_bytes,
                           std::vector<unsigned char>& out, std::string& error) = 0;
    // Bounded random-access read of exactly `len` bytes (package headers).
    virtual bool read_range(const std::string& path, uint64_t offset, size_t len,
                            std::vector<unsigned char>& out, std::string& error) = 0;
    virtual bool stat_path(const std::string& path, RemoteFileEntry& out, std::string& error) = 0;
    virtual const char* kind_name() const = 0;
    // Free/total space of the filesystem containing `path`.
    virtual StorageUsage storage_usage(const std::string& path) {
        (void)path; StorageUsage u; u.error = "Storage usage is not reported by this transport"; return u;
    }
};

// Unified view over a transport, or over the local filesystem when no
// transport is set. All paths are absolute: filesystem paths for local access,
// share-relative paths ("/PS5/game") for network transports.
class FileVfs {
public:
    FileVfs() = default;
    explicit FileVfs(std::unique_ptr<IFileTransport> transport, std::string base_path = "/");

    const std::string& base_path() const { return base_path_; }
    bool is_remote() const { return transport_ != nullptr; }

    bool directory_exists(const std::string& path, std::string& error) const;
    bool list_directory(const std::string& path, std::vector<RemoteFileEntry>& out,
                        std::string& error) const;
    bool read_file(const std::string& path, size_t max_bytes, std::vector<unsigned char>& out,
                   std::string& error) const;
    bool read_range(const std::string& path, uint64_t offset, size_t len,
                    std::vector<unsigned char>& out, std::string& error) const;
    bool regular_file_size(const std::string& path, uint64_t& size, std::string& error) const;
    StorageUsage storage_usage(const std::string& path) const;
    // Bounded recursive size; cancelled() short-circuits between entries.
    uint64_t directory_size(const std::string& path, int max_depth,
                            const std::function<bool()>& cancelled) const;

    static std::string join(const std::string& base, const std::string& name);
    static std::string clean(std::string path);

private:
    std::unique_ptr<IFileTransport> transport_;
    std::string base_path_;
};

std::unique_ptr<IFileTransport> make_local_transport();

// Returns nullptr with a precise error when the backend is not built in this
// binary, or when the connection/lookup fails.
std::unique_ptr<IFileTransport> make_file_transport(const FileServerAddress& address,
                                                    std::string& error);

// True when a root path string is a network URI ("nfs://...", "smb://...").
bool is_remote_root_path(const std::string& path);

// Stable root identity: for network sources the URI encodes host, export or
// share and sub path so two sources never merge in caches or entry keys.
std::string make_network_root_uri(const FileServerAddress& address, const std::string& sub_path);

// Per-root scan specification used by the scanner and SourceManager.
struct ScanRootSpec {
    std::string path;               // identity: local path or network URI
    std::string base_path;          // transport-relative start ("/PS5"); local: same as path
    FileServerAddress address;
    std::optional<NavTab> platform; // set when derived from a configured platform path
    std::string source_id;          // owning source configuration id
    std::string storage_type;       // "local", "nfs" or "smb"
};

// Test hook: the scanner builds one transport per remote root.
using TransportFactory =
    std::function<std::unique_ptr<IFileTransport>(const FileServerAddress&, std::string&)>;

} // namespace nexodion
