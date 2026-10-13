// Native SMB2/3 file transport built on the vendored libsmb2 (LGPL-2.1).
// Synchronous, bounded operations only: this runs on scanner/probe workers,
// never on the UI thread. The password travels in memory only.
#include "nexodion/file_transport.hpp"

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <fcntl.h>

#include <algorithm>
#include <string>
#include <vector>

namespace nexodion {
namespace detail {

namespace {

class SmbTransport final : public IFileTransport {
public:
    explicit SmbTransport(struct smb2_context* ctx) : ctx_(ctx) {}
    ~SmbTransport() override {
        if (ctx_) {
            smb2_disconnect_share(ctx_);
            smb2_destroy_context(ctx_);
        }
    }
    SmbTransport(const SmbTransport&) = delete;
    SmbTransport& operator=(const SmbTransport&) = delete;

    bool list_directory(const std::string& path, std::vector<RemoteFileEntry>& out,
                        std::string& error) override {
        struct smb2dir* dir = smb2_opendir(ctx_, path.c_str());
        if (!dir) {
            error = std::string("SMB listing failed: ") + smb2_get_error(ctx_);
            return false;
        }
        for (struct smb2dirent* entry = smb2_readdir(ctx_, dir); entry; entry = smb2_readdir(ctx_, dir)) {
            const std::string name = entry->name ? entry->name : "";
            if (name == "." || name == "..") continue;
            RemoteFileEntry item;
            item.name = name;
            if (entry->st.smb2_type == SMB2_TYPE_DIRECTORY) {
                item.is_directory = true;
            } else if (entry->st.smb2_type == SMB2_TYPE_FILE) {
                item.is_regular_file = true;
                item.size = entry->st.smb2_size;
            }
            out.push_back(std::move(item));
        }
        smb2_closedir(ctx_, dir);
        std::sort(out.begin(), out.end(), [](const RemoteFileEntry& a, const RemoteFileEntry& b) {
            return a.name < b.name;
        });
        return true;
    }

    bool read_range(const std::string& path, uint64_t offset, size_t len,
                    std::vector<unsigned char>& out, std::string& error) override {
        struct smb2fh* fh = smb2_open(ctx_, path.c_str(), O_RDONLY);
        if (!fh) { error = std::string("SMB open failed: ") + smb2_get_error(ctx_); return false; }
        out.assign(len, 0);
        size_t done = 0;
        while (done < len) {
            const size_t chunk = len - done > 1024u * 512 ? 1024u * 512 : len - done;
            const int n = smb2_pread(ctx_, fh, reinterpret_cast<uint8_t*>(out.data()) + done, static_cast<uint32_t>(chunk), offset + done);
            if (n <= 0) break;
            done += static_cast<size_t>(n);
        }
        smb2_close(ctx_, fh);
        if (done != len) { error = "SMB short read"; return false; }
        return true;
    }
    bool read_file(const std::string& path, size_t max_bytes, std::vector<unsigned char>& out,
                   std::string& error) override {
        struct smb2_stat_64 st{};
        if (smb2_stat(ctx_, path.c_str(), &st) != 0) {
            error = std::string("SMB stat failed: ") + smb2_get_error(ctx_);
            return false;
        }
        if (st.smb2_type != SMB2_TYPE_FILE) {
            error = "Not a regular file";
            return false;
        }
        if (st.smb2_size > max_bytes) {
            error = "File exceeds the read limit";
            return false;
        }
        struct smb2fh* fh = smb2_open(ctx_, path.c_str(), O_RDONLY);
        if (!fh) {
            error = std::string("SMB open failed: ") + smb2_get_error(ctx_);
            return false;
        }
        out.assign(static_cast<size_t>(st.smb2_size), 0);
        size_t offset = 0;
        bool ok = true;
        while (offset < out.size()) {
            const size_t chunk = out.size() - offset > 1024u * 512 ? 1024u * 512 : out.size() - offset;
            const int n = smb2_pread(ctx_, fh, reinterpret_cast<uint8_t*>(out.data()) + offset,
                                     static_cast<uint32_t>(chunk), static_cast<uint64_t>(offset));
            if (n < 0) {
                error = std::string("SMB read failed: ") + smb2_get_error(ctx_);
                ok = false;
                break;
            }
            if (n == 0) break;
            offset += static_cast<size_t>(n);
        }
        if (ok && offset != out.size()) {
            error = "Short read";
            ok = false;
        }
        smb2_close(ctx_, fh);
        return ok;
    }

    bool stat_path(const std::string& path, RemoteFileEntry& out, std::string& error) override {
        struct smb2_stat_64 st{};
        if (smb2_stat(ctx_, path.c_str(), &st) != 0) {
            error = std::string("SMB stat failed: ") + smb2_get_error(ctx_);
            return false;
        }
        out.name = path;
        out.is_directory = st.smb2_type == SMB2_TYPE_DIRECTORY;
        out.is_regular_file = st.smb2_type == SMB2_TYPE_FILE;
        out.size = out.is_regular_file ? st.smb2_size : 0;
        return true;
    }

    const char* kind_name() const override { return "smb"; }

    StorageUsage storage_usage(const std::string& path) override {
        StorageUsage u; struct smb2_statvfs v{};
        if (smb2_statvfs(ctx_, path.empty() ? "" : path.c_str(), &v) != 0) { u.error = std::string("SMB statvfs failed: ") + smb2_get_error(ctx_); return u; }
        const uint64_t unit = v.f_frsize ? v.f_frsize : v.f_bsize;
        u.ok = true; u.total_bytes = v.f_blocks * unit; u.free_bytes = v.f_bavail * unit;
        return u;
    }

private:
    struct smb2_context* ctx_;
};

} // namespace

std::unique_ptr<IFileTransport> make_smb_transport(const FileServerAddress& address,
                                                   std::string& error) {
    struct smb2_context* ctx = smb2_init_context();
    if (!ctx) {
        error = "Cannot initialize SMB context";
        return nullptr;
    }
    smb2_set_timeout(ctx, address.timeout_ms > 0 ? (address.timeout_ms + 999) / 1000 : 3);
    smb2_set_version(ctx, SMB2_VERSION_ANY);
    if (!address.username.empty()) smb2_set_user(ctx, address.username.c_str());
    if (!address.secret.empty()) smb2_set_password(ctx, address.secret.c_str());

    std::string share = address.remote_path;
    while (!share.empty() && share.front() == '/') share.erase(share.begin());
    while (!share.empty() && share.back() == '/') share.pop_back();
    if (share.empty()) {
        error = "SMB share name is required";
        smb2_destroy_context(ctx);
        return nullptr;
    }
    if (smb2_connect_share(ctx, address.host.c_str(), share.c_str(),
                           address.username.empty() ? nullptr : address.username.c_str()) != 0) {
        error = std::string("SMB connect failed: ") + (smb2_get_error(ctx) ? smb2_get_error(ctx) : "unknown error");
        smb2_destroy_context(ctx);
        return nullptr;
    }
    return std::make_unique<SmbTransport>(ctx);
}

} // namespace detail
} // namespace nexodion
