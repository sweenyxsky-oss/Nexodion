#pragma once
// Nexodion color system (native, renderer-agnostic RGBA8 tokens).
#include <cstdint>

namespace nexodion::theme {

struct Rgba {
    uint8_t r, g, b, a;
    constexpr uint32_t packed() const {
        return (uint32_t(r) << 24) | (uint32_t(g) << 16) | (uint32_t(b) << 8) | a;
    }
};

constexpr Rgba hex(uint32_t rgb, uint8_t a = 0xFF) {
    return {uint8_t(rgb >> 16), uint8_t(rgb >> 8), uint8_t(rgb), a};
}

// Brand palette
inline constexpr Rgba kObsidian     = hex(0x090B12); // primary background
inline constexpr Rgba kVoidSlate    = hex(0x171B2B);
inline constexpr Rgba kNexusViolet  = hex(0x8B5CF6);
inline constexpr Rgba kPlasmaLilac  = hex(0xB9A0FF);
inline constexpr Rgba kTitanium     = hex(0xD8DCE8);
inline constexpr Rgba kSteelShadow  = hex(0x343B52);
inline constexpr Rgba kFrostWhite   = hex(0xE9EAF2);

// Semantic UI roles
inline constexpr Rgba kBackground       = kObsidian;
inline constexpr Rgba kBackgroundAlt    = hex(0x10131D);
inline constexpr Rgba kCard             = kVoidSlate;
inline constexpr Rgba kCardHover        = hex(0x22283A);
inline constexpr Rgba kSelectedBorder   = kNexusViolet;
inline constexpr Rgba kTextPrimary      = hex(0xF5F6FC);
inline constexpr Rgba kTextSecondary    = hex(0xA7AEC2);
inline constexpr Rgba kTextMuted        = hex(0x737D94);
inline constexpr Rgba kDivider          = hex(0x2A3041);
inline constexpr Rgba kModalOverlay     = hex(0x05060B, 0xCC);

// Status roles
inline constexpr Rgba kSuccess    = hex(0x35D399);
inline constexpr Rgba kInfo       = hex(0x35D9FF);
inline constexpr Rgba kInProgress = hex(0xA78BFA);
inline constexpr Rgba kWarning    = hex(0xFBBF24);
inline constexpr Rgba kError      = hex(0xFF5D6C);
inline constexpr Rgba kPaused     = hex(0x94A3B8);
inline constexpr Rgba kDisabled   = hex(0x626B7D);

} // namespace nexodion::theme
