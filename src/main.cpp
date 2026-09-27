// main.cpp — FRLG multi-variant entry point (FireRed / LeafGreen).
//
// One source file backs every variant; the build picks the game via
// compile-defs set in CMakeLists.txt (add_gba_variant):
//
//   GBARECOMP_BUILTIN_NAME      e.g. "Pokemon FireRed (USA)"
//   GBARECOMP_BUILTIN_SHA1      expected ROM sha1 (hash gate)
//   GBARECOMP_DEFAULT_GAME_CONFIG  variants/<name>/game.toml
//   GBARECOMP_DEFAULT_DEBUG_PORT / GBARECOMP_WINDOW_TITLE  (read by runtime)
//
// Every gbarecomp game binary takes BOTH a BIOS and a ROM at launch
// (see ../gbarecomp/PRINCIPLES.md "BIOS is sacred"). The CLI accepts:
//
//   <Variant>Recomp [--bios <path>] [--rom <path>] [game.toml]
//
// All three are optional on the command line; missing values are pulled
// from game.toml. Hashes are verified before any code runs.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "runtime.h"
#include "runtime_arm.h"
#include "emerald_ram_dispatch.h"
#include "mobile_platform.h"
#include "emerald_extended_view.h"
#include "touch/emerald_touch.h"
#if defined(GBAGAME_NETPLAY)
#include "multiplayer_launch.h"
#include "gba_netplay_build_identity.h"
#include "emerald_multiplayer.h"
#endif

#ifndef GBARECOMP_BUILTIN_NAME
#define GBARECOMP_BUILTIN_NAME "GBA cartridge"
#endif
#ifndef GBARECOMP_BUILTIN_SHA1
#define GBARECOMP_BUILTIN_SHA1 ""
#endif
#ifndef GBARECOMP_WINDOW_TITLE
#define GBARECOMP_WINDOW_TITLE "gbarecomp"
#endif
#ifndef GBARECOMP_BUILTIN_CRC32
#define GBARECOMP_BUILTIN_CRC32 0
#endif
#ifndef GBARECOMP_BUILTIN_REGION
#define GBARECOMP_BUILTIN_REGION ""
#endif
#ifndef GBARECOMP_BOXART
#define GBARECOMP_BOXART ""
#endif

#if defined(GBAGAME_RECOMP_UI)
#include "game_launcher_boot.h"
#endif

namespace {

void print_usage() {
    std::printf(
        "%s [--bios <path>] [--rom <path>] [game.toml]\n"
        "\n"
        "Both BIOS and ROM are required (either via flags or via the\n"
        "[bios] / [rom] sections of game.toml). The runtime refuses\n"
        "to start unless both hash-verify.\n"
        "\n"
        "Default BIOS path: ../gbarecomp/bios/gba_bios.bin\n"
        "Default game config: " GBARECOMP_DEFAULT_GAME_CONFIG " (relative to CWD)\n",
        GBARECOMP_WINDOW_TITLE);
}

}  // namespace

int emerald_main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--help") == 0 ||
            std::strcmp(argv[i], "-h") == 0) {
            print_usage();
            return 0;
        }
    }
    std::vector<std::string> args(argv, argv + argc);
    // Android: private-storage layout, log file, staged game.toml, no
    // pre-boot launcher. No-op on desktop.
    gbarecomp::MobileProcessOptions mobile;
    mobile.game_config = GBARECOMP_DEFAULT_GAME_CONFIG;
    mobile.program_name = "./EmeraldRecomp";
    const bool on_mobile = gbarecomp::mobile_prepare_process(args, mobile);

    g_runtime_ram_dispatch_hook = &emerald::ram_dispatch;

    // Built-in defaults so a standalone <Variant>Recomp.exe ships without
    // a sibling game.toml. The asset picker still validates against these
    // values; CLI / TOML can override.
    gbarecomp::RunOptions opts;
#if defined(GBAGAME_NETPLAY)
    opts.netplay=gbarecomp::make_gba_netplay_launch("emerald-usa",GBARECOMP_NETPLAY_BUILD_ID,
        "a9dec84dfe7f62ab2220bafaef7479da0929d066ece16a6885f6226db19085af",emerald::setup_link_instance);
    opts.netplay->view_policy = {true, true, emerald::install_netplay_view,
        emerald::update_extended_view, emerald::reset_extended_view};
    try { gbarecomp::parse_gba_netplay_arguments(args,*opts.netplay); }
    catch (const std::exception& e) { std::fprintf(stderr,"netplay: %s\n",e.what()); return 1; }
#endif
    opts.builtin_game_name = GBARECOMP_BUILTIN_NAME;
    opts.builtin_rom_sha1  = (sizeof(GBARECOMP_BUILTIN_SHA1) > 1)
                                 ? GBARECOMP_BUILTIN_SHA1
                                 : nullptr;
    // CRC32 of the pinned ROM (same dump the SHA-1 gates on); the
    // launcher's GAME card uses it for its "ROM verified" check.
    opts.builtin_rom_crc32 = GBARECOMP_BUILTIN_CRC32;
    opts.mod_game_id       = "pokemon-emerald-us";
    opts.mod_owns_adaptive_view = true;
    opts.max_view_width = emerald::kMaxViewWidth;
    opts.max_resize_view_width = emerald::kMaxViewWidth;
    opts.max_resize_view_height = emerald::kMaxViewHeight;
    opts.resize_driven_view = true;
    opts.freely_resizable_window = true;
    opts.extended_view_init = emerald::install_extended_view;
    opts.extended_view_frame = emerald::update_extended_view;
    opts.launcher_expose_widescreen = false;
    opts.launcher_expose_adaptive_view = false;
    opts.launcher_region   = (sizeof(GBARECOMP_BUILTIN_REGION) > 1)
                                 ? GBARECOMP_BUILTIN_REGION
                                 : nullptr;
    opts.launcher_boxart = (sizeof(GBARECOMP_BOXART) > 1)
                               ? GBARECOMP_BOXART
                               : nullptr;
    opts.launcher_game_config = GBARECOMP_DEFAULT_GAME_CONFIG;  // prefill ROM/BIOS
    // Touch-first controls (inert without touches): gestures become verified
    // key presses; host chrome draws trails and the battle panel.
    opts.input_frame = emerald::touch::input_frame;
    opts.touch_gesture_claims = emerald::touch::kGestureClaims;
    opts.host_overlay = emerald::touch::host_overlay;
    opts.tcp_command = emerald::touch::tcp_command;
    opts.diagnostics_snapshot = emerald::touch::diagnostics_snapshot;
    opts.presentation_request = emerald::touch::presentation_request;
    emerald::touch::set_mobile(on_mobile);
    if (on_mobile) {
        // Phones and tablets: follow the sensor with live re-layout, keep a
        // physical pixel size so tablets reveal more world, resume after the
        // OS kills a backgrounded session, and use the touch-first settings.
        opts.orientation = gbarecomp::RunOptions::Orientation::Any;
        opts.resize_view_sizing = gbarecomp::RunOptions::ViewSizing::Density;
        opts.resume_suspend_state_on_launch = true;
        opts.ui_touch_friendly = true;
        opts.touch_pad_default = 0;
    }

    std::vector<char*> av;
#if defined(GBAGAME_RECOMP_UI)
    if (game_launcher_preboot(args, opts)) return 0;   // user quit the launcher
#endif
    av.reserve(args.size());
    for (auto& s : args) av.push_back(s.data());
    return gbarecomp::run_game(static_cast<int>(av.size()), av.data(), opts);
}

#if defined(__ANDROID__)
extern "C" int SDL_main(int argc, char** argv) {
    // Desktop-sized host stack for the recompiled corpus (see mobile_platform.h).
    return gbarecomp::mobile_run_with_stack(emerald_main, argc, argv);
}
#else
int main(int argc, char** argv) {
    return emerald_main(argc, argv);
}
#endif
