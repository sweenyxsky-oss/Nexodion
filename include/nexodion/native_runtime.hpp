#pragma once
#include "input.hpp"
#include "theme.hpp"
#include <memory>
#include <string>
#include <string_view>

namespace nexodion {
class UiShell;
struct Rect { float x, y, w, h; };
class NativeCanvas {
public:
    virtual ~NativeCanvas() = default;
    virtual void fill(Rect rect, theme::Rgba color) = 0;
    virtual void text(Rect rect, std::string_view text, theme::Rgba color, int size = 24) = 0;
    virtual bool image(Rect rect, const std::string& path) = 0;
};
// Shared native painter; testable without a GPU or console.
void paint_native_shell(NativeCanvas& canvas, const UiShell& shell, bool controller_connected);

class NativeRuntime : public NativeCanvas {
public:
    virtual bool poll(PadState& pad) = 0; // false on close/quit; disconnected pad is neutral.
    virtual bool controller_connected() const = 0;
    virtual void present() = 0;
};
std::unique_ptr<NativeRuntime> create_native_runtime(std::string& error);
} // namespace nexodion