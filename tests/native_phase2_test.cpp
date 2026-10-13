#include "nexodion/ui_shell.hpp"
#include "nexodion/native_runtime.hpp"
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <thread>
#include <cmath>
#define CHECK(x) do {if(!(x)){std::cerr<<"Failed: " #x << ':' << __LINE__ << '\n';std::exit(1);}}while(0)
using namespace nexodion;
struct AuditCanvas final : NativeCanvas {
    int fills=0, texts=0;
    bool brand=false;
    void fill(Rect r,theme::Rgba) override {CHECK(std::isfinite(r.x));CHECK(r.w>=0);++fills;}
    void text(Rect r,std::string_view s,theme::Rgba,int) override {CHECK(r.w>0);++texts;if(s=="Nexodion") brand=true;}
    bool image(Rect,const std::string&) override {return false;}
};
int main() {
    const auto root=std::filesystem::temp_directory_path()/"nexodion_phase2_fixture";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root/"PS5"/"sce_sys");
    std::ofstream(root/"PS5"/"sce_sys"/"param.json") << R"({"titleId":"PPSA12345","titleName":"Audit title","category":"gd"})";
    UiShell shell(nullptr,(root/"cache.json").string());
    SourceConfig config;config.id="local";config.mount_point=root.string();
    CHECK(!shell.sources().add_source(config));
    shell.set_scan_path(root.string());
    bool done=false;
    shell.trigger_scan(true,[&](bool ok){CHECK(ok);done=true;});
    CHECK(shell.is_scanning());
    for(int i=0;i<200 && !done;++i) {shell.tick();std::this_thread::sleep_for(std::chrono::milliseconds(5));}
    CHECK(done);CHECK(shell.all_entries().size()==1);
    CHECK(shell.all_entries()[0].title_id=="PPSA12345");
    InputManager input;
    input.feed_raw_buttons(pad_bits::kR1);shell.handle_pad_input(input);
    CHECK(shell.navigation().active()==NavTab::PS5);CHECK(shell.filtered_entries().size()==1);
    input.feed_raw_buttons(0);input.feed_raw_buttons(pad_bits::kR1);shell.handle_pad_input(input);
    CHECK(shell.navigation().active()==NavTab::PS4);CHECK(shell.filtered_entries().empty());
    GameEntry ambiguous;ambiguous.title_id="SLUS12345";
    shell.set_entries({ambiguous});shell.select_tab(NavTab::PS1);CHECK(shell.filtered_entries().empty());
    ambiguous.platform=NavTab::PS2;shell.set_entries({ambiguous});
    shell.select_tab(NavTab::PS2);CHECK(shell.filtered_entries().size()==1);
    CHECK(GameEntry::from_json(ambiguous.to_json()).platform==NavTab::PS2);
    shell.select_tab(NavTab::PS1);CHECK(shell.filtered_entries().empty());
    input.feed_raw_buttons(0);input.feed_raw_buttons(pad_bits::kOptions);shell.handle_pad_input(input);
    CHECK(shell.active_dialog()==DialogType::Settings);
    input.feed_raw_buttons(0);input.feed_raw_buttons(pad_bits::kCross);shell.handle_pad_input(input);
    CHECK(shell.active_dialog()==DialogType::Sources);
    AuditCanvas canvas;paint_native_shell(canvas,shell,false);
    CHECK(canvas.brand && canvas.fills>5 && canvas.texts>8);
    shell.dismiss_dialog();shell.select_tab(NavTab::PS2);
    paint_native_shell(canvas,shell,true);
    InputManager analog;
    analog.feed_raw_buttons(0,1,0);CHECK(analog.action_triggered(LogicalAction::NavRight));
    analog.feed_raw_buttons(0,1,0);CHECK(!analog.action_triggered(LogicalAction::NavRight));
    analog.feed_raw_buttons(0);CHECK(analog.is_button_released(pad_bits::kRight));
    SourceManager manager;CHECK(!manager.add_source(config));
    const auto before=manager.to_json();
    CHECK(!manager.load_json(R"({"sources":[{"id":7}]})"));CHECK(manager.to_json()==before);
    CHECK(!manager.load_json(R"({"version":2,"sources":[]})"));CHECK(manager.to_json()==before);
    {
        UiShell unavailable(nullptr,(root/"absent-cache").string());
        SourceConfig missing;missing.id="missing";missing.mount_point=(root/"missing").string();
        CHECK(!unavailable.sources().add_source(missing));
        unavailable.show_startup_diagnostics({});
        bool rejected=false;
        unavailable.trigger_scan(true,[&](bool ok){rejected=!ok;});
        CHECK(rejected);CHECK(unavailable.active_dialog()==DialogType::StartupDiagnostics);
    }
    std::filesystem::remove_all(root);
    std::cout<<"Phase 2: real fixture scan -> UI thread -> tabs -> settings -> native painter passed\n";
}