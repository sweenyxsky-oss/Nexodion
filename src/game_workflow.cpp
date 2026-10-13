#include "nexodion/ui_shell.hpp"
#include "nexodion/shadowmount_api.hpp"
#include "nexodion/native_services.hpp"

namespace nexodion {
void UiShell::show_game_actions() {
    const auto entry=focused_entry();if(!entry) return;
    action_title_=entry->title_id;
    dialog_model_={};dialog_model_.type=DialogType::GameActions;
    dialog_model_.title=entry->title;
    dialog_model_.message=action_title_;
    if(!entry->copies.empty()) {
        std::string where="Found on: ";
        for(size_t i=0;i<entry->copies.size();++i) {
            const auto& c=entry->copies[i];
            where+=(i?", ":"")+(c.source_id.empty()?c.source_root:c.source_id)+(c.storage_type.empty()?"":" ("+c.storage_type+")");
        }
        dialog_model_.secondary_text=where;
    }
    if(const auto check=focused_launch_check(); check && !check->can_launch)
        dialog_model_.secondary_text+=(dialog_model_.secondary_text.empty()?"":"\n")+std::string("Launch: ")+check->title;
    if(const auto note=update_note(*entry); !note.empty())
        dialog_model_.secondary_text+=(dialog_model_.secondary_text.empty()?"":"\n")+note;
    dialog_model_.options={"Inspect", "Mount read-only", "Unmount", "Launch", "Uninstall",
        is_favorite(*entry)?"Remove from Favorites":"Add to Favorites", "Details", "Check Before Installing", "Cancel"};
}
bool UiShell::handle_game_workflow(const InputManager& input) {
    const auto type=active_dialog();
    if(type!=DialogType::GameActions && type!=DialogType::ConfirmUninstall) return false;
    if(input.action_triggered(LogicalAction::Cancel)){dismiss_dialog();return true;}
    const int count=static_cast<int>(dialog_model_.options.size());if(count==0) return true;
    if(input.action_triggered(LogicalAction::NavDown) || input.action_triggered(LogicalAction::NavUp)) {
        const int step=input.action_triggered(LogicalAction::NavDown)?1:-1;
        dialog_model_.selected_option=(dialog_model_.selected_option+step+count)%count;return true;
    }
    if(!input.action_triggered(LogicalAction::Confirm)) return true;
    const int selected=dialog_model_.selected_option;
    if(type==DialogType::ConfirmUninstall) {
        if(selected==0) run_game_operation("uninstall");else dismiss_dialog();
        return true;
    }
    if(selected==0){dismiss_dialog();if(auto entry=focused_entry()) open_inspector(*entry);}
    else if(selected==1) run_game_operation("mount");
    else if(selected==2) run_game_operation("unmount");
    else if(selected==3) run_game_operation("launch");
    else if(selected==4){dialog_model_={};dialog_model_.type=DialogType::ConfirmUninstall;
        dialog_model_.title="Uninstall "+action_title_+"?";dialog_model_.message="Remove the console installation. Source files are not deleted.";
        dialog_model_.options={"Uninstall", "Cancel"};dialog_model_.selected_option=1;}
    else if(selected==5){dismiss_dialog();if(auto entry=focused_entry()) toggle_favorite(*entry);}
    else if(selected==6) show_game_details();
    else if(selected==7) show_install_check();
    else dismiss_dialog();
    return true;
}
void UiShell::run_game_operation(const std::string& operation) {
    if(operation_busy_.load()) {show_recovery_guidance("Operation in progress","Wait for the current operation","");return;}
    if(operation=="launch") {
        // Same per-format decision shown in the game menu; nothing is sent unless it can launch.
        const auto check=focused_launch_check();
        if(!check){show_recovery_guidance("No title selected","Select a game first.","");return;}
        if(!check->can_launch){show_recovery_guidance(check->title,check->message,check->steps);return;}
        if(operation_worker_.joinable()) operation_worker_.join();
        operation_busy_=true;
        const auto game=action_title_;const auto target=check->launch_title_id;const auto note=check->message;
        dialog_model_={};dialog_model_.type=DialogType::Notification;dialog_model_.title="Launching";dialog_model_.message=check->title;
        operation_worker_=std::thread([this,game,target,note] {
            OperationResult result{false,"","launch",game};
            try { result.ok=native_launch_title(target,result.message); if(result.ok && target!=game) result.message=note; }
            catch(const std::exception& e){result.message=e.what();}
            std::lock_guard lock(event_mutex_);pending_operation_=std::move(result);
        });
        return;
    }
    if(!valid_native_title(action_title_)) {show_recovery_guidance("Unsupported title","PS1, PS2 and PS3 require a configured native emulator adapter","No PS4/PS5 launch is attempted for legacy titles.");return;}
    if(operation!="launch" && !is_shadowmount_available()){show_shadowmount_lock();return;}
    if(operation=="launch" && !is_title_installed(action_title_)) {
        show_recovery_guidance("Title not confirmed installed","Refresh installed titles or mount the source first", "No launch request was sent.");return;
    }
    if(operation_worker_.joinable()) operation_worker_.join();
    const auto target=daemon_client_->target();const auto title=action_title_;
    operation_busy_=true;
    dialog_model_={};dialog_model_.type=DialogType::Notification;dialog_model_.title="Working";dialog_model_.message=operation+" "+title;
    operation_worker_=std::thread([this,target,title,operation] {
        OperationResult result{false,"",operation,title};
        try {
            if(operation=="launch") result.ok=native_launch_title(title,result.message);
            else {
                ShadowMountApi api(target.first,target.second);
                const auto reply=operation=="mount"?api.mount(title):operation=="unmount"?api.unmount(title):api.uninstall(title);
                result.ok=reply.ok;result.message=reply.ok?"Daemon confirmed "+operation:reply.error;
            }
        } catch(const std::exception& e){result.message=e.what();}
        {std::lock_guard lock(event_mutex_);pending_operation_=std::move(result);}
    });
}
void UiShell::refresh_installed_titles() {
    if(operation_busy_.load()) return;
    if(!is_shadowmount_available()) {show_recovery_guidance("Installed titles unavailable","ShadowMountPlus is required to read the installed-title list","Discovered source files are not treated as installed games.");return;}
    if(operation_worker_.joinable()) operation_worker_.join();
    operation_busy_=true;const auto target=daemon_client_->target();
    operation_worker_=std::thread([this,target] {
        OperationResult result{false,"","refresh",""};std::vector<GameEntry> entries;
        try {
            const auto response=ShadowMountApi(target.first,target.second).games();
            result.ok=response.ok;result.message=response.error;
            if(result.ok) {
                const auto& games=response.data.at("games");
                if(!games.is_array()) throw std::runtime_error("Invalid installed-title list");
                for(const auto& game:games) {
                    if(!game.is_object()) throw std::runtime_error("Invalid installed-title entry");
                    if(!game.value("installed_pkg",false) && !game.value("mounted",false)) continue;
                    GameEntry entry;entry.title_id=game.value("title_id","");
                    if(!valid_native_title(entry.title_id)) continue;
                    entry.title=game.value("name",entry.title_id);entry.installed=true;
                    entry.primary_format=game.value("source_type","");entry.source_root=game.value("path","");
                    entry.confidence=MetadataConfidence::Verified;
                    entry.total_size_bytes=game.value("size_bytes",0ULL);
                    entry.formatted_total_size=game.contains("size_bytes")?format_file_size(entry.total_size_bytes):"Size unavailable";
                    entry.platform=entry.title_id.starts_with("PPSA")?NavTab::PS5:NavTab::PS4;
                    entries.push_back(std::move(entry));
                }
                result.message="Installed-title list refreshed";
            }
        }catch(const std::exception& e){result.ok=false;result.message=e.what();}
        std::lock_guard lock(event_mutex_);
        if(result.ok) pending_installed_=std::move(entries);
        pending_operation_=std::move(result);
    });
}
}