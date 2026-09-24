// emerald_touch_battle.cpp — battle menus by touch, plus a host-drawn panel of
// large buttons in the space around the (native 3:2) battle image.
//
// Both paths drive the game's own battle controller: the tapped choice is
// reached by D-pad presses verified against gActionSelectionCursor /
// gMoveSelectionCursor / gMultiUsePlayerCursor, then confirmed with A.

#include "emerald_touch.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "host_overlay.h"
#include "host_window.h"

namespace emerald::touch {

namespace k = gbarecomp;
using namespace guest;

namespace {

constexpr int kWinActionMenu = 2, kWinMoveName1 = 3, kWinYesNo = 12;

Rect window_rect_of(const Mem& m, int window) {
    // Same derivation as the classifier (live BG scroll, map-size wrap).
    const std::uint32_t w = addr::gWindows + window * off::kWindowSize;
    const std::uint8_t bg = m.u8(w);
    if (bg > 3 || !m.u8(w + 3) || !m.u8(w + 4)) return {};
    const std::uint16_t cnt = m.io16(0x08 + 2 * bg);
    const int map_w = (cnt & 0x4000) ? 512 : 256, map_h = (cnt & 0x8000) ? 512 : 256;
    auto place = [](int pos, int size) {
        pos = ((pos % size) + size) % size;
        return pos >= size - 16 ? pos - size : pos;
    };
    return {place(m.u8(w + 1) * 8 - (m.io16(0x10 + 4 * bg) & 0x1FF), map_w),
            place(m.u8(w + 2) * 8 - (m.io16(0x12 + 4 * bg) & 0x1FF), map_h),
            m.u8(w + 3) * 8, m.u8(w + 4) * 8};
}

bool on_screen(const Rect& r) {
    return !r.empty() && r.x < 240 && r.y < 160 && r.x + r.w > 0 && r.y + r.h > 0;
}

// 2x2 cursors (actions, moves): bit0 = column, bit1 = row.
std::uint16_t quad_toward(int cur, int target) {
    if ((cur & 1) != (target & 1)) return (target & 1) ? k::kGbaKeyRight : k::kGbaKeyLeft;
    if ((cur & 2) != (target & 2)) return (target & 2) ? k::kGbaKeyDown : k::kGbaKeyUp;
    return 0;
}

void start_quad(Core& core, const SceneState& s, const char* label, std::uint32_t cursor_addr,
                int target) {
    const std::uint8_t battler = s.battle.battler;
    const BattleControl control = s.battle.control;
    auto read = [cursor_addr, battler, control](const FrameCtx& ctx) {
        if (ctx.scene.battle.control != control) return -9999;
        return static_cast<int>(ctx.mem.u8(cursor_addr + battler));
    };
    core.macro.start(label, {step_navigate(read, target, quad_toward, 8),
                             step_press(k::kGbaKeyA)},
                     s.signature());
}

void start_yes_no(Core& core, const SceneState& s, int target) {
    auto read = [](const FrameCtx& ctx) {
        if (ctx.scene.battle.control != BattleControl::YesNo) return -9999;
        return static_cast<int>(ctx.mem.u8(addr::gMultiUsePlayerCursor));
    };
    auto choose = [](int cur, int tgt) -> std::uint16_t {
        return tgt < cur ? k::kGbaKeyUp : k::kGbaKeyDown;
    };
    core.macro.start("battle:yes-no", {step_navigate(read, target, choose, 4),
                                       step_press(k::kGbaKeyA)},
                     s.signature());
}

void start_target(Core& core, const SceneState& s, int battler) {
    auto read = [](const FrameCtx& ctx) {
        if (ctx.scene.battle.control != BattleControl::Target) return -9999;
        return static_cast<int>(ctx.mem.u8(addr::gMultiUsePlayerCursor));
    };
    auto choose = [](int, int) -> std::uint16_t { return k::kGbaKeyRight; };
    core.macro.start("battle:target", {step_navigate(read, battler, choose, 8),
                                       step_press(k::kGbaKeyA)},
                     s.signature());
}

Rect battler_sprite_rect(const Mem& m, int battler) {
    const std::uint8_t sprite = m.u8(0x020241E4 + battler);  // gBattlerSpriteIds
    if (sprite >= 64) return {};
    const std::uint32_t sp = addr::gSprites + sprite * 0x44;
    const int x = m.s16(sp + 0x20) + m.s16(sp + 0x24);
    const int y = m.s16(sp + 0x22) + m.s16(sp + 0x26);
    return {x - 28, y - 28, 56, 56};
}

// ── Host panel ───────────────────────────────────────────────────────────

struct PanelButton {
    float x = 0, y = 0, w = 0, h = 0;   // drawable pixels
    std::string label, detail;
    int value = 0;       // action/move index, yes/no, battler id, -1 = back
    bool enabled = true;
    bool selected = false;
};

struct PanelGeometry {
    int drawable_w = 0, drawable_h = 0;
    k::PresentationLayout game{};
    float mm = 6.0f;
    int inset_l = 0, inset_t = 0, inset_r = 0, inset_b = 0;
};

const char* const kActionLabels[4] = {"FIGHT", "BAG", "POK\xC3\x89MON", "RUN"};
const char* const kSafariLabels[4] = {"BALL", "POK\xC3\x89" "BLOCK", "GO NEAR", "RUN"};

std::string move_name(const Mem& m, int move) {
    if (move <= 0 || move > 354) return {};
    return decode_text(m.ptr(addr::gMoveNames + move * 13, 13), 13);
}
std::string type_name(const Mem& m, int type) {
    if (type < 0 || type > 17) return {};
    return decode_text(m.ptr(addr::gTypeNames + type * 7, 7), 7);
}

std::vector<PanelButton> panel_buttons(const Mem& m, const SceneState& s,
                                       const PanelGeometry& g) {
    std::vector<PanelButton> out;
    const BattleControl c = s.battle.control;
    if (c == BattleControl::None || c == BattleControl::Other ||
        c == BattleControl::MoveSwitch)
        return out;

    // Free space: below the image in portrait, the side bars in landscape.
    const float margin = 2.5f * g.mm;
    const bool portrait = g.drawable_h > g.drawable_w;
    std::vector<PanelButton> items;
    if (c == BattleControl::Action || c == BattleControl::SafariAction) {
        for (int i = 0; i < 4; ++i) {
            PanelButton b;
            b.label = (c == BattleControl::SafariAction ? kSafariLabels : kActionLabels)[i];
            b.value = i;
            b.selected = s.battle.action_cursor == i;
            items.push_back(b);
        }
    } else if (c == BattleControl::Move) {
        const std::uint32_t mon = addr::gBattleMons + s.battle.battler * 0x58;
        for (int i = 0; i < 4; ++i) {
            PanelButton b;
            const int move = m.u16(mon + 0x0C + 2 * i);
            b.value = i;
            b.enabled = move != 0;
            b.selected = s.battle.move_cursor == i;
            if (move) {
                b.label = move_name(m, move);
                const std::uint32_t mv = addr::gBattleMoves + move * 12;
                const int base_pp = m.u8(mv + 4);
                const int bonus = (m.u8(mon + 0x3B) >> (2 * i)) & 3;
                const int max_pp = base_pp + base_pp * 20 * bonus / 100;
                char detail[48];
                std::snprintf(detail, sizeof(detail), "%s  PP %d/%d",
                              type_name(m, m.u8(mv + 2)).c_str(), m.u8(mon + 0x24 + i), max_pp);
                b.detail = detail;
            } else {
                b.label = "-";
            }
            items.push_back(b);
        }
    } else if (c == BattleControl::YesNo) {
        PanelButton yes, no;
        yes.label = "YES";
        yes.value = 0;
        yes.selected = s.battle.target_cursor == 0;
        no.label = "NO";
        no.value = 1;
        no.selected = s.battle.target_cursor == 1;
        items = {yes, no};
    } else if (c == BattleControl::Target) {
        const int count = std::min<int>(4, m.u8(addr::gBattlersCount));
        for (int b = 0; b < count; ++b) {
            if (m.u8(0x02024210) & (1u << b)) continue;   // gAbsentBattlerFlags
            PanelButton t;
            const std::uint32_t mon = addr::gBattleMons + b * 0x58;
            t.label = decode_text(m.ptr(mon + 0x30, 11), 11);
            t.detail = (m.u8(addr::gBattlerPositions + b) & 1) ? "foe" : "ally";
            t.value = b;
            t.selected = s.battle.target_cursor == b;
            items.push_back(t);
        }
    }
    if (c == BattleControl::Move || c == BattleControl::Target) {
        PanelButton back;
        back.label = "BACK";
        back.value = -1;
        items.push_back(back);
    }

    float rx, ry, rw, rh;
    int columns;
    if (portrait) {
        rx = static_cast<float>(g.inset_l) + margin;
        ry = static_cast<float>(g.game.y + g.game.height) + margin;
        rw = static_cast<float>(g.drawable_w - g.inset_l - g.inset_r) - 2 * margin;
        rh = static_cast<float>(g.drawable_h - g.inset_b) - margin - ry;
        columns = 2;
    } else {
        const float left = static_cast<float>(g.game.x - g.inset_l);
        const float right = static_cast<float>(g.drawable_w - g.inset_r - (g.game.x + g.game.width));
        constexpr float kMinBarMm = 12.0f;
        if (std::max(left, right) < kMinBarMm * g.mm) return out;   // no room: tap the image
        // Two stacked columns: left bar and right bar.
        const float bar = std::min(left, right) >= kMinBarMm * g.mm ? std::min(left, right)
                                                                   : std::max(left, right);
        rx = left >= right ? static_cast<float>(g.inset_l) + margin
                           : static_cast<float>(g.game.x + g.game.width) + margin;
        ry = static_cast<float>(g.inset_t) + margin;
        rw = bar - 2 * margin;
        rh = static_cast<float>(g.drawable_h - g.inset_t - g.inset_b) - 2 * margin;
        columns = 1;
        if (std::min(left, right) >= kMinBarMm * g.mm) {
            // Use both bars: items alternate left/right like the 2x2 menu.
            const float lx = static_cast<float>(g.inset_l) + margin;
            const float rxx = static_cast<float>(g.game.x + g.game.width) + margin;
            const int per_side = static_cast<int>((items.size() + 1) / 2);
            const float h = std::min(rh / std::max(1, per_side) - margin, 18.0f * g.mm);
            for (std::size_t i = 0; i < items.size(); ++i) {
                PanelButton b = items[i];
                const bool on_right = (i % 2) == 1;
                const int row = static_cast<int>(i / 2);
                b.x = on_right ? rxx : lx;
                b.y = ry + row * (h + margin);
                b.w = bar - 2 * margin;
                b.h = h;
                out.push_back(b);
            }
            return out;
        }
    }
    if (rh < 14.0f * g.mm || rw < 9.0f * g.mm) return out;
    const int rows = static_cast<int>((items.size() + columns - 1) / columns);
    const float h = std::min((rh - margin * (rows - 1)) / std::max(1, rows), 20.0f * g.mm);
    const float w = (rw - margin * (columns - 1)) / columns;
    for (std::size_t i = 0; i < items.size(); ++i) {
        PanelButton b = items[i];
        b.x = rx + (i % columns) * (w + margin);
        b.y = ry + (i / columns) * (h + margin);
        b.w = w;
        b.h = h;
        out.push_back(b);
    }
    return out;
}

void activate(Core& core, const SceneState& s, int value) {
    const BattleControl c = s.battle.control;
    if (value < 0) {
        core.macro.start("battle:back", {step_press(k::kGbaKeyB)}, s.signature(), false);
        return;
    }
    if (c == BattleControl::Action || c == BattleControl::SafariAction)
        start_quad(core, s, "battle:action", addr::gActionSelectionCursor, value);
    else if (c == BattleControl::Move)
        start_quad(core, s, "battle:move", addr::gMoveSelectionCursor, value);
    else if (c == BattleControl::YesNo)
        start_yes_no(core, s, value);
    else if (c == BattleControl::Target)
        start_target(core, s, value);
}

}  // namespace

bool battle_tap(Core& core, const SceneState& s, float nx, float ny) {
    if (!s.battle.active || core.macro.busy()) return false;
    const Mem m = Mem::current();
    const BattleControl c = s.battle.control;
    if (c == BattleControl::Action || c == BattleControl::SafariAction) {
        const Rect w = window_rect_of(m, kWinActionMenu);
        if (!on_screen(w) || !w.contains(nx, ny)) return false;
        const int col = nx >= w.x + w.w / 2 ? 1 : 0;
        const int row = ny >= w.y + w.h / 2 ? 1 : 0;
        activate(core, s, col | (row << 1));
        record_action(core.frame, "tap", "battle", "action", nx, ny, col | (row << 1));
        return true;
    }
    if (c == BattleControl::Move) {
        for (int i = 0; i < 4; ++i) {
            const Rect w = window_rect_of(m, kWinMoveName1 + i);
            if (!on_screen(w) || !w.contains(nx, ny, 2.0f)) continue;
            if (i >= s.battle.moves) return true;
            activate(core, s, i);
            record_action(core.frame, "tap", "battle", "move", nx, ny, i);
            return true;
        }
        return false;
    }
    if (c == BattleControl::YesNo) {
        const Rect w = window_rect_of(m, kWinYesNo);
        if (!on_screen(w) || !w.contains(nx, ny)) return false;
        const int choice = ny >= w.y + w.h / 2 ? 1 : 0;
        activate(core, s, choice);
        record_action(core.frame, "tap", "battle", "yes-no", nx, ny, choice);
        return true;
    }
    if (c == BattleControl::Target) {
        const int count = std::min<int>(4, m.u8(addr::gBattlersCount));
        for (int b = 0; b < count; ++b) {
            if (!battler_sprite_rect(m, b).contains(nx, ny)) continue;
            activate(core, s, b);
            record_action(core.frame, "tap", "battle", "target", nx, ny, b);
            return true;
        }
    }
    return false;
}

bool battle_panel_tap(Core& core, const SceneState& s, float dx, float dy) {
    if (!core.info || core.macro.busy()) return false;
    PanelGeometry g;
    g.drawable_w = core.info->drawable_width;
    g.drawable_h = core.info->drawable_height;
    g.game = core.info->game_rect;
    g.mm = core.info->drawable_px_per_mm;
    g.inset_l = core.info->safe_insets.left;
    g.inset_t = core.info->safe_insets.top;
    g.inset_r = core.info->safe_insets.right;
    g.inset_b = core.info->safe_insets.bottom;
    const Mem m = Mem::current();
    for (const PanelButton& b : panel_buttons(m, s, g)) {
        if (dx < b.x || dy < b.y || dx >= b.x + b.w || dy >= b.y + b.h) continue;
        if (!b.enabled) return true;
        activate(core, s, b.value);
        record_action(core.frame, "tap", "battle", "panel", dx, dy, b.value);
        k::host_haptic_pulse(12, 0.45f);
        return true;
    }
    return false;
}

void battle_overlay(k::HostOverlay& ov, const SceneState& s) {
    PanelGeometry g;
    g.drawable_w = ov.drawable_width();
    g.drawable_h = ov.drawable_height();
    g.game = ov.game_rect();
    g.mm = ov.drawable_px_per_mm();
    const auto in = ov.safe_insets();
    g.inset_l = in.left;
    g.inset_t = in.top;
    g.inset_r = in.right;
    g.inset_b = in.bottom;
    const Mem m = Mem::current();
    const auto D = k::OverlaySpace::Drawable;
    for (const PanelButton& b : panel_buttons(m, s, g)) {
        const k::OverlayColor fill = !b.enabled ? k::OverlayColor{40, 44, 56, 150}
            : b.value < 0 ? k::OverlayColor{36, 58, 92, 220}
            : b.selected ? k::OverlayColor{255, 214, 74, 235}
                         : k::OverlayColor{28, 34, 52, 225};
        const k::OverlayColor text = b.selected ? k::OverlayColor{20, 20, 28, 255}
                                                : k::OverlayColor{236, 242, 252, 255};
        ov.fill_rect(b.x, b.y, b.w, b.h, 2.5f * g.mm, fill, D);
        ov.stroke_rect(b.x, b.y, b.w, b.h, 2.5f * g.mm, 1.5f, {130, 150, 190, 180}, D);
        if (!ov.text_supported()) continue;
        // Shrink a line until it fits 88% of the button width.
        auto fit = [&](const std::string& s, float size) {
            const float w = ov.text_width(s.c_str(), size, D);
            return w > b.w * 0.88f && w > 0.0f ? size * (b.w * 0.88f) / w : size;
        };
        const float size = fit(b.label, std::min(b.h * 0.34f, 5.5f * g.mm));
        const float cy = b.detail.empty() ? b.y + (b.h - size) * 0.5f : b.y + b.h * 0.18f;
        ov.text(b.x + b.w * 0.5f, cy, size, text, b.label.c_str(), k::OverlayAlign::Center, D);
        if (!b.detail.empty()) {
            const float dsize = fit(b.detail, std::min(b.h * 0.34f, 5.5f * g.mm) * 0.62f);
            ov.text(b.x + b.w * 0.5f, b.y + b.h * 0.58f, dsize, text, b.detail.c_str(),
                    k::OverlayAlign::Center, D);
        }
    }
}

}  // namespace emerald::touch
