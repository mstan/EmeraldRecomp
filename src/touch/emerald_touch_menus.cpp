// emerald_touch_menus.cpp — generic menu providers.
//
// sMenu menus (Start menu, script multichoice, Yes/No, bag/party context
// menus) and ListMenu tasks (bag pockets, shops, scrolling lists) cover most
// of the game's selection UI. A tap on an item navigates the game's own cursor
// there — verified against the cursor the game reports after every press —
// and then presses A, so sounds, redraws and description callbacks all run
// exactly as with a D-pad.

#include "emerald_touch.h"

#include <cmath>
#include <cstdlib>

namespace emerald::touch {

namespace k = gbarecomp;
using guest::addr::gTasks;
using guest::addr::sMenu;
using guest::off::kListScroll;
using guest::off::kListSelected;
using guest::off::kTaskSize;

namespace {

std::uint16_t vertical_toward(int cur, int target, bool wrap, int min, int max) {
    if (cur == target) return 0;
    if (!wrap) return target < cur ? k::kGbaKeyUp : k::kGbaKeyDown;
    const int span = max - min + 1;
    const int down = ((target - cur) % span + span) % span;
    const int up = span - down;
    return up < down ? k::kGbaKeyUp : k::kGbaKeyDown;
}

struct ListDrag {
    bool active = false;
    std::uint8_t task = 0xFF;
    float last_ny = 0;
    float accum = 0;
};
ListDrag g_list_drag;

}  // namespace

bool menus_tap(Core& core, const SceneState& s, float nx, float ny) {
    if (s.menu.live) {
        const MenuState& m = s.menu;
        for (int i = m.min; i <= m.max; ++i) {
            const Rect r = m.item(i);
            if (!r.contains(nx, ny, 1.0f)) continue;
            const bool grid = m.grid;
            const int columns = m.columns;
            const bool wrap = m.wrap;
            const int min = m.min, max = m.max;
            auto read = [](const FrameCtx& ctx) {
                return ctx.scene.menu.live ? static_cast<int>(ctx.mem.s8(sMenu + 2)) : -9999;
            };
            auto choose = [grid, columns, wrap, min, max](int cur, int target) -> std::uint16_t {
                if (grid && columns > 0) {
                    if (cur / columns != target / columns)
                        return target / columns < cur / columns ? k::kGbaKeyUp : k::kGbaKeyDown;
                    return target % columns < cur % columns ? k::kGbaKeyLeft : k::kGbaKeyRight;
                }
                return vertical_toward(cur, target, wrap, min, max);
            };
            core.macro.start("menu:select", {step_navigate(read, i, choose),
                                             step_press(k::kGbaKeyA)},
                             s.signature());
            record_action(core.frame, "tap", scene_name(s.kind), "menu-item", nx, ny, i);
            return true;
        }
        if (m.window_rect.contains(nx, ny)) {
            record_action(core.frame, "tap", scene_name(s.kind), "menu-frame", nx, ny, 0);
            return true;  // inside the menu but between items: nothing to do
        }
    }
    if (s.list.live) {
        const ListState& l = s.list;
        const int visible = std::min(l.max_showed, l.total - l.scroll);
        for (int r = 0; r < visible; ++r) {
            if (!l.row(r).contains(nx, ny)) continue;
            const int target = l.scroll + r;
            const std::uint8_t task = l.task;
            auto read = [task](const FrameCtx& ctx) {
                if (!ctx.scene.list.live || ctx.scene.list.task != task) return -9999;
                const std::uint32_t t = gTasks + task * kTaskSize;
                return static_cast<int>(ctx.mem.u16(t + kListScroll) +
                                        ctx.mem.u16(t + kListSelected));
            };
            auto choose = [](int cur, int tgt) -> std::uint16_t {
                return tgt < cur ? k::kGbaKeyUp : k::kGbaKeyDown;
            };
            core.macro.start("list:select", {step_navigate(read, target, choose, 64),
                                             step_press(k::kGbaKeyA)},
                             s.signature());
            record_action(core.frame, "tap", scene_name(s.kind), "list-row", nx, ny, target);
            return true;
        }
        if (l.window_rect.contains(nx, ny)) {
            record_action(core.frame, "tap", scene_name(s.kind), "list-frame", nx, ny, 0);
            return true;
        }
    }
    return false;
}

// Screen-specific providers for full-screen UIs whose selection is not an
// sMenu/ListMenu (the generic providers above already ran).
bool screens_tap(Core& core, const SceneState& s, float nx, float ny) {
    if (core.macro.busy() || s.menu.live || s.list.live) return false;
    const guest::Mem m = guest::Mem::current();

    if (s.kind == SceneKind::Party) {
        // Slot boxes are windows 0..5 (both layouts); Cancel/Confirm sit in
        // the bottom-right corner (slot ids 7 / 6).
        using guest::addr::gPartyMenu;
        const int count = std::min<int>(6, m.u8(guest::addr::gPlayerPartyCount));
        int target = -1;
        for (int i = 0; i < count && target < 0; ++i)
            if (window_screen_rect(m, i).contains(nx, ny, 1.0f)) target = i;
        if (target < 0 && nx >= 184 && ny >= 128) target = ny >= 144 ? 7 : 6;
        if (target < 0) return false;
        auto read = [](const FrameCtx& ctx) {
            if (ctx.scene.kind != SceneKind::Party) return -9999;
            const int action = ctx.mem.u8(gPartyMenu + 11);
            // While choosing a switch partner the second cursor moves.
            const std::uint32_t slot = (action == 8 || action == 9) ? gPartyMenu + 10
                                                                    : gPartyMenu + 9;
            return static_cast<int>(ctx.mem.s8(slot));
        };
        // Down steps through every slot (…, last mon, Confirm?, Cancel, 0).
        auto choose = [](int, int) -> std::uint16_t { return k::kGbaKeyDown; };
        core.macro.start("party:select", {step_navigate(read, target, choose, 10),
                                          step_press(k::kGbaKeyA)},
                         s.signature());
        record_action(core.frame, "tap", "party", "slot", nx, ny, target);
        return true;
    }

    if (s.kind == SceneKind::Bag && ny < 24 && nx < 112) {
        // The pocket title strip: left/right half flips pockets.
        const std::uint16_t key = nx < 56 ? k::kGbaKeyLeft : k::kGbaKeyRight;
        core.macro.start("bag:pocket", {step_press(key)}, s.signature(), false);
        record_action(core.frame, "tap", "bag", "pocket", nx, ny, nx < 56 ? -1 : 1);
        return true;
    }

    if (s.kind == SceneKind::Summary) {
        // Page edges flip pages; elsewhere is the screen's own A action.
        if (nx < 48 || nx >= 192) {
            core.macro.start("summary:page", {step_press(nx < 48 ? k::kGbaKeyLeft
                                                                 : k::kGbaKeyRight)},
                             s.signature(), false);
            record_action(core.frame, "tap", "summary", "page", nx, ny, nx < 48 ? -1 : 1);
            return true;
        }
    }
    return false;
}

bool menus_drag(Core& core, const SceneState& s, const k::Gesture& g) {
    float nx = 0, ny = 0;
    view_to_native(g.view_x, g.view_y, &nx, &ny);
    if (g.kind == k::GestureKind::DragBegin) {
        float sx = 0, sy = 0;
        view_to_native(g.start_view_x, g.start_view_y, &sx, &sy);
        g_list_drag = {};
        if (s.list.live && s.list.window_rect.contains(sx, sy)) {
            g_list_drag.active = true;
            g_list_drag.task = s.list.task;
            g_list_drag.last_ny = sy;
        } else if (s.menu.live && s.menu.window_rect.contains(sx, sy)) {
            return true;  // a drag inside a static menu does nothing
        } else {
            return false;
        }
    }
    if (!g_list_drag.active) return false;
    if (g.kind == k::GestureKind::DragEnd || g.kind == k::GestureKind::Cancel) {
        g_list_drag.active = false;
        return true;
    }
    if (!s.list.live || s.list.task != g_list_drag.task) {
        g_list_drag.active = false;
        return true;
    }
    // Natural scrolling: moving the finger up reveals later rows (Down).
    g_list_drag.accum += g_list_drag.last_ny - ny;
    g_list_drag.last_ny = ny;
    const float step = static_cast<float>(std::max(8, s.list.row_height));
    while (std::fabs(g_list_drag.accum) >= step) {
        const bool down = g_list_drag.accum > 0;
        core.synth.tap(down ? k::kGbaKeyDown : k::kGbaKeyUp, 1, 1);
        g_list_drag.accum += down ? -step : step;
        record_action(core.frame, "drag", scene_name(s.kind),
                      down ? "list-scroll-down" : "list-scroll-up", nx, ny, 0);
    }
    return true;
}

}  // namespace emerald::touch
