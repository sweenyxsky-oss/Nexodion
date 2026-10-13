# Nexodion — Architecture & Milestone Audits

Audit date: 2026-10-13. Scope: entire `nexodion` tree (≈7,000 lines C++20).

## 1. Forks and build instructions
- Initial foundation source: GitHub `sweenyxsky-oss/ps5-library-browser` (upstream; local native app now `nexodion/`). Further accessible reference forks are documented in the Phase 2 audit below.
- Host build: `cmake -B build && cmake --build build && ctest --test-dir build` (`.github/workflows/ci.yml`).
- PS5 build: PS5 Payload SDK v0.41.1, `prospero.cmake` toolchain, clang/lld 18, target `nexodion_elf` (`.github/workflows/ps5.yml`).

## 2. Runtime and UI framework (verified)
- Pure native C++20, Threads, vendored `nlohmann/json`. **No WebKit, webview, HTML or JS anywhere.**
- PS5 links: libkernel, SceSystemService, SceNotification, SceAppInstUtil, SceNet, SceUserService; Phase 2 adds SDK-bundled SDL2 and native Pad/VideoOut dependencies.
- Phase 1 renderer/input gap is addressed by the Phase 2 native SDL implementation; actual console validation remains outstanding.

## 3. Module map
| Module | Role |
|---|---|
| daemon_client | ShadowMountPlus probe on 127.0.0.1:10101, feature lock |
| scanner / game_metadata / index_cache | Library crawl, param parsing, JSON cache |
| inspector / installer | Game/DLC/Update inspector, install queue, retry, history, persistence |
| startup_diagnostics | Mount/daemon/network/service health checks |
| views / input / ui_shell | Layout engine, pad state, top-level UI state |
| **theme** (new) | Nexodion color tokens |
| **source_manager** (new) | Local / NFS / SMB source abstraction |
| **navigation** (new) | 6-tab top nav bar |

## 4. New in Milestone 1
- `theme.hpp`: Obsidian #090B12, Void Slate #171B2B, Nexus Violet #8B5CF6, Plasma Lilac #B9A0FF, Titanium #D8DCE8, Steel Shadow #343B52, Frost White #E9EAF2, plus surface/text/status roles. Renderer-agnostic RGBA8.
- `SourceManager`: `ILibrarySource` interface, config validation (absolute mount path, no `..`, host+share for network), duplicate-id guard, states Unmounted/Mounted/Unreachable/Locked, network sources locked until ShadowMountPlus is online (dependency lock), JSON persistence (passwords never stored), atomic load (bad file leaves state intact), thread-safe.
- `NavigationBar`: Installed, PS5, PS4, PS3, PS2, PS1; L1/R1 wrap-around; title-ID prefix hints; non-Installed tabs flagged as daemon-dependent.
- `tests/foundation_test.cpp` covering all of the above.

## 5. ELF and release package (confirmed)
`nexodion_<git-describe>.zip` containing `nexodion.elf`, `eboot.bin` (same ELF), `sce_sys/param.json`, `sce_sys/icon0.png`, README, LICENSE; validated by `tools/validate_release_package.py`. Title ID PPSA77777 retained for installation identity; title "Nexodion", content ID `UP9000-PPSA77777_00-NEXODION00000000`.

## 6. Verification results
- All 11 core sources compile with `-Wall -Wextra -Wpedantic` (GCC 14.3), new files also with `-Werror`.
- 8/8 test executables pass (7 original + foundation_test). `--headless` run exits 0.
- These are historical Phase 1 results; Phase 2 PS5 cross-compilation now passes (see §9).

## 7. Findings / risks
1. Native display and pad input are now implemented; console performance and compatibility still require hardware testing.
2. SourceManager and NavigationBar are wired into the shell; source CRUD and per-platform Mounting configuration are implemented.
3. Direct SMB/NFS client access is not yet integrated. Existing network sources probe paths mounted externally; ShadowMountPlus's documented game-image mount API does not mount network shares.
4. `param.json` contentId/titleId are placeholders; display branding is "Nexodion"; numeric IDs retained, not newly assigned.
5. PS3/PS2/PS1 tabs need an emulator/launcher backend definition before they can launch anything.

## 8. Native rename audit (2026-10-13)
- Local source tree `nexodion/`, includes/namespace `nexodion`, CMake targets `nexodion_core` / `nexodion_elf`, executable `nexodion.elf`, archive `nexodion_<version>.zip`.
- Startup/help/recovery messages, cache/job default filenames, manifest title and content suffix, documentation and validator use Nexodion. Existing cache/job files are not migrated automatically; use `--cache` / `--jobs` to reuse a previous file.
- Real upstream URLs and third-party credits are preserved. Numeric title ID `PPSA77777` is unchanged to avoid inventing a console identity.
- Existing `sce_sys/icon0.png` is unchanged: it contains old lettering. No image editing or visual generation was performed, per the no-visual-generation requirement.
- Verification: Debug CMake build with GCC 14.3 completed without warnings; 8/8 CTest programs passed. `nexodion.elf --help` shows the renamed executable and paths; headless launch exits 0. A host-only ZIP with renamed artifacts passed the release validator and explicit manifest identity assertions. Leftover text matches are limited to upstream URLs/provenance. A host ELF is not evidence of PS5 compatibility; PS5 cross-compilation remains unverified here.

## 9. Phase 1 re-audit and Phase 2 implementation audit (2026-10-13)

### References and dependency selection
- Consulted user forks: `ps4mount`, `PS5-Network-Mounting`, `ps5-homebrew-ui`, `ps5-react`, `game-store` (Overdrive), `fuse-nfs`, `libnfs`, `PS5SX2`, `SwanStationPS5`, and `ShadowMountPlus`.
- Native UI reference `ps5-homebrew-ui` at `333e4ee07498c552b30770acf6063003fed6833e` uses EGL/OpenGL and scePad; `ps5-react` is an engine shell, while Overdrive is web-oriented and is not embedded in Nexodion.
- Selected the official SDK's SDL2 native software framebuffer/controller backend rather than importing web technology or hand-writing undocumented VideoOut structures. Versions and SDK SHA-256 are in `dependencies.lock.json`.

### Implemented
- `native_runtime.hpp`: renderer/input interface; `native_runtime_sdl.cpp`: native display, PNG artwork from existing library files, controller attach/detach, DualSense mapping and desktop validation input; stub retained for headless host builds.
- `native_painter.cpp`: dark token-based six-tab navigation, grid/carousel, selection, inspector/dialogs and settings/source status. No generated images or concepts.
- Options opens settings; touchpad switches layout; L1/R1 switch tabs. Input repeat is 300 ms initially and 100 ms thereafter; synthesized stick directions have correct transition detection.
- Source JSON loads through `--sources`; active mount roots feed scanning. Explicit platform metadata distinguishes ambiguous PS1/PS2 serials. Source status, refresh, rescan, diagnostics and history are available; there is no add/edit/remove-source form.
- Scanner and installer workers enqueue events; only `UiShell::tick()` changes UI-visible data. Destruction cancels and joins workers. Source loading validates object/version/type atomically and uses a virtual daemon-state setter.

### Deep audit and verification
- Phase 1 clean Debug rebuild: warning-free; original 8/8 tests pass.
- Phase 2 host SDL Debug build: 10/10 tests pass, including multi-source scans, tab filtering, input repeat, malformed JSON, paint calls, and virtual controller → PS5 tab → settings → disconnect → quit.
- Independent read-only audit verified worker teardown and UI-thread handoff. Fixed startup diagnostics being replaced when no source is reachable; added a regression test. Updated SDK checksum and native dependency checks in the release workflow. Fixed grid card vertical positioning and a PS5-only unused helper warning.
- Real PS5 SDK v0.41.1 cross-build with clang/lld 18 completes without warnings. Sandbox LLVM resource headers required a local `-resource-dir` override; this is environment-specific and is not a project dependency path.
- ELF inspection confirms ELF64 x86-64 PIE, Sony SPRX dependencies, and `scePadReadState`, `sceVideoOutOpen`, `sceVideoOutSubmitFlip` bindings. This is not a Linux-only substitute.
- ZIP staged in `/tmp/nexodion_phase2_verified.zip` using the PS5 ELF passes package validation. Headless startup/scan exits successfully. Validation checks structure and architecture, not installation or hardware execution.

### Remaining limitations / external verification
- No PS5 hardware is available here: installation, firmware compatibility, actual DualSense input, display framing and sustained software-rendering performance remain unverified. Phase 2's on-device acceptance is blocked on console testing.
- NFS/SMB mount/auth remains delegated; emulator launching for PS1/PS2/PS3 remains undefined. Multi-root cache uses one cache file tagged with the configured scan path, not independent per-source caches.
- Built-in SDL2_gfx text is limited to Latin-1/ASCII fallback; non-ASCII titles and controller glyphs are not fully supported. Cache flushes at bounded limits may cause frame stalls; hardware profiling and a proper font path are follow-up work.
- Startup diagnostics still use the existing probe configuration; test/mocked service availability is not proof of real installed-title enumeration or install-service operation.
- Workflows remain inside the native tree's `.github/workflows` and assume that tree as repository root; the enclosing Lovable repository does not automatically execute nested workflows.
- Bundled icon still has upstream lettering; replacement requires a user-provided asset. Numeric installation IDs are retained, not assigned or certified.

## 10. Milestone 3 implementation and audit
- Added controller-driven source creation, editing, enable/disable, confirmed removal, search entry, format selection and title/size sorting. Configuration writes use private temporary files, fsync and atomic rename; failed writes roll back the in-memory configuration.
- Entries retain source identity; equal title IDs in different roots do not merge. Cache schema 2 records every scanned root, including empty roots, and invalid cache loads preserve existing output.
- Independent audit confirmed persistence rollback, multi-root separation and worker teardown. Resolved filesystem probes holding the source configuration mutex by using shared lifetime snapshots and atomic availability state.
- Verification: warning-free host SDL Debug build; 11/11 CTest programs passed, including controller-driven add/disable/remove, disk readback, write-failure rollback, search, duplicate title IDs and empty multi-root cache coverage.
- Remaining constraint: filesystem availability probes are still synchronous at UI call sites; a stale mounted filesystem can stall the calling thread even though unrelated configuration readers no longer share its lock. Network mounting/authentication itself remains delegated.
- Independent integrity pre-audit found the default installation delegate simulated success and the Installed tab represented all discovered files. These are milestone 4 correctness targets, not verified installed-title behavior.
- Console testing is deferred by request and is not an exit blocker; no hardware compatibility claim is made.

## 11. Milestone 4 implementation and audit
- Replaced the production simulated installer with the PS5 React native PKG relay; host builds explicitly refuse installation. Payload requests restrict paths and title IDs, bound socket responses and distinguish interruption from cancellation. No exploit or privilege acquisition is supplied: an existing authorized loader is required.
- Implemented documented ShadowMountPlus POST mount/unmount/uninstall operations with destructive-action confirmation and UI-thread result application. Installed now contains only confirmed installed PKGs/mounted titles from the daemon, never every scanned file; refresh is available in Settings.
- Audited thread lifetime and corrected the inspector navigation regression, per-task cancellation isolation, relay response validation and non-EOF upload failures. Independent review findings about operation flags were checked against tick(), which resets the flag before reuse; malformed headers intentionally fail closed.
- Verification: warning-free native headless Debug build; 11/11 CTest programs pass. New tests exercise exact POST mount requests, daemon error status, HTTP errors, truncated bodies, malformed schema, strict native input checks, host refusal, cancel isolation, and Installed filtering.
- Native SDK/helper linking and release packaging remain milestone 5 targets. Hardware testing is deferred, not claimed. Legacy emulator launch adapters are not configured; NFS/SMB authentication and filesystem mounting remain owned by the external daemon/OS.

## 12. Milestone 5 and final integrity review
- Integrated native ps5-homebrew-ui tween code and PS5 React helper adaptations with commit locks, licenses and notices; no web renderer is embedded.
- Added bounded binary SFO parsing, proportional native text and helper ELF release validation. Independent final review found no blocking defects in parser bounds, worker result handoffs, relay serialization, installer locking or package staging.
- The Mounting extension has been rebuilt with GCC 14.3 and native SDL; twelve host tests pass. The locked SDK was restored and the latest PS5 app and both helpers cross-compiled with LLVM 18; a trailing-newline warning was corrected and the rebuild was warning-free. The current release ZIP passes every package/ELF/license check. No console execution is claimed.
- Historical counts and limitations in sections 6–11 describe their respective stage, not current functionality. Source editing, multi-root cache headers and native integration supersede the older omissions.

## 13. Mounting settings and network-library investigation
- Settings → Mounting lists configured devices; selecting a device displays its type, path, availability and separate PS5/PS4/PS3/PS2/PS1 paths. Internal/NFS/SMB choices and controller field editing persist without passwords. Proposed `/data/nexodion/libraries/PS*` defaults are editable; folders are not fabricated or silently created.
- Version-1 source JSON remains readable. Explicit platform assignments and source/storage identities survive scan and cache serialization. Missing and overlapping roots are rejected with recovery guidance rather than silent success; legacy configurations retain their metadata classification.
- Live reference research: `sweenyxsky-oss/ps5-ezremote-client` uses libsmb2 (`smb2_connect_share`, `smb2_opendir`, `smb2_pread`) and libnfs (`nfs_mount`, `nfs_opendir`, `nfs_pread`) for normal native network file access. These libraries provide listing/reading, not a kernel-visible filesystem mount. They are a viable future scanner transport, but are not linked or implemented in Nexodion yet.
- `sweenyxsky-oss/PS5-Network-Mounting` documents unverified patched FUSE dependencies; upstream `sahlberg/fuse-nfs` is Linux FUSE glue. No verified SDK FUSE backend was found. Do not claim direct SMB/NFS mounting works because an existing directory is reachable.
- Remaining limitations: source probes remain synchronous; confinement is lexical, not a sandbox against symlink or filesystem changes; unavailable legacy/emulator launch adapters and non-ASCII font coverage remain unresolved. Whole-code review does not establish device compatibility.

## 14. SDK restoration and mounting-share repository verification
- Confirmed user SDK source fork: https://github.com/sweenyxsky-oss/sdk . Restored compiled SDK v0.41.1 from the existing locked upstream archive and verified its SHA-256. Sandbox-only LLVM tool/resource-header and unwrapped pkg-config paths were required; these are environment setup, not project dependencies.
- Verified https://github.com/sweenyxsky-oss/PS5-Network-Mounting : genuine libnfs/libsmb2 FUSE adapters and `GET /api/v1/status`, `POST /api/v1/mount`, `POST /api/v1/unmount` exist. This corrects the earlier broad implication that no referenced repository exposes a network-mount API: ShadowMountPlus does not, but this separate service does.
- Options: NFS or SMB, host/port, remote export/share path, SMB username/password; one read-only share at `/hostapp`. Platform-specific folders could live beneath that share, but this implementation does not provide simultaneous independent mounts or arbitrary mountpoints.
- Independently compiled its no-FUSE API scaffold to a PS5 ELF using the restored SDK. Its JSON tests and host HTTP smoke tests passed; these validate routing/state checks, not a real mounted share. The full build stops at missing ORBISDEV/FUSE dependencies. Its current workflow describes building libnfs/libsmb2 plus Itemzflow FUSE compatibility sources, but that full path was not built or verified here.
- Native `main.cpp` defaults to `0.0.0.0:18080` despite a contradictory loopback statement in BUILD.md; use loopback for a future app integration. `X-Requested-With` is a CSRF header, not authentication. Its standalone control page invokes WebKit; Nexodion must not embed or launch that page. Any integration would use its native service API only.
- Package validation after SDK restoration: `/tmp/nexodion-sdk/nexodion-sdk-verified.zip` contains the latest native app, matching eboot, helpers, metadata and attribution; every validator check passed. The mounting service remains separate, not integrated or certified functional.
