#include "nexodion/ui_shell.hpp"
#include <algorithm>
#include <chrono>

namespace nexodion {
// Last character range includes SMB password symbols; secrets stay in memory.
namespace { constexpr std::string_view characters="abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 /._-:@#$%&*+!?=~|"; }

bool UiShell::persist_source_change(const std::string& before) {
    std::string error;
    if(sources_.save_file(sources_path_, &error)) {show_sources();return true;}
    sources_.load_json(before);
    show_recovery_guidance("Source not saved",error,"Previous configuration has been restored.");
    return false;
}

void UiShell::show_source_editor(const std::string& id) {
    source_edit_id_=id;
    source_draft_={};
    source_draft_secret_=sources_.session_secret(id);
    if(id.empty()) {
        source_draft_.id="source-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        source_draft_.display_name="Internal storage";source_draft_.mount_point="/data";
        for(auto tab:kNavTabs)if(tab!=NavTab::Installed)source_draft_.platform_paths[tab]="/data/nexodion/libraries/"+std::string(to_string(tab));
    } else {
        const auto configs=sources_.list();
        auto it=std::find_if(configs.begin(),configs.end(),[&](const auto& c){return c.id==id;});
        if(it==configs.end()){show_sources();return;}
        source_draft_=*it;
    }
    dialog_model_={};dialog_model_.type=DialogType::SourceEditor;
    update_source_editor();
}

void UiShell::update_source_editor() {
    const int selected=dialog_model_.selected_option;
    dialog_model_={};dialog_model_.type=DialogType::SourceEditor;
    dialog_model_.title=source_edit_id_.empty()?"Add Device":"Device Mounting";
    dialog_model_.options={"Name: "+source_draft_.display_name,"Type: "+std::string(source_draft_.kind==SourceKind::Local?"Internal":to_string(source_draft_.kind)),
        "Device path: "+source_draft_.mount_point,"Host: "+source_draft_.host,"Remote path: "+source_draft_.remote_path,
        "Username: "+source_draft_.username,"Password: "+(source_draft_secret_.empty()?std::string("Not set"):std::string("Set")),
        source_draft_.enabled?"Enabled":"Disabled","Save","Cancel"};
    for(auto tab:kNavTabs)if(tab!=NavTab::Installed) {
        const auto it=source_draft_.platform_paths.find(tab);
        dialog_model_.options.push_back(std::string(to_string(tab))+" path: "+(it==source_draft_.platform_paths.end()?"Not configured":it->second));
    }
    dialog_model_.selected_option=std::clamp(selected,0,static_cast<int>(dialog_model_.options.size())-1);
}

void UiShell::show_text_entry(const std::string& initial,const std::string& purpose) {
    text_value_=initial;text_purpose_=purpose;text_character_=0;
    dialog_model_={};dialog_model_.type=DialogType::TextEntry;
    update_text_entry();
}
void UiShell::update_text_entry() {
    dialog_model_.title=text_purpose_=="search"?"Search Library":text_purpose_=="diag_search"?"Search Diagnostics":text_purpose_=="diag_export_path"?"Export Report Path":text_purpose_=="launcher_id"?"Launcher App ID":text_purpose_=="launcher_search"?"Search Installed Apps":text_purpose_=="backup_path"?"Back Up Settings To":text_purpose_=="restore_path"?"Restore Settings From":text_purpose_=="logs_path"?"Export Logs To":"Edit Source Field";
    dialog_model_.message=text_value_+"_";
    dialog_model_.options={std::string("Character: [")+characters[static_cast<size_t>(text_character_)]+"]","Append","Delete","Apply","Cancel"};
}

bool UiShell::handle_source_workflow(const InputManager& input) {
    const auto type=active_dialog();
    if(type!=DialogType::SourceEditor && type!=DialogType::SourceActions && type!=DialogType::SourceRemove && type!=DialogType::TextEntry) return false;
    const bool settings_text=type==DialogType::TextEntry && (text_purpose_=="backup_path" || text_purpose_=="restore_path" || text_purpose_=="logs_path");
    if(input.action_triggered(LogicalAction::Cancel)) {
        if(settings_text) show_settings();
        else if(type==DialogType::TextEntry && text_purpose_=="diag_search") show_mounting_diagnostics();
        else if(type==DialogType::TextEntry && text_purpose_=="diag_export_path") show_mounting_diagnostics();
        else if(type==DialogType::TextEntry && text_purpose_=="launcher_id") show_launcher_options(launcher_platform_);
        else if(type==DialogType::TextEntry && text_purpose_=="launcher_search") show_launcher_picker();
        else if(type==DialogType::TextEntry && text_purpose_!="search") {dialog_model_.selected_option=0;update_source_editor();}
        else if(type==DialogType::TextEntry) dismiss_dialog();
        else show_sources();
        return true;
    }
    const int count=static_cast<int>(dialog_model_.options.size());
    if(count==0) return true;
    if(input.action_triggered(LogicalAction::NavUp) || input.action_triggered(LogicalAction::NavDown)) {
        const int step=input.action_triggered(LogicalAction::NavDown)?1:-1;
        dialog_model_.selected_option=(dialog_model_.selected_option+step+count)%count;return true;
    }
    if(type==DialogType::TextEntry && (input.action_triggered(LogicalAction::NavLeft)||input.action_triggered(LogicalAction::NavRight))) {
        const int step=input.action_triggered(LogicalAction::NavRight)?1:-1;
        text_character_=(text_character_+step+static_cast<int>(characters.size()))%static_cast<int>(characters.size());update_text_entry();return true;
    }
    if(!input.action_triggered(LogicalAction::Confirm)) return true;
    const int selected=dialog_model_.selected_option;
    if(type==DialogType::TextEntry) {
        if(selected<=1 && text_value_.size()<4096) text_value_+=characters[static_cast<size_t>(text_character_)];
        else if(selected==2 && !text_value_.empty()) text_value_.pop_back();
        else if(selected==3) {
            if(settings_text){apply_settings_path(text_purpose_,text_value_);return true;}
            if(text_purpose_=="search"){set_search_query(text_value_);dismiss_dialog();return true;}
            if(text_purpose_=="diag_search"){diag_search_=text_value_;show_mounting_diagnostics();return true;}
            if(text_purpose_=="launcher_search"){launcher_search_=text_value_;show_launcher_picker();return true;}
            if(text_purpose_=="launcher_id"){
                std::string error;
                if(text_value_.empty() || !set_launcher(launcher_platform_,text_value_,&error))
                    show_recovery_guidance("App ID not saved",text_value_.empty()?"No app ID entered":error,"Enter an ID such as PPSA12345.");
                else show_launcher_options(launcher_platform_);
                return true;
            }
            if(text_purpose_=="diag_export_path"){
                std::string error;
                if(export_filtered_diagnostics(text_value_.empty()?diagnostics_export_path_:text_value_,&error))
                    show_recovery_guidance("Report exported",text_value_.empty()?diagnostics_export_path_:text_value_,"");
                else show_recovery_guidance("Report not exported",error,"Check that the folder is writable.");
                return true;
            }
            if(text_purpose_=="name") source_draft_.display_name=text_value_;
            else if(text_purpose_=="mount") source_draft_.mount_point=text_value_;
            else if(text_purpose_=="host") source_draft_.host=text_value_;
            else if(text_purpose_=="remote") source_draft_.remote_path=text_value_;
            else if(text_purpose_=="username") source_draft_.username=text_value_;
            else if(text_purpose_=="password") source_draft_secret_=text_value_;
            else for(auto tab:kNavTabs)if(text_purpose_==std::string("path:")+to_string(tab))source_draft_.platform_paths[tab]=text_value_;
            dialog_model_.selected_option=0;update_source_editor();return true;
        } else if(selected==4) {
            if(settings_text) show_settings();
            else if(text_purpose_=="search") dismiss_dialog();
            else if(text_purpose_=="diag_search") show_mounting_diagnostics();
            else if(text_purpose_=="diag_export_path") show_mounting_diagnostics();
            else if(text_purpose_=="launcher_id") show_launcher_options(launcher_platform_);
            else if(text_purpose_=="launcher_search") show_launcher_picker();
            else {dialog_model_.selected_option=0;update_source_editor();}
            return true;
        }
        update_text_entry();return true;
    }
    if(type==DialogType::SourceEditor) {
        if(selected>=10 && selected<=14){const auto tab=kNavTabs[static_cast<size_t>(selected-9)];
            show_text_entry(source_draft_.platform_paths[tab],std::string("path:")+to_string(tab));return true;}
        switch(selected) {
            case 0:show_text_entry(source_draft_.display_name,"name");break;
            case 1:source_draft_.kind=source_draft_.kind==SourceKind::Local?SourceKind::Nfs:source_draft_.kind==SourceKind::Nfs?SourceKind::Smb:SourceKind::Local;update_source_editor();break;
            case 2:show_text_entry(source_draft_.mount_point,"mount");break;
            case 3:show_text_entry(source_draft_.host,"host");break;
            case 4:show_text_entry(source_draft_.remote_path,"remote");break;
            case 5:show_text_entry(source_draft_.username,"username");break;
            case 6:show_text_entry("","password");break;
            case 7:source_draft_.enabled=!source_draft_.enabled;update_source_editor();break;
            case 8:{
                const auto before=sources_.to_json();
                const auto error=source_edit_id_.empty()?sources_.add_source(source_draft_):sources_.update_source(source_edit_id_,source_draft_);
                if(error) dialog_model_.message=*error;
                else {
                    sources_.set_session_secret(source_edit_id_.empty()?source_draft_.id:source_edit_id_,source_draft_secret_);
                    const std::string saved_id=source_edit_id_.empty()?source_draft_.id:source_edit_id_;
                    // Successful mount: scan (and identify packages) automatically.
                    if(persist_source_change(before) && source_draft_.enabled) {
                        sources_.refresh_all();
                        if(sources_.state_of(saved_id)==SourceState::Mounted) trigger_scan(true);
                    }
                }
                break;
            }
            default:show_sources();break;
        }
        return true;
    }
    if(type==DialogType::SourceRemove) {
        if(selected==0){const auto before=sources_.to_json();sources_.remove_source(source_edit_id_);persist_source_change(before);}
        else show_sources();
        return true;
    }
    if(selected==0) show_source_editor(source_edit_id_);
    else if(selected==1) {
        for(auto c:sources_.list()) if(c.id==source_edit_id_) {
            const auto before=sources_.to_json();c.enabled=!c.enabled;
            if(!sources_.update_source(c.id,c)) persist_source_change(before);
            break;
        }
    } else if(selected==2) {
        dialog_model_={};dialog_model_.type=DialogType::SourceRemove;dialog_model_.title="Remove source?";
        dialog_model_.message=source_edit_id_;dialog_model_.options={"Remove configuration", "Cancel"};
    } else show_sources();
    return true;
}
} // namespace nexodion