#include "nexodion/native_runtime.hpp"
#include "nexodion/ui_shell.hpp"
#include <algorithm>
#include "ps5-homebrew-ui/tween.hpp"
#include <chrono>

namespace nexodion {
using namespace theme;
namespace {
void panel(NativeCanvas& c, Rect r) { c.fill(r, kCard); }
void line(NativeCanvas& c, float x, float y, float w, const std::string& s, Rgba color = kTextPrimary, int size = 24) {
    c.text({x,y,w,static_cast<float>(size + 10)}, s, color, size);
}
void modal(NativeCanvas& c, const UiShell& shell) {
    const auto& d = shell.dialog_model();
    c.fill({0,0,1920,1080}, kModalOverlay);
    panel(c, {280,140,1360,800});
    line(c, 320,178,1280,d.title,kTextPrimary,32);
    c.text({320,238,1280,110},d.message,kTextSecondary);
    if(d.type==DialogType::Sources && d.selected_option>=0) {
        const auto sources=shell.sources().list();
        const auto index=static_cast<size_t>(d.selected_option);
        if(index<sources.size()) {
            const auto& source=sources[index];const auto state=shell.sources().state_of(source.id);
            std::string details=source.display_name+" ["+(source.kind==SourceKind::Local?std::string("Internal"):to_string(source.kind))+"] "+(state?to_string(*state):"unknown")+"\n"+source.mount_point+"\n";
            for(const auto& [platform,path]:source.platform_paths)details+=std::string(to_string(platform))+": "+(path.empty()?"Not configured":path)+"\n";
            c.text({320,358,1280,320},details,kTextSecondary,20);
        }
    } else c.text({320,358,1280,320},d.secondary_text,kTextSecondary,20);
    // Long diagnostics scroll in their existing text report; options never overlap it.
    float y = 700;
    const size_t start=d.selected_option>4 ? static_cast<size_t>(d.selected_option-4) : 0;
    for (size_t i=start; i<d.options.size() && i<start+5; ++i) {
        if (static_cast<int>(i)==d.selected_option) c.fill({310,y-4,1300,38},kCardHover);
        line(c,330,y,1260,d.options[i],static_cast<int>(i)==d.selected_option?kPlasmaLilac:kTextPrimary,20);
        y+=40;
    }
}
}
void paint_native_shell(NativeCanvas& c, const UiShell& shell, bool connected) {
    c.fill({0,0,1920,1080}, kBackground);
    c.fill({0,0,1920,154}, kBackgroundAlt);
    line(c,64,28,500,"Nexodion",kTextPrimary,36);
    line(c,1270,34,586,connected?"Controller connected":"Controller disconnected",connected?kSuccess:kWarning,20);
    float x=64;
    for (auto tab:kNavTabs) {
        const bool active=tab==shell.navigation().active();
        line(c,x,98,180,to_string(tab),active?kPlasmaLilac:kTextSecondary,24);
        if(active) c.fill({x,142,150,4}, kSelectedBorder);
        x+=205;
    }
    line(c,64,166,1450,shell.current_scan_path(),kTextMuted,20);
    line(c,1580,166,275,std::to_string(shell.filtered_entries().size())+" titles",kTextSecondary,20);
    if(shell.is_scanning()) {
        const auto seconds=std::chrono::duration<float>(std::chrono::steady_clock::now().time_since_epoch()).count();
        const float fraction=seconds-std::floor(seconds);
        const float x=64+hui::tween::cubic_in_out(fraction)*1572;
        c.fill({x,202,220,4},kInProgress);
    }

    auto cards=shell.get_current_view_cards();
    std::stable_sort(cards.begin(), cards.end(), [](const auto& a,const auto& b){return a.z_order<b.z_order;});
    // Content clipping is owned by the SDL backend. Keep geometry in the unframed content band.
    for (const auto& card:cards) {
        const float w=card.width*card.scale, h=card.height*card.scale;
        const float offset = shell.view_layout() == ViewLayout::BlockGrid ? 250.0f : 120.0f;
        Rect r{card.x-w/2,card.y-h/2+offset,w,h};
        if(r.y<208 || r.y+r.h>960 || r.x+r.w<0 || r.x>1920) continue;
        if(card.is_focused) c.fill({r.x-4,r.y-4,r.w+8,r.h+8},kSelectedBorder);
        panel(c,r);
        const Rect art{r.x+12,r.y+12,r.w-24,r.h-136};
        if(!c.image(art,card.cover_path)) {
            c.fill(art,kBackgroundAlt);
            c.text({art.x+16,art.y+art.h/2-20,art.w-32,70},card.title_id.empty()?"No artwork":card.title_id,kTextMuted,20);
        }
        c.text({r.x+16,r.y+r.h-118,r.w-32,50},(card.favorite?"[Fav] ":"")+card.title,kTextPrimary,24);
        line(c,r.x+16,r.y+r.h-68,r.w-32,card.metadata_line,kTextMuted,16);
        line(c,r.x+16,r.y+r.h-42,r.w-32,card.format_badge+"  "+card.formatted_size,kTextSecondary,18);
    }
    if(cards.empty()) {
        line(c,64,380,1750,shell.is_scanning()?"Scanning library...":"No titles in this view",kTextSecondary,32);
    }

    if(const auto* inspector=shell.inspector()) {
        c.fill({0,208,1920,752}, kBackground);
        line(c,64,232,1792,inspector->title(),kTextPrimary,32);
        line(c,64,282,1792,inspector->get_outside_header_info(),kTextSecondary,20);
        float tx=64;
        for(auto tab:{InspectorTab::Game,InspectorTab::Dlc,InspectorTab::Update}) {
            line(c,tx,335,300,to_string(tab),tab==inspector->active_tab()?kPlasmaLilac:kTextSecondary,24);
            tx+=330;
        }
        const auto& items=inspector->current_tab_items();
        const size_t start=inspector->selected_item_index()>5?inspector->selected_item_index()-5:0;
        for(size_t i=start;i<items.size() && i<start+6;++i) {
            float y=390+static_cast<float>(i-start)*62;
            if(i==inspector->selected_item_index()) c.fill({60,y-4,1796,54},kCardHover);
            line(c,80,y,1750,items[i].formatted_summary_line(),i==inspector->selected_item_index()?kPlasmaLilac:kTextPrimary,20);
        }
        float ay=800;
        const auto actions=inspector->available_action_labels();
        for(size_t i=0;i<actions.size() && i<3;++i) {
            line(c,64,ay,1750,actions[i],i==inspector->selected_action_button()?kPlasmaLilac:kTextSecondary,20);
            ay+=36;
        }
    }
    if(shell.active_dialog()!=DialogType::None) modal(c,shell);
    c.fill({0,972,1920,108}, kBackgroundAlt);
    float px=64, py=988;
    for(const auto& p:shell.get_contextual_prompts()) {
        const std::string label=std::string(to_glyph_name(p.glyph))+" "+p.action_label;
        const float width=std::min(700.0f,static_cast<float>(label.size())*10+28);
        if(px+width>1856) {px=64;py+=36;}
        if(py>1040) break;
        line(c,px,py,width,label,p.enabled?kTextSecondary:kDisabled,18);
        px+=width;
    }
}
} // namespace nexodion