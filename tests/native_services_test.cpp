#include "nexodion/native_services.hpp"
#include "nexodion/shadowmount_api.hpp"
#include "nexodion/ui_shell.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <thread>
#include <iostream>
#include <cstdlib>
#include <fstream>
#include <atomic>
#define CHECK(x) do{if(!(x)){std::cerr<<"Failed " #x <<":"<<__LINE__<<'\n';std::exit(1);}}while(0)
using namespace nexodion;
struct Server {
    int fd;uint16_t port;std::thread thread;std::string request;
    Server(const std::string& body,int status=200,bool truncate=false) {
        fd=socket(AF_INET,SOCK_STREAM,0);CHECK(fd>=0);
        sockaddr_in a{};a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        CHECK(bind(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a))==0);
        socklen_t size=sizeof(a);getsockname(fd,reinterpret_cast<sockaddr*>(&a),&size);port=ntohs(a.sin_port);
        CHECK(listen(fd,1)==0);
        thread=std::thread([this,body,status,truncate] {
            int client=accept(fd,nullptr,nullptr);if(client<0)return;
            char buffer[1024];
            while(request.find("\r\n\r\n")==std::string::npos) {auto n=recv(client,buffer,sizeof(buffer),0);if(n<=0)break;request.append(buffer,static_cast<size_t>(n));}
            const auto response="HTTP/1.1 "+std::to_string(status)+" OK\r\nContent-Type: application/json\r\nContent-Length: "+
                std::to_string(body.size()+(truncate?10:0))+"\r\nConnection: close\r\n\r\n"+body;
            for(size_t off=0;off<response.size();) {const auto n=send(client,response.data()+off,response.size()-off,0);if(n<=0)break;off+=static_cast<size_t>(n);}
            close(client);
        });
    }
    ~Server(){if(thread.joinable()) thread.join();close(fd);}
};
struct CancellingDelegate final : IInstallDelegate {
    InstallPipeline* pipeline{};int calls=0;
    bool install_package(const InstallTask&,std::function<bool(uint64_t,uint64_t)> progress,InstallErrorCode& code,std::string&) override {
        if(++calls==1) pipeline->cancel_active_task();
        if(!progress(1,1)){code=InstallErrorCode::Cancelled;return false;}
        code=InstallErrorCode::None;return true;
    }
};
int main() {
    CHECK(valid_native_title("PPSA12345"));CHECK(!valid_native_title("FAKE12345"));CHECK(!valid_native_title("PPSA12\n45"));
    CHECK(valid_native_package_path("/mnt/usb0/game.pkg"));CHECK(valid_native_package_path("/data/a/game.fpkg"));
    CHECK(!valid_native_package_path("/mnt/usbmalicious/game.pkg"));CHECK(!valid_native_package_path("/data/../system/game.pkg"));
    CHECK(!valid_native_package_path("/data/game.pkg\n"));
    NativePackageDelegate delegate;InstallTask task;task.source_path="/data/a.pkg";task.display_name="Game";
    InstallErrorCode code;std::string error;
    CHECK(!delegate.install_package(task,nullptr,code,error));CHECK(code==InstallErrorCode::UnsupportedPlatform);
    CHECK(!native_launch_title("PPSA12345",error));
    {Server server(R"({"status":0})");auto r=ShadowMountApi("127.0.0.1",server.port).mount("PPSA12345");CHECK(r.ok);
        server.thread.join();CHECK(server.request.starts_with("POST /api/v1/games/mount"));CHECK(server.request.find("\"mode\":\"ro\"")!=std::string::npos);}
    {Server server(R"({"status":1,"error":"refused"})");CHECK(!ShadowMountApi("127.0.0.1",server.port).uninstall("PPSA12345").ok);}
    {Server server(R"({"status":0})",403);CHECK(!ShadowMountApi("127.0.0.1",server.port).unmount("PPSA12345").ok);}
    {Server server(R"({"status":0})",200,true);CHECK(!ShadowMountApi("127.0.0.1",server.port).games().ok);}
    {Server server(R"({"foo":0})");CHECK(!ShadowMountApi("127.0.0.1",server.port).games().ok);}
    auto cancelling=std::make_shared<CancellingDelegate>();InstallPipeline pipeline(cancelling);cancelling->pipeline=&pipeline;
    GameItem item;item.filename="a.pkg";item.absolute_path="/data/a.pkg";
    auto a=pipeline.queue_item(item,"PPSA00001");item.filename="b.pkg";item.absolute_path="/data/b.pkg";
    auto b=pipeline.queue_item(item,"PPSA00002");CHECK(pipeline.process_one_step());CHECK(pipeline.process_one_step());
    CHECK(pipeline.get_task(a)->status==InstallStatus::Cancelled);CHECK(pipeline.get_task(b)->status==InstallStatus::Completed);
    UiShell shell(nullptr,"/tmp/nexodion_service_fixture_missing");GameEntry entry;entry.title_id="PPSA12345";entry.title="Source file";
    shell.set_entries({entry});CHECK(shell.filtered_entries().empty());CHECK(!shell.is_title_installed(entry.title_id));
    shell.select_tab(NavTab::PS5);CHECK(shell.filtered_entries().size()==1);
    {Server version(R"({"status":0,"api_version":1,"shadowmount_version":"1.7"})");
        auto daemon=std::make_shared<DaemonLock>("127.0.0.1",version.port);CHECK(daemon->probe().success);
        Server games(R"({"status":0,"games":[{"title_id":"PPSA12345","name":"Installed fixture","installed_pkg":true,"source_type":"pkg","size_bytes":1024}]})");
        daemon->set_target("127.0.0.1",games.port);UiShell live(daemon,"/tmp/nexodion_installed_fixture_missing");live.refresh_installed_titles();
        for(int i=0;i<200 && live.filtered_entries().empty();++i){live.tick();std::this_thread::sleep_for(std::chrono::milliseconds(5));}
        CHECK(live.filtered_entries().size()==1);CHECK(live.filtered_entries()[0].title=="Installed fixture");
        CHECK(live.filtered_entries()[0].total_size_bytes==1024);CHECK(live.is_title_installed("PPSA12345"));
        games.thread.join();CHECK(games.request.find("\"include_size\":true")!=std::string::npos);
    }
    {InstallPipeline concurrent(std::make_shared<MockInstallDelegate>());std::atomic<int> accepted{0};std::vector<std::thread> workers;
        for(int i=0;i<20;++i)workers.emplace_back([&]{if(!concurrent.queue_item(item,"PPSA00002").empty())++accepted;});
        for(auto& worker:workers)worker.join();
        CHECK(accepted==1);
    }
    const auto root=std::filesystem::temp_directory_path()/"nexodion_sfo_audit";
    std::filesystem::create_directories(root/"game"/"sce_sys");
    std::vector<unsigned char> bytes(68);std::string keys,data;
    std::vector<std::pair<std::string,std::string>> fields={{"TITLE_ID","CUSA12345"},{"TITLE","SFO Game"},{"CATEGORY","gd"}};
    auto put16=[&](size_t p,uint32_t v){bytes[p]=v&255;bytes[p+1]=(v>>8)&255;};
    auto put32=[&](size_t p,uint32_t v){put16(p,v);put16(p+2,v>>16);};
    bytes[1]='P';bytes[2]='S';bytes[3]='F';put32(4,0x101);put32(8,68);put32(16,3);
    for(size_t i=0;i<fields.size();++i){size_t p=20+i*16;put16(p,keys.size());put16(p+2,0x0204);
        put32(p+4,fields[i].second.size()+1);put32(p+8,fields[i].second.size()+1);put32(p+12,data.size());
        keys+=fields[i].first;keys+='\0';data+=fields[i].second;data+='\0';}
    put32(12,68+keys.size());bytes.insert(bytes.end(),keys.begin(),keys.end());bytes.insert(bytes.end(),data.begin(),data.end());
    const auto sfo=root/"game"/"sce_sys"/"param.sfo";
    auto write=[&]{std::ofstream file(sfo,std::ios::binary);file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());};write();
    CHECK(parse_param_sfo_file(sfo).valid);CHECK(parse_param_sfo_file(sfo).title_name=="SFO Game");
    ScannerEngine scanner;ScanOptions options;options.roots={root.string()};CHECK(scanner.scan(options).size()==1);
    put32(12,0xffffffff);write();CHECK(!parse_param_sfo_file(sfo).valid);std::filesystem::remove_all(root);
    std::cout<<"Native service refusal, POST protocol, truncation, cancel isolation and installed-state tests passed\n";
}