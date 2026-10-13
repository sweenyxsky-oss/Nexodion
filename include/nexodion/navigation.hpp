#pragma once
// Top navigation bar: Installed, PS5, PS4, PS3, PS2, PS1 (L1/R1 cycles, wraps).
#include <array>
#include <cstddef>
#include <string>

namespace nexodion {

enum class NavTab { Installed = 0, PS5, PS4, PS3, PS2, PS1 };
inline constexpr std::size_t kNavTabCount = 6;
inline constexpr std::array<NavTab, kNavTabCount> kNavTabs{
    NavTab::Installed, NavTab::PS5, NavTab::PS4, NavTab::PS3, NavTab::PS2, NavTab::PS1};

const char* to_string(NavTab tab);
// Title-ID prefixes for platform filtering (empty for Installed).
const char* title_prefix_hint(NavTab tab);
// Whether the platform tab needs ShadowMountPlus / emulation support (locked otherwise).
bool tab_requires_daemon(NavTab tab);

class NavigationBar {
public:
    NavTab active() const { return active_; }
    std::size_t active_index() const { return static_cast<std::size_t>(active_); }
    void set_active(NavTab tab) { active_ = tab; }
    void next();
    void prev();
private:
    NavTab active_{NavTab::Installed};
};

} // namespace nexodion
