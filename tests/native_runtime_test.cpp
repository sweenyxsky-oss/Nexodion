#define SDL_MAIN_HANDLED
#include "nexodion/native_runtime.hpp"
#include "nexodion/ui_shell.hpp"
#include <SDL.h>
#include <cstdlib>
#include <iostream>
#define CHECK(x) do{if(!(x)){std::cerr<<"Failed: " #x << ':' << __LINE__<<' '<<SDL_GetError()<<'\n';std::exit(1);}}while(0)
using namespace nexodion;
int main() {
    std::string error;
    auto runtime=create_native_runtime(error);CHECK(runtime);
    const int index=SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,6,SDL_CONTROLLER_BUTTON_MAX,0);
    CHECK(index>=0);
    auto* joystick=SDL_JoystickOpen(index);CHECK(joystick);
    PadState pad;CHECK(runtime->poll(pad));CHECK(runtime->controller_connected());
    CHECK(SDL_JoystickSetVirtualButton(joystick,SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,1)==0);
    CHECK(runtime->poll(pad));CHECK((pad.buttons&pad_bits::kR1)!=0);
    UiShell shell(nullptr,"/tmp/nexodion_runtime_test_missing.json");
    InputManager input;input.process_pad_input(pad);shell.handle_pad_input(input);
    CHECK(shell.navigation().active()==NavTab::PS5);
    paint_native_shell(*runtime,shell,true);runtime->present();
    CHECK(SDL_JoystickSetVirtualButton(joystick,SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,0)==0);
    CHECK(SDL_JoystickSetVirtualButton(joystick,SDL_CONTROLLER_BUTTON_START,1)==0);
    CHECK(runtime->poll(pad));input.process_pad_input(pad);shell.handle_pad_input(input);
    CHECK(shell.active_dialog()==DialogType::Settings);
    paint_native_shell(*runtime,shell,true);runtime->present();
    SDL_JoystickClose(joystick);CHECK(SDL_JoystickDetachVirtual(index)==0);
    CHECK(runtime->poll(pad));CHECK(!runtime->controller_connected());CHECK(pad.buttons==0);
    SDL_Event event{};event.type=SDL_QUIT;SDL_PushEvent(&event);CHECK(!runtime->poll(pad));
    std::cout<<"Native SDL: display, controller -> PS5 tab -> settings, disconnect and quit passed\n";
}