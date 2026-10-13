#include "nexodion/source_manager.hpp"
#include "nexodion/library_state.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <atomic>
#include <mutex>
#include <nlohmann/json.hpp>

namespace nexodion {

const char* to_string(SourceKind k) {
    switch (k) {
        case SourceKind::Local: return "local";
        case SourceKind::Nfs: return "nfs";
        case SourceKind::Smb: return "smb";
    }
    return "local";
}

const char* to_string(SourceState s) {
    switch (s) {
        case SourceState::Unmounted: return "unmounted";
        case SourceState::Mounted: return "mounted";
        case SourceState::Unreachable: return "unreachable";
        case SourceState::Locked: return "locked";
    }
    return "unmounted";
}

static std::optional<SourceKind> kind_from(const std::string& s) {
    if (s == "local") return SourceKind::Local;
    if (s == "nfs") return SourceKind::Nfs;
    if (s == "smb") return SourceKind::Smb;
    return std::nullopt;
}

static FileTransportKind transport_kind(SourceKind kind) {
    switch (kind) {
        case SourceKind::Local: return FileTransportKind::Local;
        case SourceKind::Nfs: return FileTransportKind::Nfs;
        case SourceKind::Smb: return FileTransportKind::Smb;
    }
    return FileTransportKind::Local;
}

std::optional<std::string> validate_source_config(const SourceConfig& c) {
    if (c.id.empty()) return "Source id is required";
    for (const auto* field : {&c.id, &c.display_name, &c.host, &c.remote_path, &c.mount_point, &c.username}) {
        if (field->size() > 4096) return "Source field is too long";
        if (std::any_of(field->begin(), field->end(), [](unsigned char ch){return ch < 32 || ch == 127;}))
            return "Source fields must not contain control characters";
    }
    if (c.mount_point.empty() || c.mount_point.front() != '/')
        return "Mount point must be an absolute path";
    if (c.mount_point.find("..") != std::string::npos) return "Mount point must not contain '..'";
    if (c.kind != SourceKind::Local) {
        if (c.host.empty()) return "Host is required for network sources";
        if (c.remote_path.empty()) return "Remote path/share is required for network sources";
    }
    for(const auto& [platform,path]:c.platform_paths) {
        if(platform==NavTab::Installed) return "Installed is not a library platform";
        if(path.empty())continue;
        if(path.size()>4096 || path.front()!='/' || path.find("..")!=std::string::npos ||
            std::any_of(path.begin(),path.end(),[](unsigned char ch){return ch<32 || ch==127;})) return "Platform path must be a plain absolute path";
        // Containment applies to local roots only; on network sources a
        // platform path is resolved relative to the share/export, not the
        // local mount point.
        if(c.kind==SourceKind::Local) {
            const auto base=std::filesystem::path(c.mount_point).lexically_normal();
            const auto relative=std::filesystem::path(path).lexically_normal().lexically_relative(base);
            if(relative.empty() || *relative.begin()=="..")return "Platform path must belong to this device path";
        }
        for(const auto& [other,other_path]:c.platform_paths)if(other!=platform && !other_path.empty() &&
            std::filesystem::path(other_path).lexically_normal()==std::filesystem::path(path).lexically_normal())return "Each platform needs its own path";
    }
    return std::nullopt;
}

namespace {
class BasicSource : public ILibrarySource {
public:
    BasicSource(SourceConfig cfg, bool unlocked) : cfg_(std::move(cfg)), unlocked_(unlocked) { refresh(); }
    const SourceConfig& config() const override { return cfg_; }
    SourceState state() const override { return state_.load(); }
    // Native file access removed the ShadowMountPlus dependency for NFS/SMB.
    bool requires_daemon() const override { return false; }
    std::string scan_root() const override { return cfg_.mount_point; }
    FileServerAddress address() const override {
        std::lock_guard lk(secret_mu_);
        FileServerAddress a;
        a.kind = transport_kind(cfg_.kind);
        a.host = cfg_.host;
        a.remote_path = cfg_.remote_path;
        a.username = cfg_.username;
        a.secret = secret_;
        a.timeout_ms = 3000;
        return a;
    }
    void set_session_secret(const std::string& secret) override {
        std::lock_guard lk(secret_mu_);
        secret_ = secret;
    }
    std::string session_secret() const override {
        std::lock_guard lk(secret_mu_);
        return secret_;
    }
    SourceState refresh() override {
        if (cfg_.kind == SourceKind::Local) {
            std::error_code ec;
            if (!std::filesystem::is_directory(cfg_.mount_point, ec))
                return state_ = SourceState::Unmounted;
            return state_ = SourceState::Mounted;
        }
        // Bounded network probe: connect and stat the export/share root.
        FileServerAddress a = address();
        std::string error;
        auto transport = make_file_transport(a, error);
        if (!transport) return state_ = SourceState::Unreachable;
        RemoteFileEntry root;
        if (!transport->stat_path("/", root, error) || !root.is_directory)
            return state_ = SourceState::Unreachable;
        return state_ = SourceState::Mounted;
    }
    void set_daemon_unlocked(bool u) override { unlocked_ = u; }
private:
    SourceConfig cfg_;
    std::atomic<bool> unlocked_;
    std::atomic<SourceState> state_{SourceState::Unmounted};
    mutable std::mutex secret_mu_;
    std::string secret_;  // SMB password; session memory only
};
} // namespace

std::unique_ptr<ILibrarySource> make_source(const SourceConfig& cfg, bool unlocked) {
    return std::make_unique<BasicSource>(cfg, unlocked);
}

std::optional<std::string> SourceManager::add_source(const SourceConfig& cfg) {
    if (auto err = validate_source_config(cfg)) return err;
    bool unlocked;
    {std::lock_guard lk(mu_);unlocked=daemon_unlocked_;}
    auto source=make_source(cfg,unlocked);
    std::lock_guard lk(mu_);
    for (auto& s : sources_)
        if (s->config().id == cfg.id) return "Duplicate source id: " + cfg.id;
    source->set_daemon_unlocked(daemon_unlocked_);
    sources_.push_back(std::move(source));
    apply_session_secrets_locked();
    return std::nullopt;
}

bool SourceManager::remove_source(const std::string& id) {
    std::lock_guard lk(mu_);
    auto it = std::remove_if(sources_.begin(), sources_.end(),
                             [&](auto& s) { return s->config().id == id; });
    bool removed = it != sources_.end();
    sources_.erase(it, sources_.end());
    session_secrets_.erase(id);
    return removed;
}

std::optional<std::string> SourceManager::update_source(const std::string& id, const SourceConfig& cfg) {
    if (auto err = validate_source_config(cfg)) return err;
    bool unlocked;
    {std::lock_guard lk(mu_);unlocked=daemon_unlocked_;}
    auto source=make_source(cfg,unlocked);
    std::lock_guard lk(mu_);
    for (const auto& s : sources_) if (s->config().id == cfg.id && s->config().id != id)
        return "Duplicate source id: " + cfg.id;
    for (auto& s : sources_) if (s->config().id == id) {
        source->set_daemon_unlocked(daemon_unlocked_);
        s = std::move(source);
        apply_session_secrets_locked();
        return std::nullopt;
    }
    return "Source no longer exists";
}

bool SourceManager::save_file(const std::string& path, std::string* error) const {
    auto fail = [&](const std::string& text){if(error) *error=text;return false;};
    if(path.empty()) return fail("No source configuration path configured");
    std::error_code ec;
    const auto parent=std::filesystem::path(path).parent_path();
    if(!parent.empty()) std::filesystem::create_directories(parent,ec);
    if(ec) return fail(ec.message());
    keep_backup(path);
    std::string temp=path+".tmp.XXXXXX";
    const int fd=mkstemp(temp.data());
    if(fd<0) return fail("Cannot create source configuration temporary file");
    const auto bytes=to_json();
    size_t offset=0;
    bool ok=true;
    while(offset<bytes.size()) {
        const auto n=write(fd,bytes.data()+offset,bytes.size()-offset);
        if(n<0 && errno==EINTR) continue;
        if(n<=0){ok=false;break;}
        offset+=static_cast<size_t>(n);
    }
    if(fsync(fd)!=0) ok=false;
    if(close(fd)!=0) ok=false;
    if(ok && rename(temp.c_str(),path.c_str())==0) return true;
    unlink(temp.c_str());
    return fail("Cannot commit source configuration; previous file preserved");
}

bool SourceManager::load_file(const std::string& path, std::string* error) {
    std::error_code ec;
    const auto size=std::filesystem::file_size(path,ec);
    if(ec || size>1024*1024) {if(error)*error="Unreadable or oversized source configuration";return false;}
    std::ifstream file(path,std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(file)),{});
    if(file.bad() || !load_json(text)) {if(error)*error="Invalid source configuration";return false;}
    return true;
}

std::vector<SourceConfig> SourceManager::list() const {
    std::lock_guard lk(mu_);
    std::vector<SourceConfig> out;
    for (auto& s : sources_) out.push_back(s->config());
    return out;
}

std::optional<SourceState> SourceManager::state_of(const std::string& id) const {
    std::lock_guard lk(mu_);
    for (auto& s : sources_)
        if (s->config().id == id) return s->state();
    return std::nullopt;
}

std::vector<std::string> SourceManager::active_scan_roots() const {
    std::vector<std::string> out;
    for (const auto& spec : active_scan_specs())
        if (spec.address.kind == FileTransportKind::Local) out.push_back(spec.path);
    return out;
}

std::vector<ScanRootSpec> SourceManager::active_scan_specs() const {
    std::lock_guard lk(mu_);
    std::vector<ScanRootSpec> out;
    for (auto& s : sources_) {
        const auto& c = s->config();
        // Enabled sources are always spec'd: the scanner skips roots whose
        // transport cannot be created, so a temporary outage does not hide
        // the configured platform paths from the scan plan.
        if (!c.enabled) continue;
        const auto addr = s->address();
        auto platform_spec = [&](const std::string& path) {
            // Relative sub path for network roots; the absolute path for local.
            std::string sub = path;
            if (addr.kind != FileTransportKind::Local && !c.mount_point.empty() && sub.starts_with(c.mount_point))
                sub = sub.substr(c.mount_point.size());
            if (sub.empty()) sub = "/";
            return sub;
        };
        if (c.platform_paths.empty()) {
            const std::string sub = platform_spec(c.mount_point);
            if (addr.kind == FileTransportKind::Local)
                out.push_back({c.mount_point, c.mount_point, addr, std::nullopt, c.id, to_string(c.kind)});
            else
                out.push_back({make_network_root_uri(addr, sub), sub, addr, std::nullopt, c.id, to_string(c.kind)});
        } else {
            for (const auto& [platform, path] : c.platform_paths) {
                if (path.empty()) continue;
                const std::string sub = platform_spec(path);
                if (addr.kind == FileTransportKind::Local)
                    out.push_back({path, path, addr, platform, c.id, to_string(c.kind)});
                else
                    out.push_back({make_network_root_uri(addr, sub), sub, addr, platform, c.id, to_string(c.kind)});
            }
        }
    }
    return out;
}

void SourceManager::set_session_secret(const std::string& id, const std::string& secret) {
    std::vector<std::shared_ptr<ILibrarySource>> snapshot;
    {
        std::lock_guard lk(mu_);
        session_secrets_[id] = secret;
        snapshot = sources_;
    }
    for (auto& s : snapshot)
        if (s->config().id == id) s->set_session_secret(secret);
}

std::string SourceManager::session_secret(const std::string& id) const {
    std::lock_guard lk(mu_);
    for (auto& s : sources_)
        if (s->config().id == id) return s->session_secret();
    return {};
}

void SourceManager::set_daemon_unlocked(bool u) {
    std::vector<std::shared_ptr<ILibrarySource>> snapshot;
    {std::lock_guard lk(mu_);daemon_unlocked_=u;snapshot=sources_;}
    for(auto& s:snapshot) {s->set_daemon_unlocked(u);s->refresh();}
}

void SourceManager::refresh_all() {
    std::vector<std::shared_ptr<ILibrarySource>> snapshot;
    {std::lock_guard lk(mu_);snapshot=sources_;}
    for (auto& s : snapshot) s->refresh();
}

void SourceManager::apply_session_secrets_locked() {
    for (auto& s : sources_) {
        const auto it = session_secrets_.find(s->config().id);
        if (it != session_secrets_.end()) s->set_session_secret(it->second);
    }
}

std::string SourceManager::to_json() const {
    std::lock_guard lk(mu_);
    nlohmann::json arr = nlohmann::json::array();
    for (auto& s : sources_) {
        const auto& c = s->config();
        // Passwords are intentionally never persisted here.
        nlohmann::json paths=nlohmann::json::object();
        for(const auto& [platform,path]:c.platform_paths)paths[to_string(platform)]=path;
        arr.push_back({{"platform_paths",paths},{"id", c.id}, {"display_name", c.display_name}, {"kind", to_string(c.kind)},
                       {"host", c.host}, {"remote_path", c.remote_path},
                       {"mount_point", c.mount_point}, {"username", c.username},
                       {"enabled", c.enabled}});
    }
    return nlohmann::json{{"version", 1}, {"sources", arr}}.dump(2);
}

bool SourceManager::load_json(const std::string& text) {
    try {
    auto j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || j.value("version", 1) != 1 || !j.contains("sources") || !j["sources"].is_array()) return false;
    std::vector<std::shared_ptr<ILibrarySource>> next;
    bool unlocked;
    {std::lock_guard lk(mu_);unlocked=daemon_unlocked_;}
    for (auto& e : j["sources"]) {
        if (!e.is_object()) return false;
        SourceConfig c;
        c.id = e.value("id", "");
        c.display_name = e.value("display_name", "");
        auto k = kind_from(e.value("kind", "local"));
        if (!k) return false;
        c.kind = *k;
        c.host = e.value("host", "");
        c.remote_path = e.value("remote_path", "");
        c.mount_point = e.value("mount_point", "");
        c.username = e.value("username", "");
        c.enabled = e.value("enabled", true);
        if(e.contains("platform_paths")) {
            if(!e["platform_paths"].is_object())return false;
            for(const auto& [key,value]:e["platform_paths"].items()) {
                if(!value.is_string())return false;
                bool found=false;
                for(auto tab:kNavTabs)if(tab!=NavTab::Installed && key==to_string(tab)){c.platform_paths[tab]=value.get<std::string>();found=true;}
                if(!found)return false;
            }
        }
        if (validate_source_config(c)) return false;
        for (auto& n : next) if (n->config().id == c.id) return false;
        next.push_back(make_source(c, unlocked));
    }
    std::lock_guard lk(mu_);
    for(auto& s:next) s->set_daemon_unlocked(daemon_unlocked_);
    sources_ = std::move(next);
    apply_session_secrets_locked();
    return true;
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}

} // namespace nexodion
