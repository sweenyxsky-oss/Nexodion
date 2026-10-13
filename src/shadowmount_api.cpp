#include "nexodion/shadowmount_api.hpp"
#include "nexodion/native_services.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <sstream>
#include <algorithm>

namespace nexodion {
namespace {
struct Socket {int fd{-1};~Socket(){if(fd>=0) close(fd);}};
}
ApiResponse ShadowMountApi::request(const std::string& route,const nlohmann::json& body,std::chrono::milliseconds timeout) const {
    ApiResponse result;
    auto fail=[&](const std::string& message){result.error=message;return result;};
    if(!route.starts_with("/api/v1/") || route.find_first_of("\r\n ")!=std::string::npos || !body.is_object())
        return fail("Invalid daemon request");
    if(timeout.count()<=0 || timeout.count()>60000 || port_==0) return fail("Invalid daemon timeout or port");
    Socket socket_handle{socket(AF_INET,SOCK_STREAM,0)};
    const int fd=socket_handle.fd;
    if(fd<0) return fail("Cannot open daemon socket");
    const int flags=fcntl(fd,F_GETFL,0);
    if(flags<0 || fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0) return fail("Cannot configure daemon socket");
#if defined(SO_NOSIGPIPE)
    int one=1;setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof(one));
#endif
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(port_);
    if(inet_pton(AF_INET,host_.c_str(),&address.sin_addr)!=1) return fail("Daemon host must be an IPv4 address");
    const auto deadline=std::chrono::steady_clock::now()+timeout;
    auto wait=[&](short event){
        for(;;) {
            const auto left=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-std::chrono::steady_clock::now()).count();
            if(left<=0) return false;
            pollfd p{fd,event,0};const int n=poll(&p,1,static_cast<int>(left));
            if(n<0 && errno==EINTR) continue;
            return n>0 && (p.revents&(event|POLLHUP))!=0 && (p.revents&POLLNVAL)==0;
        }
    };
    if(connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address))<0 && errno!=EINPROGRESS)
        return fail("Daemon connection refused");
    if(!wait(POLLOUT)) return fail("Daemon connection timed out");
    int socket_error=0;socklen_t length=sizeof(socket_error);
    if(getsockopt(fd,SOL_SOCKET,SO_ERROR,&socket_error,&length)<0 || socket_error!=0) return fail("Daemon connection failed");
    const auto json=body.dump();
    if(json.size()>65536) return fail("Daemon request is too large");
    const std::string request="POST "+route+" HTTP/1.1\r\nHost: "+host_+":"+std::to_string(port_)+
        "\r\nContent-Type: application/json\r\nAccept: application/json\r\nContent-Length: "+std::to_string(json.size())+
        "\r\nConnection: close\r\n\r\n"+json;
    size_t offset=0;
    while(offset<request.size()) {
        if(!wait(POLLOUT)) return fail("Daemon send timed out");
#if defined(MSG_NOSIGNAL)
        const auto n=send(fd,request.data()+offset,request.size()-offset,MSG_NOSIGNAL);
#else
        const auto n=send(fd,request.data()+offset,request.size()-offset,0);
#endif
        if(n<0 && (errno==EINTR || errno==EAGAIN || errno==EWOULDBLOCK)) continue;
        if(n<=0) return fail("Daemon send failed");
        offset+=static_cast<size_t>(n);
    }
    std::string raw;bool closed=false;
    size_t header_end=std::string::npos, expected=0;bool has_length=false;
    for(;;) {
        if(!wait(POLLIN)) return fail("Daemon response timed out");
        char buffer[8192];const auto n=recv(fd,buffer,sizeof(buffer),0);
        if(n<0 && (errno==EINTR || errno==EAGAIN || errno==EWOULDBLOCK)) continue;
        if(n<0) return fail("Daemon response failed");
        if(n==0){closed=true;break;}
        raw.append(buffer,static_cast<size_t>(n));
        if(raw.size()>4*1024*1024) return fail("Daemon response exceeds limit");
        if(header_end==std::string::npos) {
            header_end=raw.find("\r\n\r\n");
            if(header_end==std::string::npos && raw.size()>16384) return fail("Daemon headers exceed limit");
            if(header_end!=std::string::npos) {
                std::istringstream headers(raw.substr(0,header_end));std::string line;
                std::getline(headers,line);std::istringstream status(line);std::string protocol;
                status>>protocol>>result.http_status;
                if((protocol!="HTTP/1.0" && protocol!="HTTP/1.1") || result.http_status<100 || result.http_status>599)
                    return fail("Invalid HTTP status");
                while(std::getline(headers,line)) {
                    if(!line.empty() && line.back()=='\r') line.pop_back();
                    const auto colon=line.find(':');if(colon==std::string::npos) return fail("Malformed daemon header");
                    auto key=line.substr(0,colon);
                    std::transform(key.begin(),key.end(),key.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
                    if(key=="transfer-encoding") return fail("Unsupported daemon transfer encoding");
                    if(key=="content-length") {
                        if(has_length) return fail("Duplicate response length");
                        try {size_t used=0;const auto text=line.substr(colon+1);expected=std::stoull(text,&used);
                            if(text.find_first_not_of(" \t",used)!=std::string::npos || expected>4*1024*1024) return fail("Invalid response length");}
                        catch(...){return fail("Invalid response length");}
                        has_length=true;
                    }
                }
            }
        }
        if(header_end!=std::string::npos && has_length && raw.size()>=header_end+4+expected) break;
    }
    if(header_end==std::string::npos) return fail("Incomplete daemon headers");
    const auto text=raw.substr(header_end+4);
    if((has_length && text.size()!=expected) || (!has_length && !closed)) return fail("Incomplete daemon body");
    try {
        result.data=nlohmann::json::parse(text);
        if(!result.data.is_object() || !result.data.contains("status") || !result.data["status"].is_number_integer())
            return fail("Invalid daemon response schema");
        result.daemon_status=result.data["status"].get<int>();
        result.ok=result.http_status>=200 && result.http_status<300 && result.daemon_status==0;
        if(!result.ok) result.error="Daemon rejected request (HTTP "+std::to_string(result.http_status)+", status "+
            std::to_string(result.daemon_status)+"): "+text;
    } catch(const std::exception& e){return fail(std::string("Invalid daemon JSON: ")+e.what());}
    return result;
}
ApiResponse ShadowMountApi::mount(const std::string& title,bool writable) const {
    if(!valid_native_title(title)) {ApiResponse r;r.error="Invalid PS4/PS5 title ID";return r;}
    return request("/api/v1/games/mount",{{"title_id",title},{"mode",writable?"rw":"ro"}});
}
ApiResponse ShadowMountApi::unmount(const std::string& title) const {
    if(!valid_native_title(title)) {ApiResponse r;r.error="Invalid PS4/PS5 title ID";return r;}
    return request("/api/v1/games/unmount",{{"title_id",title}});
}
ApiResponse ShadowMountApi::uninstall(const std::string& title) const {
    if(!valid_native_title(title)) {ApiResponse r;r.error="Invalid PS4/PS5 title ID";return r;}
    return request("/api/v1/games/uninstall",{{"title_id",title}});
}
}