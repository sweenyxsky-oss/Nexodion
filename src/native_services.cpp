// Native payload relay adapted from PS5 React by half144 and contributors.
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution: external/ps5-react/LICENSE-ATTRIBUTION.
#include "nexodion/native_services.hpp"
#include "nexodion/package_identity.hpp"
#include "nexodion/game_metadata.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <thread>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#if defined(__PS5__) || defined(__PROSPERO__)
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <cerrno>
#endif

namespace nexodion {
bool valid_native_title(const std::string& id) {
    return id.size()==9 && (id.starts_with("PPSA") || id.starts_with("CUSA")) &&
        std::all_of(id.begin()+4,id.end(),[](unsigned char c){return c>='0' && c<='9';});
}
bool valid_native_package_path(const std::string& path) {
    if(path.empty() || path.size()>=1024 || path.find_first_of("\r\n")!=std::string::npos || path.find('\0')!=std::string::npos ||
       path.find("//")!=std::string::npos || !(path.ends_with(".pkg") || path.ends_with(".fpkg"))) return false;
    for(const auto& part:std::filesystem::path(path)) if(part==".." || part==".") return false;
    if(path.starts_with("/data/")) return true;
    for(const auto* prefix:{"/mnt/usb","/mnt/ext"}) {
        const size_t start=std::char_traits<char>::length(prefix);
        if(path.starts_with(prefix) && path.size()>start+1 && path[start]>='0' && path[start]<='9' && path[start+1]=='/') return true;
    }
    return false;
}
bool native_runtime_available() {
#if defined(__PS5__) || defined(__PROSPERO__)
    return true;
#else
    return false;
#endif
}

InstallFailure classify_installer_failure(const std::string& line) {
    InstallFailure f; f.code = InstallErrorCode::IoError;
    std::string rest = line.starts_with("fail ") ? line.substr(5) : line;
    std::string hex, reason = rest;
    const auto sp = rest.find(' ');
    if (sp == 8 && std::all_of(rest.begin(), rest.begin() + 8, [](unsigned char c){ return std::isxdigit(c); })) {
        hex = rest.substr(0, 8); reason = rest.substr(9);
    }
    std::string lower = reason;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c){ return char(std::tolower(c)); });
    auto has = [&](const char* w){ return lower.find(w) != std::string::npos; };
    if (hex == "80b2116f") f.code = InstallErrorCode::InvalidPackage; // documented: package treated as a patch and refused
    else if (has("space") || has("storage full") || has("capacity")) f.code = InstallErrorCode::InsufficientSpace;
    else if (has("already") || has("exist") || has("duplicate")) f.code = InstallErrorCode::DuplicateInstallBlocked;
    else if (has("running") || has("in use")) f.code = InstallErrorCode::TargetAppRunning;
    else if (has("not found") || has("no such")) f.code = InstallErrorCode::FileNotFound;
    else if (has("module") || has("load") || has("symbol")) f.code = InstallErrorCode::SystemModuleMissing;
    else if (has("invalid") || has("corrupt") || has("header")) f.code = InstallErrorCode::InvalidPackage;
    f.message = "Console refused the package" + (hex.empty() ? std::string() : " (code 0x" + hex + ")") +
                (reason.empty() ? std::string() : ": " + reason);
    return f;
}

bool verify_installed_title(const std::string& apps_root, const std::string& title_id, std::string& error) {
    if (!valid_native_title(title_id)) { error = "Title ID is unknown, so the installation cannot be verified"; return false; }
    std::error_code ec;
    const auto dir = std::filesystem::path(apps_root) / title_id;
    if (!std::filesystem::is_directory(dir, ec)) { error = "Title folder " + dir.string() + " was not created"; return false; }
    if (std::filesystem::directory_iterator(dir, ec) == std::filesystem::directory_iterator()) {
        error = "Title folder " + dir.string() + " is empty"; return false;
    }
    error.clear();
    return true;
}

int compare_versions(const std::string& a, const std::string& b) {
    auto split = [](const std::string& v) { std::vector<std::string> out; std::string cur;
        for (char c : v) { if (c == '.' || c == '-' || c == '_') { out.push_back(cur); cur.clear(); } else cur += c; }
        out.push_back(cur); return out; };
    const auto x = split(a), y = split(b);
    for (size_t i = 0; i < std::max(x.size(), y.size()); ++i) {
        const std::string p = i < x.size() ? x[i] : "0", q = i < y.size() ? y[i] : "0";
        const bool pn = !p.empty() && std::all_of(p.begin(), p.end(), ::isdigit), qn = !q.empty() && std::all_of(q.begin(), q.end(), ::isdigit);
        if (pn && qn) { const auto pv = std::stoull(p.substr(0, 18)), qv = std::stoull(q.substr(0, 18)); if (pv != qv) return pv < qv ? -1 : 1; }
        else if (p != q) return p < q ? -1 : 1;
    }
    return 0;
}

std::string installed_title_version(const std::string& apps_root, const std::string& title_id) {
    if (!valid_native_title(title_id)) return {};
    const auto sys = std::filesystem::path(apps_root) / title_id / "sce_sys";
    std::error_code ec;
    if (std::filesystem::is_regular_file(sys / "param.json", ec) && std::filesystem::file_size(sys / "param.json", ec) < 1024 * 1024) {
        std::ifstream in(sys / "param.json");
        auto j = nlohmann::json::parse(in, nullptr, false);
        if (j.is_object() && j.contains("contentVersion") && j["contentVersion"].is_string()) return j["contentVersion"].get<std::string>().substr(0, 32);
    }
    if (std::filesystem::is_regular_file(sys / "param.sfo", ec)) {
        const auto m = parse_param_sfo_file(sys / "param.sfo");
        if (m.valid) return m.version.substr(0, 32);
    }
    return {};
}

std::string InstallReadiness::summary() const {
    std::string out;
    for (const auto& c : checks) out += (c.passed ? "[OK] " : c.blocking ? "[FAIL] " : "[NOTE] ") + c.name + ": " + c.detail + "\n";
    return out;
}

InstallReadiness check_install_readiness(const std::string& package_path, const std::string& apps_root,
                                         const std::string& known_title_id, uint64_t margin) {
    InstallReadiness r;
    std::error_code ec;
    // 1. Package readable and identifiable.
    ReadinessCheck read{"Package readable"};
    const auto size = std::filesystem::file_size(package_path, ec);
    PackageIdentity id;
    if (ec || size == 0) { read.detail = ec ? "Cannot open " + package_path + " (" + ec.message() + ")" : "File is empty";
        read.fix = "Check the share is connected and the file finished copying, then retry."; }
    else {
        id = identify_package_file(package_path);
        if (id.ok) { read.passed = true; read.detail = format_file_size(size) + ", header readable"; }
        else { read.detail = "Header could not be read: " + id.error; read.fix = "The file is damaged or not a PS4/PS5 package. Copy it again from the original source."; }
    }
    r.checks.push_back(read);
    r.title_id = valid_native_title(id.title_id) ? id.title_id : known_title_id;
    r.package_version = id.version;
    // 2. Free space on the console.
    ReadinessCheck space{"Console space"};
    const auto info = std::filesystem::space(apps_root, ec);
    if (ec) { space.detail = "Cannot read free space of " + apps_root + " (" + ec.message() + ")"; space.fix = "Make sure the app storage is available, then retry."; }
    else if (read.passed && info.available < size + margin) {
        space.detail = "Needs " + format_file_size(size + margin) + ", only " + format_file_size(info.available) + " free";
        space.fix = "Delete games or move them to extended storage, then retry.";
    } else { space.passed = true; space.detail = format_file_size(info.available) + " free"; }
    r.checks.push_back(space);
    // 3. Existing installation and version (not blocking: reinstalling or updating is allowed).
    ReadinessCheck existing{"Existing installation", false, false};
    std::string why;
    if (!valid_native_title(r.title_id)) { existing.detail = "Title ID unknown, so an existing copy cannot be checked"; existing.passed = true; }
    else if (verify_installed_title(apps_root, r.title_id, why)) {
        r.already_installed = true;
        r.installed_version = installed_title_version(apps_root, r.title_id);
        if (!r.installed_version.empty() && !r.package_version.empty()) {
            const int cmp = compare_versions(r.installed_version, r.package_version);
            r.same_or_older = cmp >= 0;
            existing.detail = r.title_id + " installed v" + r.installed_version + (cmp < 0 ? ", package is newer (v" + r.package_version + ")"
                              : cmp == 0 ? ", same version as the package" : ", package is older (v" + r.package_version + ")");
            if (cmp < 0) existing.passed = true;
            else existing.fix = "Nothing new to install. Choose Install anyway only if the installed copy is broken.";
        } else { existing.detail = r.title_id + " is already installed (version unknown)"; existing.fix = "Installing again may replace it."; }
    } else { existing.passed = true; existing.detail = "Not installed yet"; }
    r.checks.push_back(existing);
    for (const auto& c : r.checks) if (c.blocking && !c.passed) r.ok = false;
    return r;
}

const std::vector<EmulatorAdapter>& emulator_adapters() {
    static const std::vector<EmulatorAdapter> list{
        {NavTab::PS1, "SwanStationPS5", "https://github.com/sweenyxsky-oss/SwanStationPS5", "PPSA98510"},
        {NavTab::PS2, "PS5SX2", "https://github.com/sweenyxsky-oss/PS5SX2", "PPSA99203"},
        // Placeholder: RPS5S3 is not forked yet; fill in the repository and app ID once it is.
        {NavTab::PS3, "RPS5S3", "", ""},
    };
    return list;
}
const EmulatorAdapter* emulator_for(NavTab p) {
    for (const auto& a : emulator_adapters()) if (a.platform == p) return &a;
    return nullptr;
}

ResolvedLauncher resolve_launcher(NavTab p, const LauncherOverrides& overrides) {
    ResolvedLauncher r;
    if (const auto* emu = emulator_for(p)) { r.name = emu->name; r.title_id = emu->title_id; r.repository = emu->repository; }
    const auto it = overrides.find(to_string(p));
    if (it == overrides.end() || it->second.empty()) return r;
    if (it->second == "none") { r.disabled = true; return r; }
    r.custom = true; r.name = "Custom launcher"; r.title_id = it->second; r.repository.clear();
    return r;
}

LauncherValidation validate_launcher(NavTab p, const LauncherOverrides& overrides, const InstalledCheck& installed) {
    const auto r = resolve_launcher(p, overrides);
    LauncherValidation v;
    if (r.disabled) { v.status = "Disabled"; v.steps = "Choose an emulator to allow launching."; return v; }
    if (r.title_id.empty()) { v.status = r.name + ": not available yet"; v.steps = "Repository link pending; choose Enter App ID if you have a launcher installed."; return v; }
    if (!valid_native_title(r.title_id)) { v.status = "Invalid app ID " + r.title_id; v.steps = "Use a PS4/PS5 app ID such as PPSA12345 or CUSA12345."; return v; }
    if (!(installed && installed(r.title_id))) {
        v.status = r.name + " (" + r.title_id + ") not installed";
        v.steps = r.repository.empty() ? "Install the app, then choose Refresh Installed and validate again."
                                       : "Install it from " + r.repository + ", then choose Refresh Installed and validate again.";
        return v;
    }
    v.ok = true; v.status = r.name + " (" + r.title_id + ") installed and ready";
    return v;
}

LaunchCheck evaluate_launch(const GameEntry& e, const InstalledCheck& is_installed, bool runtime, const LauncherOverrides& overrides) {
    auto installed_id = [&](const std::string& id) { return is_installed && is_installed(id); };
    LaunchCheck c;
    const auto platform = e.platform ? *e.platform
        : e.title_id.starts_with("PPSA") ? NavTab::PS5 : e.title_id.starts_with("CUSA") ? NavTab::PS4 : NavTab::Installed;
    if (platform == NavTab::PS1 || platform == NavTab::PS2 || platform == NavTab::PS3) {
        const std::string name = to_string(platform);
        const auto resolved = resolve_launcher(platform, overrides);
        const auto* emu = &resolved;
        if (resolved.disabled) {
            c.title = name + " launch turned off";
            c.message = "Launching " + name + " games is disabled in Settings > Emulators & Launchers.";
            c.steps = "Nothing was sent to the console.";
            return c;
        }
        if (!valid_native_title(resolved.title_id)) {
            c.title = name + " launch unavailable";
            c.message = resolved.title_id.empty() ? resolved.name + " is not available yet (repository link pending)."
                                                  : "Launcher app ID " + resolved.title_id + " is not valid.";
            c.steps = "Nothing was sent to the console. Choose a launcher in Settings > Emulators & Launchers.";
            return c;
        }
        if (!installed_id(emu->title_id)) {
            c.title = emu->name + " not installed";
            c.message = name + " games open in " + emu->name + " (" + emu->title_id + "), which is not in the installed-title list.";
            c.steps = emu->repository.empty() ? "Install it, then choose Refresh Installed."
                                              : "Install " + emu->name + " from " + emu->repository + ", then choose Refresh Installed.";
            return c;
        }
        if (!runtime) { c.title = "Console runtime required"; c.message = "Launching only works when Nexodion runs on the PS5."; c.steps = "Run the PS5 build of Nexodion."; return c; }
        c.can_launch = true;
        c.launch_title_id = emu->title_id;
        c.title = "Opens " + emu->name;
        c.message = "Choose " + (e.title.empty() ? e.title_id : e.title) + " on the " + emu->name + " shelf.";
        return c;
    }
    if (!valid_native_title(e.title_id)) {
        c.title = "Unknown title ID";
        c.message = "This entry has no valid PS4/PS5 title ID (CUSA/PPSA).";
        c.steps = "Rescan or retry identification in Mounting > Scan Diagnostics.";
        return c;
    }
    if (!installed_id(e.title_id)) {
        c.title = "Not installed";
        c.message = e.title_id + " is not in the console's installed-title list.";
        c.steps = "Install it or mount it with ShadowMountPlus, then choose Refresh Installed.";
        return c;
    }
    if (!runtime) {
        c.title = "Console runtime required";
        c.message = "Launching only works when Nexodion runs on the PS5.";
        c.steps = "Run the PS5 build of Nexodion.";
        return c;
    }
    c.can_launch = true;
    c.launch_title_id = e.title_id;
    c.title = "Ready to launch";
    return c;
}

LaunchCheck explain_launch_failure(const std::string& error) {
    LaunchCheck c; c.title = "Launch failed"; c.message = error;
    if (error.find("Missing native helper") != std::string::npos)
        c.steps = "Reinstall Nexodion: nexodion_launcher.elf is missing from the app folder.";
    else if (error.find("ELF loader") != std::string::npos)
        c.steps = "Start your ELF loader (port 9021), then try again.";
    else if (error.find("did not confirm") != std::string::npos)
        c.steps = "The console did not answer in time. Check whether the game opened; if not, close other apps and retry.";
    else
        c.steps = "Check that the title is still installed (Refresh Installed) and retry.";
    return c;
}

bool NativePackageDelegate::is_platform_supported() const {
#if defined(__PS5__) || defined(__PROSPERO__)
    return true;
#else
    return false;
#endif
}
std::string NativePackageDelegate::platform_status_description() const {
    return is_platform_supported()?"Native PKG relay; requires ELF loader on 127.0.0.1:9021":"Package installation requires a native PS5 runtime";
}
#if defined(__PS5__) || defined(__PROSPERO__)
namespace {
std::mutex relay_mutex;
struct Relay {
    int fd{-1};
    ~Relay(){if(fd>=0) close(fd);}
    bool send_bytes(const char* data,size_t length) {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
        while(length) {
            const auto left=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-std::chrono::steady_clock::now()).count();
            if(left<=0) return false;
            pollfd p{fd,POLLOUT,0};const int ready=poll(&p,1,static_cast<int>(left));
            if(ready<0 && errno==EINTR) continue;
            if(ready<=0) return false;
            const auto n=send(fd,data,length,0);
            if(n<0 && (errno==EINTR || errno==EAGAIN || errno==EWOULDBLOCK)) continue;
            if(n<=0) return false;
            data+=n;length-=static_cast<size_t>(n);
        }
        return true;
    }
    bool open_payload(const char* name,const std::string& request,std::string& error) {
        std::ifstream file(std::string("/app0/")+name,std::ios::binary);
        if(!file){error=std::string("Missing native helper: ")+name;return false;}
        fd=socket(AF_INET,SOCK_STREAM,0);if(fd<0){error="Cannot open ELF loader socket";return false;}
        const int flags=fcntl(fd,F_GETFL,0);
        if(flags<0 || fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0){error="Cannot configure ELF loader socket";return false;}
#if defined(SO_NOSIGPIPE)
        int one=1;setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof(one));
#endif
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(9021);
        inet_pton(AF_INET,"127.0.0.1",&address.sin_addr);
        if(connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))<0 && errno!=EINPROGRESS){error="ELF loader unavailable on port 9021";return false;}
        pollfd p{fd,POLLOUT,0};int code=0;socklen_t size=sizeof(code);
        if(poll(&p,1,5000)<=0 || getsockopt(fd,SOL_SOCKET,SO_ERROR,&code,&size)<0 || code!=0){error="ELF loader connection failed";return false;}
        char bytes[65536];size_t total=0;
        while(file.read(bytes,sizeof(bytes)) || file.gcount()>0) {
            total+=static_cast<size_t>(file.gcount());
            if(total>16*1024*1024 || !send_bytes(bytes,static_cast<size_t>(file.gcount()))){error="Native helper upload failed";return false;}
        }
        if(file.bad() || (!file.eof() && file.fail()) || total==0 || !send_bytes(request.data(),request.size())){error="Native helper request failed";return false;}
        return true;
    }
};
}
#endif
bool NativePackageDelegate::install_package(const InstallTask& task,std::function<bool(uint64_t,uint64_t)> progress,
                                           InstallErrorCode& code,std::string& error) {
    code=InstallErrorCode::UnsupportedPlatform;error=platform_status_description();
    if(!valid_native_package_path(task.source_path) || task.display_name.size()>=256 || task.display_name.find_first_of("\r\n")!=std::string::npos || task.display_name.find('\0')!=std::string::npos) {
        code=InstallErrorCode::InvalidPackage;error="Package must be a .pkg/.fpkg under /data, /mnt/usbN or /mnt/extN with a plain path";return false;
    }
#if defined(__PS5__) || defined(__PROSPERO__)
    {
        // Refuse before anything is sent when the package is unreadable or the console lacks space.
        const auto ready=check_install_readiness(task.source_path,apps_root_,task.title_id);
        for(const auto& c:ready.checks) if(c.blocking && !c.passed) {
            code=c.name=="Console space"?InstallErrorCode::InsufficientSpace:InstallErrorCode::FileUnreadable;
            error=c.detail+". "+c.fix;return false;
        }
    }
    std::lock_guard lock(relay_mutex);
    Relay relay;
    if(!relay.open_payload("nexodion_pkg_installer.elf","PKI1\n"+task.source_path+"\n"+task.display_name+"\n",error)){code=InstallErrorCode::SystemModuleMissing;return false;}
    std::string line,content_id;auto last=std::chrono::steady_clock::now();uint64_t written=0,total=task.total_bytes;
    while(std::chrono::steady_clock::now()-last<std::chrono::seconds(30)) {
        if(progress && !progress(written,total)) {code=InstallErrorCode::InstallInterrupted;error="Stopped monitoring; the console may continue an accepted package. Check Downloads before retrying.";return false;}
        pollfd p{relay.fd,POLLIN,0};const int ready=poll(&p,1,100);
        if(ready<0 && errno==EINTR) continue;
        if(ready<0) break;
        if(ready==0) continue;
        char buffer[1024];const auto n=recv(relay.fd,buffer,sizeof(buffer),0);
        if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR)) continue;
        if(n<=0) break;
        last=std::chrono::steady_clock::now();
        for(int i=0;i<n;++i) {
            if(buffer[i]!='\n'){if(line.size()>=2048){code=InstallErrorCode::IoError;error="Installer response exceeds limit";return false;}line+=buffer[i];continue;}
            if(line.starts_with("started ") && content_id.empty()) content_id=line.substr(8,48);
            if(line=="ok") {
                // The helper's "ok" is only the console accepting the job; confirm the title landed.
                std::string title_id=task.title_id;
                if(!valid_native_title(title_id) && content_id.size()>=16) title_id=content_id.substr(7,9);
                const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(verify_timeout_ms_);
                std::string verify_error;
                while(!verify_installed_title(apps_root_,title_id,verify_error) && std::chrono::steady_clock::now()<deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                if(verify_error.empty()){code=InstallErrorCode::None;error.clear();return true;}
                code=InstallErrorCode::InstallInterrupted;
                error="Console accepted the package but the installation could not be confirmed: "+verify_error+
                      ". Check Notifications > Downloads before retrying.";
                return false;
            }
            if(line.starts_with("fail ")){const auto f=classify_installer_failure(line);code=f.code;error=f.message;return false;}
            if(line.starts_with("p ")){std::istringstream stream(line.substr(2));uint64_t next_written=0,next_total=0;
                if(!(stream>>next_written>>next_total) || next_written>next_total){code=InstallErrorCode::IoError;error="Invalid installer progress";return false;}
                written=next_written;total=next_total;}
            line.clear();
        }
    }
    code=InstallErrorCode::InstallInterrupted;error="Installer stopped answering; check console Downloads before retrying";
#else
    (void)progress;
#endif
    return false;
}
bool native_launch_title(const std::string& title,std::string& error) {
    if(!valid_native_title(title)){error="Only validated PS4/PS5 installed title IDs can launch; legacy emulator adapters are not configured";return false;}
#if defined(__PS5__) || defined(__PROSPERO__)
    std::lock_guard lock(relay_mutex);Relay relay;
    if(!relay.open_payload("nexodion_launcher.elf","NXL1\n"+title+"\n",error)) return false;
    std::string line;const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(std::chrono::steady_clock::now()<deadline) {
        pollfd p{relay.fd,POLLIN,0};if(poll(&p,1,100)<=0) continue;
        char byte;const auto n=recv(relay.fd,&byte,1,0);
        if(n<0 && (errno==EINTR || errno==EAGAIN || errno==EWOULDBLOCK)) continue;
        if(n<=0) break;
        if(byte=='\n'){if(line=="ok"){error.clear();return true;}error=line;return false;}
        if(line.size()>=512) break;
        line+=byte;
    }
    error="Native launcher did not confirm launch";
#else
    error="Launching requires a native PS5 runtime";
#endif
    return false;
}
}