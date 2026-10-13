#include "nexodion/navigation.hpp"

namespace nexodion {

const char* to_string(NavTab t) {
    switch (t) {
        case NavTab::Installed: return "Installed";
        case NavTab::PS5: return "PS5";
        case NavTab::PS4: return "PS4";
        case NavTab::PS3: return "PS3";
        case NavTab::PS2: return "PS2";
        case NavTab::PS1: return "PS1";
    }
    return "Installed";
}

const char* title_prefix_hint(NavTab t) {
    switch (t) {
        case NavTab::PS5: return "PPSA";
        case NavTab::PS4: return "CUSA";
        case NavTab::PS3: return "BLUS|BLES|NPUB|NPEB";
        case NavTab::PS2: return "SLUS|SLES|SCUS|SCES";
        case NavTab::PS1: return "SLUS|SLES|SCUS|SCES";
        default: return "";
    }
}

bool tab_requires_daemon(NavTab t) { return t != NavTab::Installed; }

void NavigationBar::next() {
    active_ = static_cast<NavTab>((active_index() + 1) % kNavTabCount);
}
void NavigationBar::prev() {
    active_ = static_cast<NavTab>((active_index() + kNavTabCount - 1) % kNavTabCount);
}

} // namespace nexodion
