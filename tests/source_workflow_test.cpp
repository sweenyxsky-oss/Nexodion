#include "nexodion/ui_shell.hpp"
#include <fstream>
#include <iostream>
#include <cstdlib>
#define CHECK(x) do{if(!(x)){std::cerr<<"Failed " #x <<":"<<__LINE__<<'\n';std::exit(1);}}while(0)
using namespace nexodion;
void press(UiShell& shell,uint32_t button) {InputManager input;input.feed_raw_buttons(button);shell.handle_pad_input(input);}
void down(UiShell& shell,int n) {for(int i=0;i<n;++i) press(shell,pad_bits::kDown);}
int main(){
    const auto root=std::filesystem::temp_directory_path()/"nexodion_source_workflow_test";
    std::filesystem::remove_all(root);std::filesystem::create_directories(root);
    UiShell shell(nullptr,(root/"cache.json").string());shell.set_sources_path((root/"sources.json").string());
    shell.show_sources();press(shell,pad_bits::kCross);CHECK(shell.active_dialog()==DialogType::SourceEditor);
    down(shell,8);press(shell,pad_bits::kCross);CHECK(shell.sources().list().size()==1);
    SourceManager restored;CHECK(restored.load_file((root/"sources.json").string()));CHECK(restored.list().size()==1);
    CHECK(restored.list()[0].platform_paths.size()==5);
    CHECK(restored.list()[0].platform_paths.at(NavTab::PS5)!=restored.list()[0].platform_paths.at(NavTab::PS4));
    press(shell,pad_bits::kCross);CHECK(shell.active_dialog()==DialogType::SourceActions);
    down(shell,1);press(shell,pad_bits::kCross);CHECK(!shell.sources().list()[0].enabled);
    press(shell,pad_bits::kCross);down(shell,2);press(shell,pad_bits::kCross);CHECK(shell.active_dialog()==DialogType::SourceRemove);
    press(shell,pad_bits::kCross);CHECK(shell.sources().list().empty());CHECK(restored.load_file((root/"sources.json").string()));CHECK(restored.list().empty());
    shell.show_text_entry("Audit","search");down(shell,3);press(shell,pad_bits::kCross);CHECK(shell.search_query()=="Audit");
    shell.set_sources_path((root/"missing"/"sources.json"/"blocked").string());
    std::ofstream(root/"missing")<<"file";
    shell.show_source_editor();down(shell,8);press(shell,pad_bits::kCross);CHECK(shell.active_dialog()==DialogType::RecoveryGuidance);CHECK(shell.sources().list().empty());
    for(const auto& name:{"a","b"}) {
        std::filesystem::create_directories(root/name/"game"/"sce_sys");
        std::ofstream(root/name/"game"/"sce_sys"/"param.json")<<R"({"titleId":"PPSA12345","titleName":"Duplicate","category":"gd"})";
    }
    ScannerEngine scanner;ScanOptions options;options.roots={(root/"a").string(),(root/"b").string()};
    auto entries=scanner.scan(options);CHECK(entries.size()==2);CHECK(entries[0].dlcs.empty() && entries[1].dlcs.empty());
    options.root_identities[(root/"a").string()]={"internal","local",NavTab::PS2};
    options.root_identities[(root/"b").string()]={"nas","nfs",NavTab::PS1};
    entries=scanner.scan(options);CHECK(entries.size()==2);
    for(const auto& entry:entries){CHECK(entry.platform.has_value());CHECK(!entry.source_id.empty());CHECK(!entry.storage_type.empty());}
    SourceConfig paths;paths.id="internal";paths.mount_point=root.string();paths.platform_paths={{NavTab::PS2,(root/"a").string()},{NavTab::PS1,(root/"b").string()}};
    SourceManager grouped;CHECK(!grouped.add_source(paths));CHECK(grouped.active_scan_roots().size()==2);
    SourceManager copy;CHECK(copy.load_json(grouped.to_json()));CHECK(copy.list()[0].platform_paths==paths.platform_paths);
    paths.platform_paths[NavTab::PS5]="/outside-device";CHECK(validate_source_config(paths));
    paths.platform_paths.erase(NavTab::PS5);
    UiShell flow(nullptr,(root/"flow-cache.json").string());CHECK(!flow.sources().add_source(paths));
    bool finished=false;flow.trigger_scan(true,[&](bool ok){CHECK(ok);finished=true;});
    for(int i=0;i<200 && !finished;++i){flow.tick();std::this_thread::sleep_for(std::chrono::milliseconds(5));}
    CHECK(finished);flow.select_tab(NavTab::PS2);CHECK(flow.filtered_entries().size()==1);
    flow.select_tab(NavTab::PS1);CHECK(flow.filtered_entries().size()==1);
    SourceConfig overlap;overlap.id="overlap";overlap.mount_point=(root/"a"/"game").string();CHECK(!flow.sources().add_source(overlap));
    bool rejected=false;flow.trigger_scan(true,[&](bool ok){rejected=!ok;});CHECK(rejected);CHECK(flow.active_dialog()==DialogType::RecoveryGuidance);
    IndexCache cache((root/"multi.json").string());CHECK(cache.save_atomic(entries,(root/"a").string()));
    CacheHeader header;CHECK(cache.load(entries,&header));CHECK(header.source_roots.size()==2);
    CHECK(cache.save_atomic({},(root/"a").string(),nullptr,{(root/"a").string(),(root/"b").string()}));
    std::vector<GameEntry> empty;CHECK(cache.load(empty,&header));CHECK(empty.empty());CHECK(header.source_roots.size()==2);
    std::filesystem::last_write_time(root/"b",std::filesystem::file_time_type::clock::now()+std::chrono::seconds(2));
    CHECK(!cache.is_fresh((root/"a").string()));
    const auto before=entries;std::ofstream(root/"multi.json")<<R"({"entries":[{},7]})";
    CHECK(!cache.load(entries));CHECK(entries.size()==before.size());
    std::filesystem::remove_all(root);std::cout<<"Source CRUD persistence rollback, search, duplicate roots and cache integrity passed\n";
}