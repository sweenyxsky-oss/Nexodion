#include "nexodion/file_transport.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace nexodion {

const char* to_string(FileTransportKind kind) {
    switch (kind) {
        case FileTransportKind::Local: return "local";
        case FileTransportKind::Nfs: return "nfs";
        case FileTransportKind::Smb: return "smb";
    }
    return "unknown";
}

namespace {

class LocalTransport final : public IFileTransport {
public:
    bool list_directory(const std::string& path, std::vector<RemoteFileEntry>& out,
                        std::string& error) override {
        std::error_code ec;
        if (!std::filesystem::is_directory(path, ec)) {
            error = "Not a directory";
            return false;
        }
        for (const auto& entry : std::filesystem::directory_iterator(
                 path, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (ec) break;
            RemoteFileEntry item;
            item.name = entry.path().filename().string();
            std::error_code fec;
            if (entry.is_directory(fec)) {
                item.is_directory = true;
            } else if (entry.is_regular_file(fec)) {
                item.is_regular_file = true;
                item.size = entry.file_size(fec);
            }
            if (!item.name.empty()) out.push_back(std::move(item));
        }
        if (ec) {
            error = ec.message();
            return false;
        }
        std::sort(out.begin(), out.end(), [](const RemoteFileEntry& a, const RemoteFileEntry& b) {
            return a.name < b.name;
        });
        return true;
    }

    bool read_range(const std::string& path, uint64_t offset, size_t len,
                    std::vector<unsigned char>& out, std::string& error) override {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) { error = "Cannot open file"; return false; }
        out.assign(len, 0);
        file.seekg(static_cast<std::streamoff>(offset));
        file.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(len));
        if (static_cast<size_t>(file.gcount()) != len) { error = "Short read"; return false; }
        return true;
    }
    bool read_file(const std::string& path, size_t max_bytes, std::vector<unsigned char>& out,
                   std::string& error) override {
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        if (ec) {
            error = "Cannot read file";
            return false;
        }
        if (size > max_bytes) {
            error = "File exceeds the read limit";
            return false;
        }
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            error = "Cannot open file";
            return false;
        }
        out.assign(static_cast<size_t>(size), 0);
        if (size > 0) {
            file.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
        }
        if (!file) {
            error = "Short read";
            return false;
        }
        return true;
    }

    bool stat_path(const std::string& path, RemoteFileEntry& out, std::string& error) override {
        std::error_code ec;
        const auto status = std::filesystem::status(path, ec);
        if (ec || status.type() == std::filesystem::file_type::not_found) {
            error = "Path not found";
            return false;
        }
        out.name = std::filesystem::path(path).filename().string();
        out.is_directory = status.type() == std::filesystem::file_type::directory;
        out.is_regular_file = status.type() == std::filesystem::file_type::regular;
        if (out.is_regular_file) out.size = std::filesystem::file_size(path, ec);
        return true;
    }

    const char* kind_name() const override { return "local"; }
    StorageUsage storage_usage(const std::string& path) override {
        StorageUsage u; std::error_code ec;
        const auto info = std::filesystem::space(path, ec);
        if (ec) { u.error = "Cannot read storage space: " + ec.message(); return u; }
        u.ok = true; u.total_bytes = info.capacity; u.free_bytes = info.available;
        return u;
    }
};

} // namespace

std::unique_ptr<IFileTransport> make_local_transport() {
    return std::make_unique<LocalTransport>();
}

// Weak backend hooks: a build without a network library still links and
// reports a precise error; the compiled backend overrides these symbols.
#if defined(__GNUC__) || defined(__clang__)
#define NEXODION_WEAK __attribute__((weak))
#else
#define NEXODION_WEAK
#endif

namespace detail {

NEXODION_WEAK std::unique_ptr<IFileTransport> make_nfs_transport(const FileServerAddress&,
                                                                 std::string& error) {
    error = "NFS transport is not available in this build";
    return nullptr;
}

NEXODION_WEAK std::unique_ptr<IFileTransport> make_smb_transport(const FileServerAddress&,
                                                                 std::string& error) {
    error = "SMB transport is not available in this build";
    return nullptr;
}

} // namespace detail

std::unique_ptr<IFileTransport> make_file_transport(const FileServerAddress& address,
                                                    std::string& error) {
    switch (address.kind) {
        case FileTransportKind::Local:
            return make_local_transport();
        case FileTransportKind::Nfs:
            return detail::make_nfs_transport(address, error);
        case FileTransportKind::Smb:
            return detail::make_smb_transport(address, error);
    }
    error = "Unknown transport kind";
    return nullptr;
}

bool is_remote_root_path(const std::string& path) {
    return path.starts_with("nfs://") || path.starts_with("smb://");
}

std::string FileVfs::clean(std::string path) {
    if (path.empty()) return "/";
    if (path.front() != '/') path.insert(path.begin(), '/');
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    return path;
}

std::string FileVfs::join(const std::string& base, const std::string& name) {
    if (base.empty()) return clean(name);
    if (base.back() == '/') return clean(base + name);
    return clean(base + "/" + name);
}

FileVfs::FileVfs(std::unique_ptr<IFileTransport> transport, std::string base_path)
    : transport_(std::move(transport)), base_path_(clean(std::move(base_path))) {}

bool FileVfs::directory_exists(const std::string& path, std::string& error) const {
    if (!transport_) {
        std::error_code ec;
        return std::filesystem::is_directory(path, ec);
    }
    RemoteFileEntry entry;
    if (!transport_->stat_path(path, entry, error)) return false;
    return entry.is_directory;
}

bool FileVfs::list_directory(const std::string& path, std::vector<RemoteFileEntry>& out,
                             std::string& error) const {
    if (!transport_) return make_local_transport()->list_directory(path, out, error);
    return transport_->list_directory(path, out, error);
}

bool FileVfs::read_file(const std::string& path, size_t max_bytes,
                        std::vector<unsigned char>& out, std::string& error) const {
    if (!transport_) return make_local_transport()->read_file(path, max_bytes, out, error);
    return transport_->read_file(path, max_bytes, out, error);
}

bool FileVfs::read_range(const std::string& path, uint64_t offset, size_t len,
                         std::vector<unsigned char>& out, std::string& error) const {
    if (len > 4u * 1024 * 1024) { error = "Read exceeds the range limit"; return false; }
    if (!transport_) return make_local_transport()->read_range(path, offset, len, out, error);
    return transport_->read_range(path, offset, len, out, error);
}

StorageUsage FileVfs::storage_usage(const std::string& path) const {
    if (!transport_) return make_local_transport()->storage_usage(path);
    return transport_->storage_usage(path);
}

bool FileVfs::regular_file_size(const std::string& path, uint64_t& size, std::string& error) const {
    RemoteFileEntry entry;
    if (!transport_) {
        std::error_code ec;
        const auto file_size = std::filesystem::file_size(path, ec);
        if (ec) {
            error = "Cannot read file";
            return false;
        }
        size = file_size;
        return true;
    }
    if (!transport_->stat_path(path, entry, error)) return false;
    if (!entry.is_regular_file) {
        error = "Not a regular file";
        return false;
    }
    size = entry.size;
    return true;
}

uint64_t FileVfs::directory_size(const std::string& path, int max_depth,
                                 const std::function<bool()>& cancelled) const {
    uint64_t total = 0;
    if (!transport_) {
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(
                 path, std::filesystem::directory_options::skip_permission_denied, ec);
             it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (cancelled && cancelled()) break;
            std::error_code fec;
            if (it->is_directory(fec)) {
                const int depth = static_cast<int>(it.depth());
                if (depth >= max_depth) it.disable_recursion_pending();
            } else if (it->is_regular_file(fec)) {
                total += it->file_size(fec);
            }
        }
        return total;
    }

    // Network: bounded recursive listing so a deep tree cannot spin forever.
    struct Frame {
        std::string path;
        int depth;
    };
    std::vector<Frame> stack{{path, 0}};
    while (!stack.empty()) {
        if (cancelled && cancelled()) break;
        const auto current = stack.back();
        stack.pop_back();
        std::vector<RemoteFileEntry> entries;
        std::string error;
        if (!transport_->list_directory(current.path, entries, error)) continue;
        for (const auto& entry : entries) {
            if (cancelled && cancelled()) break;
            if (entry.is_directory) {
                if (current.depth < max_depth) stack.push_back({join(current.path, entry.name), current.depth + 1});
            } else if (entry.is_regular_file) {
                total += entry.size;
            }
        }
    }
    return total;
}

std::string make_network_root_uri(const FileServerAddress& address, const std::string& sub_path) {
    const std::string sub = FileVfs::clean(sub_path);
    if (address.kind == FileTransportKind::Nfs) {
        const std::string base = FileVfs::clean(address.remote_path.empty() ? "/" : address.remote_path);
        return "nfs://" + address.host + (base == "/" ? "" : base) + (sub == "/" ? "" : sub);
    }
    if (address.kind == FileTransportKind::Smb) {
        std::string share = address.remote_path;
        while (!share.empty() && share.front() == '/') share.erase(share.begin());
        while (!share.empty() && share.back() == '/') share.pop_back();
        return "smb://" + address.host + "/" + share + (sub == "/" ? "" : sub);
    }
    return sub;
}

} // namespace nexodion
