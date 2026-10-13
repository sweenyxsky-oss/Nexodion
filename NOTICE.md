# Native third-party notices

PS5 React — created by half144 and PS5 React contributors.
https://github.com/overdriveps5/ps5-react

Native payload helpers and relay logic derive from the user fork
`sweenyxsky-oss/ps5-react`, commit `c4ff3e1cb80bb3331b8c8ce1a5e8f07e1e44a0c5`.
GPL-3.0-or-later and the attribution-preservation term are retained under
`external/ps5-react/`. Nexodion adapts validation, cancellation reporting and
title launch; it embeds no JavaScript, web renderer or WebKit from the fork.

PS5 Homebrew UI — Copyright (C) 2026 BlackBearReloaded.
https://github.com/sweenyxsky-oss/ps5-homebrew-ui

Native easing utilities come from commit
`333e4ee07498c552b30770acf6063003fed6833e`; GPL-3.0-or-later notices and license
are retained under `external/ps5-homebrew-ui/`.

The SDK's SDL2, SDL2_image and SDL2_gfx libraries retain their upstream
licenses. Existing Nexodion upstream credits and license remain unchanged.
## PKG Viewer
Package identification logic is ported from PKG Viewer by Loopayeh (MIT), https://github.com/sweenyxsky-oss/pkg-viewer, commit 91b06fcc.

## Legacy emulator launch targets (not bundled)
- SwanStationPS5 (PS1) — https://github.com/sweenyxsky-oss/SwanStationPS5 (fork of darkxex/SwanStationPS5, GPL-3.0), commit 838906e6, app PPSA98510.
- PS5SX2 (PS2) — https://github.com/sweenyxsky-oss/PS5SX2 (fork of Swordpdf/PS5SX2, GPL-3.0), commit b4cf55e8, app PPSA99203.
- RPS5S3 (PS3) — placeholder; repository link pending.
Nexodion only opens these installed apps by title ID; no emulator code is included.
