#include <cstdint>
#define SDL_MAIN_HANDLED
#include "nexodion/native_runtime.hpp"
#include <SDL.h>
#include <SDL_image.h>
#include <SDL2_gfxPrimitives.h>
#include <algorithm>
#include <cmath>
#include <map>

namespace nexodion {
namespace {
struct Texture { SDL_Texture* value{}; int w{},h{}; uint64_t used{}; };
class SdlRuntime final : public NativeRuntime {
    SDL_Window* window_{};
    SDL_Renderer* renderer_{};
    SDL_GameController* controller_{};
    std::map<std::string,Texture> images_;
    std::map<std::string,Texture> labels_;
    bool initialized_{};
    static SDL_Rect rect(Rect r) { return {static_cast<int>(r.x),static_cast<int>(r.y),static_cast<int>(r.w),static_cast<int>(r.h)}; }
    static void clear(std::map<std::string,Texture>& cache) {
        for(auto& [key,t]:cache) { (void)key; if(t.value) SDL_DestroyTexture(t.value); }
        cache.clear();
    }
public:
    ~SdlRuntime() override {
        clear(images_); clear(labels_);
        if(controller_) SDL_GameControllerClose(controller_);
        if(renderer_) SDL_DestroyRenderer(renderer_);
        if(window_) SDL_DestroyWindow(window_);
        if(initialized_) { IMG_Quit(); SDL_Quit(); }
    }
    bool initialize(std::string& error) {
        SDL_SetMainReady();
        if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMECONTROLLER|SDL_INIT_EVENTS)!=0) {error=SDL_GetError();return false;}
        initialized_=true;
        if((IMG_Init(IMG_INIT_PNG)&IMG_INIT_PNG)==0) {error=IMG_GetError();return false;}
        window_=SDL_CreateWindow("Nexodion",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,1920,1080,SDL_WINDOW_SHOWN);
        if(!window_) {error=SDL_GetError();return false;}
        // The PS5 SDL backend exposes a native VideoOut framebuffer to this software renderer.
        renderer_=SDL_CreateRenderer(window_,-1,SDL_RENDERER_SOFTWARE);
        if(!renderer_) {error=SDL_GetError();return false;}
        SDL_RenderSetLogicalSize(renderer_,1920,1080);
        SDL_SetRenderDrawBlendMode(renderer_,SDL_BLENDMODE_BLEND);
        return true;
    }
    bool controller_connected() const override {return controller_ && SDL_GameControllerGetAttached(controller_);}
    bool poll(PadState& pad) override {
        pad={};
        SDL_Event event;
        while(SDL_PollEvent(&event)) if(event.type==SDL_QUIT) return false;
        if(controller_ && !SDL_GameControllerGetAttached(controller_)) {SDL_GameControllerClose(controller_);controller_=nullptr;}
        if(!controller_) for(int i=0;i<SDL_NumJoysticks();++i) if(SDL_IsGameController(i)) {
            controller_=SDL_GameControllerOpen(i); if(controller_) break;
        }
        SDL_GameControllerUpdate();
        if(controller_connected()) {
            const std::pair<SDL_GameControllerButton,uint32_t> buttons[] = {
                {SDL_CONTROLLER_BUTTON_A,pad_bits::kCross},{SDL_CONTROLLER_BUTTON_B,pad_bits::kCircle},
                {SDL_CONTROLLER_BUTTON_X,pad_bits::kSquare},{SDL_CONTROLLER_BUTTON_Y,pad_bits::kTriangle},
                {SDL_CONTROLLER_BUTTON_LEFTSHOULDER,pad_bits::kL1},{SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,pad_bits::kR1},
                {SDL_CONTROLLER_BUTTON_LEFTSTICK,pad_bits::kL3},{SDL_CONTROLLER_BUTTON_RIGHTSTICK,pad_bits::kR3},
                {SDL_CONTROLLER_BUTTON_START,pad_bits::kOptions},{SDL_CONTROLLER_BUTTON_TOUCHPAD,pad_bits::kTouchpad},
                {SDL_CONTROLLER_BUTTON_DPAD_UP,pad_bits::kUp},{SDL_CONTROLLER_BUTTON_DPAD_DOWN,pad_bits::kDown},
                {SDL_CONTROLLER_BUTTON_DPAD_LEFT,pad_bits::kLeft},{SDL_CONTROLLER_BUTTON_DPAD_RIGHT,pad_bits::kRight}};
            for(auto [button,bit]:buttons) if(SDL_GameControllerGetButton(controller_,button)) pad.buttons|=bit;
            const auto axis=[this](SDL_GameControllerAxis a) {return std::clamp(SDL_GameControllerGetAxis(controller_,a)/32767.0f,-1.0f,1.0f);};
            pad.stick_left_x=axis(SDL_CONTROLLER_AXIS_LEFTX);pad.stick_left_y=axis(SDL_CONTROLLER_AXIS_LEFTY);
            pad.stick_right_x=axis(SDL_CONTROLLER_AXIS_RIGHTX);pad.stick_right_y=axis(SDL_CONTROLLER_AXIS_RIGHTY);
            if(axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT)>.45f) pad.buttons|=pad_bits::kL2;
            if(axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT)>.45f) pad.buttons|=pad_bits::kR2;
        }
#if !defined(__PS5__) && !defined(__PROSPERO__)
        // Native desktop validation uses keyboard through the same pad/input/shell flow.
        const auto* keys=SDL_GetKeyboardState(nullptr);
        const std::pair<SDL_Scancode,uint32_t> keys_map[]={
            {SDL_SCANCODE_LEFT,pad_bits::kLeft},{SDL_SCANCODE_RIGHT,pad_bits::kRight},
            {SDL_SCANCODE_UP,pad_bits::kUp},{SDL_SCANCODE_DOWN,pad_bits::kDown},
            {SDL_SCANCODE_RETURN,pad_bits::kCross},{SDL_SCANCODE_ESCAPE,pad_bits::kCircle},
            {SDL_SCANCODE_Q,pad_bits::kL1},{SDL_SCANCODE_E,pad_bits::kR1},
            {SDL_SCANCODE_TAB,pad_bits::kTouchpad},{SDL_SCANCODE_F1,pad_bits::kOptions}};
        for(auto [key,bit]:keys_map) if(keys[key]) pad.buttons|=bit;
#endif
        return true;
    }
    void fill(Rect r,theme::Rgba color) override {
        SDL_SetRenderDrawColor(renderer_,color.r,color.g,color.b,color.a);
        auto bounds=rect(r);SDL_RenderFillRect(renderer_,&bounds);
    }
    void text(Rect r,std::string_view value,theme::Rgba color,int size) override {
        const int glyph=std::max(8,size);
        if(r.w<glyph || r.h<glyph || value.empty()) return;
        const int columns=std::max(1,static_cast<int>(r.w)/glyph);
        const int rows=std::max(1,static_cast<int>(r.h)/glyph);
        std::string output; int column=0,row=0;
        for(unsigned char ch:value) {
            if(ch=='\n' || column>=columns) {output+='\n';column=0;if(++row>=rows) break;}
            if(ch=='\n') continue;
            // SDL2_gfx built-in font is Latin-1, avoid corrupting multi-byte sequences.
            if(ch>=128) {if((ch&0xc0)==0x80) continue;ch='?';}
            if(ch<32) ch=' ';
            output+=static_cast<char>(ch);++column;
        }
        const auto key=output+std::to_string(color.packed())+":"+std::to_string(columns)+":"+std::to_string(rows);
        if(labels_.size()>=128) clear(labels_);
        auto it=labels_.find(key);
        if(it==labels_.end()) {
            const int w=columns*8,h=rows*8;
            auto* surface=SDL_CreateRGBSurfaceWithFormat(0,w,h,32,SDL_PIXELFORMAT_RGBA32);
            if(!surface) return;
            SDL_FillRect(surface,nullptr,0);
            auto* software=SDL_CreateSoftwareRenderer(surface);
            if(!software) {SDL_FreeSurface(surface);return;}
            size_t start=0; int y=0;
            while(start<output.size()) {
                auto end=output.find('\n',start);
                const auto part=output.substr(start,end==std::string::npos?end:end-start);
                stringRGBA(software,0,static_cast<Sint16>(y),part.c_str(),color.r,color.g,color.b,color.a);
                y+=8; if(end==std::string::npos) break;start=end+1;
            }
            SDL_RenderPresent(software);SDL_DestroyRenderer(software);
            auto* texture=SDL_CreateTextureFromSurface(renderer_,surface);SDL_FreeSurface(surface);
            if(!texture) return;
            SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_BLEND);
            it=labels_.emplace(key,Texture{texture,w,h}).first;
        }
        // Preserve square bitmap glyph proportions instead of stretching them 1:2.
        auto destination=rect({r.x,r.y,static_cast<float>(columns*glyph),static_cast<float>(rows*glyph)});
        SDL_RenderCopy(renderer_,it->second.value,nullptr,&destination);
    }
    bool image(Rect r,const std::string& path) override {
        if(path.empty()) return false;
        auto it=images_.find(path);
        if(it==images_.end()) {
            // Only on-screen covers are requested; cap new decodes per frame so scrolling stays smooth.
            if(decodes_this_frame_>=4) return false;
            ++decodes_this_frame_;
            if(images_.size()>=96) { // evict the least recently drawn cover
                auto oldest=images_.begin();
                for(auto i=images_.begin();i!=images_.end();++i) if(i->second.used<oldest->second.used) oldest=i;
                if(oldest->second.value) SDL_DestroyTexture(oldest->second.value);
                images_.erase(oldest);
            }
            int w=0,h=0;
            auto* surface=IMG_Load(path.c_str());
            SDL_Texture* texture=nullptr;
            if(surface) {
                w=surface->w;h=surface->h;
                if(w<=4096 && h<=4096) texture=SDL_CreateTextureFromSurface(renderer_,surface);
                SDL_FreeSurface(surface);
            }
            it=images_.emplace(path,Texture{texture,w,h}).first;
        }
        it->second.used=++frame_counter_;
        if(!it->second.value || it->second.w<=0 || it->second.h<=0) return false;
        const float scale=std::min(r.w/it->second.w,r.h/it->second.h);
        const float w=it->second.w*scale,h=it->second.h*scale;
        auto destination=rect({r.x+(r.w-w)/2,r.y+(r.h-h)/2,w,h});
        SDL_RenderCopy(renderer_,it->second.value,nullptr,&destination);return true;
    }
    void present() override {SDL_RenderPresent(renderer_);decodes_this_frame_=0;}
    int decodes_this_frame_{0};
    uint64_t frame_counter_{0};
};
}
std::unique_ptr<NativeRuntime> create_native_runtime(std::string& error) {
    auto runtime=std::make_unique<SdlRuntime>();
    if(!runtime->initialize(error)) return nullptr;
    return runtime;
}
} // namespace nexodion