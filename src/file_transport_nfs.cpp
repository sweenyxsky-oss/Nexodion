// Native NFS file transport built on the vendored libnfs (LGPL-2.1).
// Synchronous, bounded operations only: this runs on scanner/probe workers,
// never on the UI thread.
#include "nexodion/file_transport.hpp"

#include <nfsc/libnfs.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace nexodion {
namespace detail {

namespace {
// NFSv3 file type constants (NF3REG=1, NF3DIR=2) from the RFC 1813 protocol.
constexpr uint32_t kNf3Reg = 1;
constexpr uint32_t kNf3Dir = 2;

class NfsTransport final : public IFileTransport {
public:
    explicit NfsTransport(struct nfs_context* ctx) : ctx_(ctx) {}
    ~NfsTransport() override {
        if (ctx_) nfs_destroy_context(ctx_);
    }
    NfsTransport(const NfsTransport&) = delete;
    NfsTransport& operator=(const NfsTransport&) = delete;

    bool list_directory(const std::string& path, std::vector<RemoteFileEntry>& out,
                        std::string& error) override {
        struct nfsdir* dir = nullptr;
        if (nfs_opendir(ctx_, path.c_str(), &dir) != 0) {
            error = std::string("NFS listing failed: ") + nfs_get_error(ctx_);
            return false;
        }
        for (struct nfsdirent* entry = nfs_readdir(ctx_, dir); entry; entry = nfs_readdir(ctx_, dir)) {
            const std::string name = entry->name ? entry->name : "";
            if (name == "." || name == "..") continue;
            RemoteFileEntry item;
            item.name = name;
            const uint32_t type = entry->type;
            const uint64_t mode = entry->mode;
            if (type == kNf3Dir || ((mode & S_IFMT) == S_IFDIR)) {
                item.is_directory = true;
            } else if (type == kNf3Reg || ((mode & S_IFMT) == S_IFREG)) {
                item.is_regular_file = true;
                item.size = entry->size;
            }
            out.push_back(std::move(item));
        }
        nfs_closedir(ctx_, dir);
        std::sort(out.begin(), out.end(), [](const RemoteFileEntry& a, const RemoteFileEntry& b) {
            return a.name < b.name;
        });
        return true;
    }

    bool read_range(const std::string& path, uint64_t offset, size_t len,
                    std::vector<unsigned char>& out, std::string& error) override {
        struct nfsfh* fh = nullptr;
        if (nfs_open(ctx_, path.c_str(), O_RDONLY, &fh) != 0) { error = std::string("NFS open failed: ") + nfs_get_error(ctx_); return false; }
        out.assign(len, 0);
        size_t done = 0;
        while (done < len) {
            const size_t chunk = len - done > 1024u * 512 ? 1024u * 512 : len - done;
            const int n = nfs_pread(ctx_, fh, reinterpret_cast<char*>(out.data()) + done, chunk, offset + done);
            if (n <= 0) break;
            done += static_cast<size_t>(n);
        }
        nfs_close(ctx_, fh);
        if (done != len) { error = "NFS short read"; return false; }
        return true;
    }
    bool read_file(const std::string& path, size_t max_bytes, std::vector<unsigned char>& out,
                   std::string& error) override {
        struct nfs_stat_64 st{};
        if (nfs_stat64(ctx_, path.c_str(), &st) != 0) {
            error = std::string("NFS stat failed: ") + nfs_get_error(ctx_);
            return false;
        }
        if ((st.nfs_mode & S_IFMT) == S_IFDIR) {
            error = "Not a regular file";
            return false;
        }
        if (st.nfs_size > max_bytes) {
            error = "File exceeds the read limit";
            return false;
        }
        struct nfsfh* fh = nullptr;
        if (nfs_open(ctx_, path.c_str(), O_RDONLY, &fh) != 0) {
            error = std::string("NFS open failed: ") + nfs_get_error(ctx_);
            return false;
        }
        out.assign(static_cast<size_t>(st.nfs_size), 0);
        size_t offset = 0;
        bool ok = true;
        while (offset < out.size()) {
            const size_t chunk = out.size() - offset > 1024u * 512 ? 1024u * 512 : out.size() - offset;
            const int n = nfs_pread(ctx_, fh, reinterpret_cast<char*>(out.data()) + offset, chunk,
                                    static_cast<uint64_t>(offset));
            if (n < 0 || static_cast<size_t>(n) == 0) {
                error = std::string("NFS read failed: ") + nfs_get_error(ctx_);
                ok = false;
                break;
            }
            offset += static_cast<size_t>(n);
        }
        nfs_close(ctx_, fh);
        return ok;
    }

    bool stat_path(const std::string& path, RemoteFileEntry& out, std::string& error) override {
        struct nfs_stat_64 st{};
        if (nfs_stat64(ctx_, path.c_str(), &st) != 0) {
            error = std::string("NFS stat failed: ") + nfs_get_error(ctx_);
            return false;
        }
        out.name = path;
        out.is_directory = (st.nfs_mode & S_IFMT) == S_IFDIR;
        out.is_regular_file = (st.nfs_mode & S_IFMT) == S_IFREG;
        out.size = out.is_regular_file ? st.nfs_size : 0;
        return true;
    }

    const char* kind_name() const override { return "nfs"; }

    StorageUsage storage_usage(const std::string& path) override {
        StorageUsage u; struct nfs_statvfs_64 v{};
        if (nfs_statvfs64(ctx_, path.c_str(), &v) != 0) { u.error = std::string("NFS statvfs failed: ") + nfs_get_error(ctx_); return u; }
        const uint64_t unit = v.f_frsize ? v.f_frsize : v.f_bsize;
        u.ok = true; u.total_bytes = v.f_blocks * unit; u.free_bytes = v.f_bavail * unit;
        return u;
    }

private:
    struct nfs_context* ctx_;
};

} // namespace

std::unique_ptr<IFileTransport> make_nfs_transport(const FileServerAddress& address,
                                                   std::string& error) {
    struct nfs_context* ctx = nfs_init_context();
    if (!ctx) {
        error = "Cannot initialize NFS context";
        return nullptr;
    }
    nfs_set_timeout(ctx, address.timeout_ms > 0 ? address.timeout_ms : 3000);
    nfs_set_version(ctx, 3);
    // Server may carry an explicit port ("host:port"); libnfs parses it.
    const char* server = address.host.c_str();
    const std::string export_path = address.remote_path.empty() ? "/" : address.remote_path;
    if (nfs_mount(ctx, server, export_path.c_str()) != 0) {
        error = std::string("NFS mount failed: ") + (nfs_get_error(ctx) ? nfs_get_error(ctx) : "unknown error");
        nfs_destroy_context(ctx);
        return nullptr;
    }
    return std::make_unique<NfsTransport>(ctx);
}

} // namespace detail
} // namespace nexodion
