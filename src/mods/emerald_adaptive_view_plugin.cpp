#include "emerald_extended_view.h"
#include "emerald_object_view.h"
#include "emerald_ui_view.h"
#include "gba_bus.h"
#include "gba_ppu.h"
#include "mod_runtime.h"
#include "runtime.h"
#include "runtime_bus_bridge.h"
#include "runtime_arm.h"

#include <cstdio>
#include <cstring>

namespace emerald {
namespace {
constexpr const char* kPackage = "pokemon-emerald.enhancement.widescreen";
FieldView view;
ObjectView objects;
UiView ui;
bool last_objects_ready = true;
bool enabled = false;
ViewStatus last_status = ViewStatus::Native;
unsigned long long last_state_epoch = 0;

int tile_provider(int bg, int x, int y, std::uint16_t* entry) {
    return view.tile(bg, x, y, entry) ? gba::kWsTilemapReplace : gba::kWsTilemapUnavailable;
}
const gba::WsMarginObjPixel* object_provider(int y, int* left, int* width) {
    return objects.row(y, left, width);
}
int ui_provider(int bg, int x, int y, int* sx, int* sy) {
    return ui.sample(bg, x, y, sx, sy);
}

void activate() {
    const char* aspect = gba_mod_option_value(kPackage, "widescreen", "aspect");
    const bool adaptive = !aspect || std::strcmp(aspect, "fit") == 0;
    const int width = aspect && std::strcmp(aspect, "32:9") == 0 ? 569 :
                      aspect && std::strcmp(aspect, "21:9") == 0 ? 373 : 284;
    enabled = true;
    gba_mod_set_adaptive_view_enabled(adaptive);
    gba_mod_set_view_width(width);
}
} // namespace

void reset_extended_view() {
    enabled = false;
    view = FieldView{};
    ui = UiView{};
    objects = ObjectView{};
    last_state_epoch = g_runtime_state_epoch;
    last_objects_ready = true;
    last_status = ViewStatus::Native;
    if (gba::g_ws_tilemap_provider == tile_provider) {
        gba::g_ws_tilemap_provider = nullptr;
        gba::g_ws_authored_margin_layers = 0;
        gba::g_ws_obj_native_clip = 0;
        gba::g_ws_obj_margin_provider = nullptr;
        gba::g_ws_bg_xy_provider = nullptr;
        gba::g_ws_pillarbox = 0;
    }
    gba_mod_set_adaptive_view_enabled(0);
    gba_mod_set_view_width(0);
}

// Touch input sees the anchored presentation; map a point relative to the
// native screen origin back to the native pixel it displays. Returns 1 when
// the point shows a relocated window, 0 for ordinary native/margin content.
int ui_view_source(int x, int y, int* source_x, int* source_y) {
    *source_x = x;
    *source_y = y;
    if (!enabled) return 0;
    int sx = x, sy = y;
    if (ui.sample(0, x, y, &sx, &sy) == 1) {
        *source_x = sx;
        *source_y = sy;
        return 1;
    }
    return 0;
}

bool extended_field_ready() { return enabled && view.status() == ViewStatus::Ready; }

void install_extended_view(std::uint32_t, std::uint32_t) {
    if (!enabled) return;
    gba::g_ws_tilemap_provider = tile_provider;
    gba::g_ws_authored_margin_layers = 1;
    // Keep arbitrary parked OAM native. Verified overworld objects have a
    // separate layer, including sprites suppressed by the guest's X culler.
    gba::g_ws_obj_native_clip = 1;
    gba::g_ws_obj_margin_provider = object_provider;
    gba::g_ws_bg_xy_provider = ui_provider;
    gba::g_ws_bg_xy_provider_layers = 1;
    gba::g_ws_pillarbox = 1;
}

void update_extended_view(const gbarecomp::ExtendedViewFrameInfo* frame) {
    if (!enabled || !frame) return;
    auto* bus = gbarecomp::active_bus();
    if (!bus) return;
    if (last_state_epoch != g_runtime_state_epoch) {
        objects = ObjectView{};
        last_state_epoch = g_runtime_state_epoch;
    }
    const ViewMemory memory{bus->ewram_ptr(), bus->iwram_ptr(), bus->rom_ptr(),
                           bus->rom_size(), bus->vram_ptr(), frame->io,
                           bus->oam_ptr(), bus->pal_ptr()};
    const auto status = view.prepare(memory, frame->view_width, frame->view_height);
    const bool objects_ready = objects.prepare(memory, view, frame->view_width, frame->view_height);
    ui.prepare(memory, view, frame->view_width, frame->view_height);
    if (status == ViewStatus::Ready && objects_ready != last_objects_ready) {
        std::fprintf(stderr, "[emerald:objects] %s frame=%llu objects=%d verified-parts=%d\n",
            objects_ready ? "verified" : "DEGRADED: OAM mismatch, native objects only",
            static_cast<unsigned long long>(frame->frame_count), objects.objects(), objects.verified_parts());
        last_objects_ready = objects_ready;
        if (!objects_ready) {
            const auto& d = objects.mismatch();
            std::fprintf(stderr,"[emerald:objects] object=%u expected=%04X,%04X,%04X hardware=%04X,%04X,%04X\n",
                d[0],d[1],d[2],d[3],d[4],d[5],d[6]);
        }
    }
    gba::g_ws_pillarbox = status == ViewStatus::Ready ? 0 : 1;
    if (status != last_status) {
        std::fprintf(stderr, "[emerald:view] %s frame=%llu width=%u tile-check=%d/%d%s\n",
                     view_status_name(status), static_cast<unsigned long long>(frame->frame_count),
                     frame->view_width, view.matched(), view.compared(),
                     status == ViewStatus::Unverified ? " DEGRADED: native fallback" : "");
        last_status = status;
    }
}
} // namespace emerald

GBA_MOD_CONSTRUCTOR(emerald_register_adaptive_view_plugin) {
    gba_mod_register_reset_callback(emerald::reset_extended_view);
    gba_mod_register_activation_plugin("pokemon-emerald.widescreen", emerald::activate);
    // Keep 0.1.0 installations working while gating new packages on this build.
    gba_mod_register_activation_plugin("pokemon-emerald.widescreen.v2", emerald::activate);
}
