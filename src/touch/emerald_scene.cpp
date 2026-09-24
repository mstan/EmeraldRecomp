// emerald_scene.cpp — see emerald_scene.h.

#include "emerald_scene.h"

#include <algorithm>

#include "gba_bus.h"
#include "mod_function_hooks.h"
#include "runtime_bus_bridge.h"

namespace emerald::guest {

std::string decode_text(const std::uint8_t* src, std::size_t max_len) {
    std::string out;
    if (!src) return out;
    for (std::size_t i = 0; i < max_len; ++i) {
        const std::uint8_t c = src[i];
        if (c == 0xFF) break;
        if (c >= 0xBB && c <= 0xD4) out += static_cast<char>('A' + (c - 0xBB));
        else if (c >= 0xD5 && c <= 0xEE) out += static_cast<char>('a' + (c - 0xD5));
        else if (c >= 0xA1 && c <= 0xAA) out += static_cast<char>('0' + (c - 0xA1));
        else switch (c) {
            case 0x00: out += ' '; break;
            case 0xAB: out += '!'; break;
            case 0xAC: out += '?'; break;
            case 0xAD: out += '.'; break;
            case 0xAE: out += '-'; break;
            case 0xB0: out += "\xE2\x80\xA6"; break;   // …
            case 0xB1: case 0xB2: out += '"'; break;
            case 0xB3: case 0xB4: out += '\''; break;
            case 0xB5: out += "\xE2\x99\x82"; break;   // ♂
            case 0xB6: out += "\xE2\x99\x80"; break;   // ♀
            case 0xB8: out += ','; break;
            case 0xBA: out += '/'; break;
            case 0x1B: out += "\xC3\xA9"; break;        // é
            case 0x5C: out += '('; break;
            case 0x5D: out += ')'; break;
            case 0xF0: out += ':'; break;
            default: break;
        }
    }
    return out;
}

Mem Mem::current() {
    Mem m;
    auto* bus = gbarecomp::active_bus();
    if (!bus) return m;
    m.ewram = bus->ewram_ptr();
    m.iwram = bus->iwram_ptr();
    m.rom = bus->rom_ptr();
    m.rom_size = bus->rom_size();
    m.vram = bus->vram_ptr();
    m.io = bus->io().raw();
    m.oam = bus->oam_ptr();
    m.pal = bus->pal_ptr();
    return m;
}

}  // namespace emerald::guest

namespace emerald::touch {

using namespace guest;

const char* scene_name(SceneKind k) {
    switch (k) {
        case SceneKind::Unknown: return "unknown";
        case SceneKind::Boot: return "boot";
        case SceneKind::TitleMenus: return "title_menus";
        case SceneKind::Overworld: return "overworld";
        case SceneKind::Battle: return "battle";
        case SceneKind::Bag: return "bag";
        case SceneKind::Party: return "party";
        case SceneKind::Summary: return "summary";
        case SceneKind::Storage: return "storage";
        case SceneKind::Pokedex: return "pokedex";
        case SceneKind::Naming: return "naming";
        case SceneKind::Shop: return "shop";
        case SceneKind::Options: return "options";
        case SceneKind::TrainerCard: return "trainer_card";
        case SceneKind::Pokenav: return "pokenav";
        case SceneKind::EasyChat: return "easy_chat";
        case SceneKind::FlyMap: return "fly_map";
        case SceneKind::Contest: return "contest";
        case SceneKind::Blender: return "blender";
        case SceneKind::Slots: return "slots";
        case SceneKind::Roulette: return "roulette";
        case SceneKind::FactorySelect: return "factory_select";
        case SceneKind::PyramidBag: return "pyramid_bag";
        case SceneKind::FrontierPass: return "frontier_pass";
        case SceneKind::Mail: return "mail";
        case SceneKind::Other: return "other";
    }
    return "unknown";
}

const char* battle_control_name(BattleControl c) {
    switch (c) {
        case BattleControl::None: return "none";
        case BattleControl::Action: return "action";
        case BattleControl::Move: return "move";
        case BattleControl::Target: return "target";
        case BattleControl::YesNo: return "yes_no";
        case BattleControl::MoveSwitch: return "move_switch";
        case BattleControl::SafariAction: return "safari_action";
        case BattleControl::Other: return "other";
    }
    return "none";
}

Rect MenuState::item(int index) const {
    if (grid && columns > 0) {
        return {window_rect.x + left + (index % columns) * option_w,
                window_rect.y + top + (index / columns) * option_h,
                option_w, option_h};
    }
    return {window_rect.x + left, window_rect.y + top + index * option_h,
            window_rect.w - left, option_h};
}

Rect ListState::row(int visible_row) const {
    return {window_rect.x, window_rect.y + up_text_y + visible_row * row_height,
            window_rect.w, row_height};
}

std::uint32_t SceneState::signature() const {
    std::uint32_t s = cb2 * 2654435761u;
    s ^= static_cast<std::uint32_t>(kind) << 24;
    if (menu.live) s ^= 0x100u | menu.window;
    if (list.live) s ^= 0x10000u | (list.task << 8);
    if (battle.active) s ^= static_cast<std::uint32_t>(battle.control) << 20;
    if (start_menu) s ^= 0x800000u;
    return s;
}

// ── Probes ──────────────────────────────────────────────────────────────
namespace {
enum MenuProbe : std::uint32_t {
    kProbeWrap = 1u << 0, kProbeNoWrap = 1u << 1, kProbeGrid = 1u << 2,
};
std::uint32_t g_menu_probe = 0;
bool g_list_probe = false;
std::uint8_t g_list_task = 0xFF;
bool g_field_probe = false;

struct ProbeSpec { const char* id; std::uint32_t addr; };
const ProbeSpec kProbes[] = {
    {"emerald.touch.probe.menu_input", fn::Menu_ProcessInput},
    {"emerald.touch.probe.menu_input_nowrap", fn::Menu_ProcessInputNoWrap},
    {"emerald.touch.probe.menu_input_other", fn::ProcessMenuInput_other},
    {"emerald.touch.probe.menu_input_nowrap_other", fn::Menu_ProcessInputNoWrapAround_other},
    {"emerald.touch.probe.menu_input_clear", fn::Menu_ProcessInputNoWrapClearOnChoose},
    {"emerald.touch.probe.menu_grid", fn::Menu_ProcessGridInput},
    {"emerald.touch.probe.list_input", fn::ListMenu_ProcessInput},
    {"emerald.touch.probe.field_input", fn::DoCB1_Overworld},
};

int probe_callback(std::uint32_t addr, int /*thumb*/, ArmCpuState* cpu) {
    if (addr == fn::ListMenu_ProcessInput) {
        probe_list_entry(cpu ? static_cast<std::uint8_t>(cpu->R[0]) : 0xFF);
    } else if (addr == fn::DoCB1_Overworld) {
        probe_field_input();
    } else {
        probe_menu_entry(addr);
    }
    return 0;  // observe only: the guest body always runs
}
}  // namespace

void probe_menu_entry(std::uint32_t pc) {
    if (pc == fn::Menu_ProcessGridInput) g_menu_probe |= kProbeGrid;
    else if (pc == fn::Menu_ProcessInput || pc == fn::ProcessMenuInput_other)
        g_menu_probe |= kProbeWrap;
    else g_menu_probe |= kProbeNoWrap;
}

void probe_list_entry(std::uint8_t task_id) {
    g_list_probe = true;
    g_list_task = task_id;
}

void probe_field_input() { g_field_probe = true; }

void register_scene_probes() {
    for (const auto& p : kProbes)
        gba_mod_register_function_entry_plugin(p.id, p.addr, 1, probe_callback);
}

void enable_scene_probes() {
    for (const auto& p : kProbes) gba_mod_set_function_hook_enabled(p.id, 1);
}

// ── Classification ───────────────────────────────────────────────────────
namespace {

SceneKind kind_for_cb2(std::uint32_t cb2) {
    switch (cb2) {
        case fn::CB2_Overworld: return SceneKind::Overworld;
        case fn::BattleMainCB2:
        case fn::CB2_InitBattle:
        case fn::CB2_HandleStartBattle: return SceneKind::Battle;
        case fn::CB2_BagMenuRun: return SceneKind::Bag;
        case fn::CB2_UpdatePartyMenu: return SceneKind::Party;
        case fn::SummaryMainCB2: return SceneKind::Summary;
        case fn::CB2_PokeStorage: return SceneKind::Storage;
        case fn::CB2_Pokedex: return SceneKind::Pokedex;
        case fn::CB2_NamingScreen: return SceneKind::Naming;
        case fn::CB2_BuyMenu: return SceneKind::Shop;
        case fn::OptionMenuMainCB2: return SceneKind::Options;
        case fn::CB2_TrainerCard: return SceneKind::TrainerCard;
        case fn::CB2_Pokenav: return SceneKind::Pokenav;
        case fn::CB2_EasyChatScreen: return SceneKind::EasyChat;
        case fn::CB2_FlyMap: return SceneKind::FlyMap;
        case fn::CB2_ContestMain: return SceneKind::Contest;
        case fn::CB2_PlayBlender: return SceneKind::Blender;
        case fn::CB2_SlotMachine: return SceneKind::Slots;
        case fn::CB2_Roulette: return SceneKind::Roulette;
        case fn::CB2_FactorySelect: return SceneKind::FactorySelect;
        case fn::CB2_PyramidBag: return SceneKind::PyramidBag;
        case fn::CB2_FrontierPass: return SceneKind::FrontierPass;
        case fn::CB2_MailRead: return SceneKind::Mail;
        case fn::CB2_MainMenu:
        case fn::CB2_InitTitleScreen:
        case fn::TitleScreenMainCB2: return SceneKind::TitleMenus;
        case 0: return SceneKind::Boot;
        default: return SceneKind::Other;
    }
}

}  // namespace

// Screen rectangle of a window from its template and live BG scroll.
Rect window_screen_rect(const Mem& m, int window) {
    if (window < 0 || window >= 32) return {};
    const std::uint32_t w = addr::gWindows + window * off::kWindowSize;
    const std::uint8_t bg = m.u8(w + 0);
    if (bg > 3 || !m.u8(w + 3) || !m.u8(w + 4)) return {};
    // Text BG maps are 256 or 512 pixels per axis (BGxCNT screen size); the
    // window's screen position is its map position minus scroll, modulo that.
    const std::uint16_t cnt = m.io16(0x08 + 2 * bg);
    const int map_w = (cnt & 0x4000) ? 512 : 256;
    const int map_h = (cnt & 0x8000) ? 512 : 256;
    const int hofs = m.io16(0x10 + 4 * bg) & 0x1FF;
    const int vofs = m.io16(0x12 + 4 * bg) & 0x1FF;
    auto place = [](int pos, int size) {
        pos = ((pos % size) + size) % size;
        return pos >= size - 16 ? pos - size : pos;   // just above/left of the screen
    };
    return {place(m.u8(w + 1) * 8 - hofs, map_w), place(m.u8(w + 2) * 8 - vofs, map_h),
            m.u8(w + 3) * 8, m.u8(w + 4) * 8};
}

namespace {
Rect window_rect(const Mem& m, std::uint8_t window) { return window_screen_rect(m, window); }

// A visible window drawn with a dialogue frame, other than `exclude`. Visible
// = at least one interior tilemap entry holds the window's own tile (hidden
// but allocated windows share the BG); dialogue frame = the column two tiles
// left of the window repeats one vertical-edge entry down every interior row
// (menu.c WindowFunc_DrawDialogueFrame and its custom-tile variants), which a
// window interior never does. Standard (menu) frames are one tile wide.
bool dialogue_window_visible(const Mem& m, int exclude) {
    if (!m.vram) return false;
    for (int i = 0; i < 32; ++i) {
        if (i == exclude) continue;
        const std::uint32_t w = addr::gWindows + i * off::kWindowSize;
        const int bg = m.u8(w + 0), left = m.u8(w + 1), top = m.u8(w + 2);
        const int width = m.u8(w + 3), height = m.u8(w + 4), pal = m.u8(w + 5);
        const unsigned base = m.u16(w + 6);
        if (bg > 3 || !width || !height || !m.u32(w + 8) || left < 2 ||
            left + width > 32 || top + height > 32)
            continue;
        const std::uint32_t screen =
            0x06000000u + ((m.io16(0x08 + 2 * bg) >> 8) & 31) * 0x800u;
        auto entry = [&](int tx, int ty) {
            return m.u16(screen + static_cast<std::uint32_t>((ty * 32 + tx) * 2));
        };
        bool visible = false;
        for (int ty = 0; ty < height && !visible; ++ty)
            for (int tx = 0; tx < width && !visible; ++tx) {
                const unsigned e = entry(left + tx, top + ty);
                visible = (e & 1023) == base + ty * width + tx && int(e >> 12) == pal;
            }
        if (!visible) continue;
        const std::uint16_t edge = entry(left - 2, top);
        bool framed = (edge & 1023) != 0;
        for (int ty = 1; ty < height && framed; ++ty) framed = entry(left - 2, top + ty) == edge;
        if (framed) return true;
    }
    return false;
}
}  // namespace

SceneState classify(const Mem& m, std::uint64_t frame) {
    SceneState s;
    s.frame = frame;
    if (!m.ok()) return s;
    s.cb1 = m.func(addr::gMain + 0);
    s.cb2 = m.func(addr::gMain + 4);
    s.kind = kind_for_cb2(s.cb2);
    for (int i = 0; i < 16; ++i) {
        const std::uint32_t t = addr::gTasks + i * off::kTaskSize;
        if (m.u8(t + off::kTaskActive)) {
            s.task_mask |= 1u << i;
            s.task_funcs[i] = m.func(t + off::kTaskFunc);
        }
    }

    // Field.
    s.field.overworld = s.kind == SceneKind::Overworld;
    s.field.input_ran = g_field_probe;
    s.field.controls_locked = m.u8(addr::sLockFieldControls) != 0;
    s.field.fading = (m.u8(addr::gPaletteFade + 7) & 0x80) != 0;
    s.field.script_running = m.u8(addr::sGlobalScriptContextStatus) != 2;
    s.field.free = s.field.overworld && s.field.input_ran &&
                   !s.field.controls_locked && !s.field.fading;

    // Text printers.
    for (int i = 0; i < 32; ++i) {
        const std::uint32_t p = addr::sTextPrinters + i * off::kTextPrinterSize;
        if (!m.u8(p + off::kTextPrinterActive)) continue;
        const std::uint8_t state = m.u8(p + off::kTextPrinterState);
        if (state == 1 || state == 2) {        // RENDER_STATE_WAIT / CLEAR
            s.text.waiting = true;
            s.text.window = m.u8(p + off::kTextPrinterWindow);
        } else {
            s.text.printing = true;
            if (s.text.window == 0xFF) s.text.window = m.u8(p + off::kTextPrinterWindow);
        }
    }
    // waitbuttonpress: the global script context is running natively in
    // WaitForAorBPress. StopScript (script.c) resets `mode` but leaves
    // `nativePtr`, so the pointer alone stays stale after the script ends
    // and would make every later overworld tap an A press.
    s.text.script_wait_button =
        m.u8(addr::sGlobalScriptContext + 1) == 2 /* SCRIPT_MODE_NATIVE */ &&
        m.u8(addr::sGlobalScriptContextStatus) != 2 /* CONTEXT_SHUTDOWN */ &&
        (m.u32(addr::sGlobalScriptContext + 4) & ~1u) == fn::WaitForAorBPress;

    // sMenu-driven menus. The field Start menu is the one sMenu user that
    // moves its cursor itself (HandleStartMenuInput -> Menu_MoveCursor, a
    // wrapping move) instead of calling a Menu_Process* routine.
    const bool start_menu_input = s.field.overworld &&
        (m.u32(addr::gMenuCallback) & ~1u) == fn::HandleStartMenuInput;
    if (g_menu_probe || start_menu_input) {
        MenuState& menu = s.menu;
        menu.live = true;
        menu.grid = (g_menu_probe & kProbeGrid) != 0;
        menu.wrap = start_menu_input || (g_menu_probe & kProbeWrap) != 0;
        menu.left = m.u8(addr::sMenu + 0);
        menu.top = m.u8(addr::sMenu + 1);
        menu.cursor = m.s8(addr::sMenu + 2);
        menu.min = m.s8(addr::sMenu + 3);
        menu.max = m.s8(addr::sMenu + 4);
        menu.window = m.u8(addr::sMenu + 5);
        menu.option_w = m.u8(addr::sMenu + 7);
        menu.option_h = m.u8(addr::sMenu + 8);
        menu.columns = menu.grid ? std::max<int>(1, m.u8(addr::sMenu + 9)) : 1;
        menu.rows = menu.grid ? std::max<int>(1, m.u8(addr::sMenu + 10)) : 1;
        if (menu.option_h <= 0) menu.option_h = 16;
        menu.window_rect = window_rect(m, menu.window);
        if (menu.window_rect.empty() || menu.max < menu.min) menu.live = false;
    }
    s.start_menu = start_menu_input && s.menu.live &&
                   s.menu.window == m.u8(addr::sStartMenuWindowId);
    if (s.menu.live) s.text.dialogue_box = dialogue_window_visible(m, s.menu.window);

    // List menus.
    if (g_list_probe && g_list_task < 16) {
        ListState& list = s.list;
        const std::uint32_t t = addr::gTasks + g_list_task * off::kTaskSize;
        list.task = g_list_task;
        list.total = m.u16(t + off::kListTotal);
        list.max_showed = m.u16(t + off::kListMaxShowed);
        list.window = m.u8(t + off::kListWindow);
        list.item_x = m.u8(t + off::kListItemX);
        list.up_text_y = m.u8(t + off::kListUpTextY) & 0x0F;
        const int padding = (m.u8(t + off::kListPadding) >> 3) & 7;
        list.row_height = font_height(m.u8(t + off::kListFont) & 0x3F) + padding;
        list.scroll = m.u16(t + off::kListScroll);
        list.selected = m.u16(t + off::kListSelected);
        list.window_rect = window_rect(m, list.window);
        list.live = !list.window_rect.empty() && list.total > 0 && list.max_showed > 0;
        if (list.live && !s.text.dialogue_box)
            s.text.dialogue_box = dialogue_window_visible(m, list.window);
    }

    // Battle controllers: the first player-side battler with an input handler.
    if (s.kind == SceneKind::Battle) {
        BattleState& b = s.battle;
        b.active = true;
        b.type_flags = m.u32(addr::gBattleTypeFlags);
        const int count = std::min<int>(4, m.u8(addr::gBattlersCount));
        for (int i = 0; i < count; ++i) {
            if (m.u8(addr::gBattlerPositions + i) & 1) continue;  // opponent side
            const std::uint32_t f = m.func(addr::gBattlerControllerFuncs + 4 * i);
            BattleControl c = BattleControl::None;
            if (f == fn::HandleInputChooseAction) c = BattleControl::Action;
            else if (f == fn::SafariHandleInputChooseAction) c = BattleControl::SafariAction;
            else if (f == fn::HandleInputChooseMove) c = BattleControl::Move;
            else if (f == fn::HandleInputChooseTarget) c = BattleControl::Target;
            else if (f == fn::PlayerHandleYesNoInput) c = BattleControl::YesNo;
            else if (f == fn::HandleMoveSwitching) c = BattleControl::MoveSwitch;
            if (c == BattleControl::None) continue;
            b.battler = static_cast<std::uint8_t>(i);
            b.control = c;
            b.action_cursor = m.u8(addr::gActionSelectionCursor + i);
            b.move_cursor = m.u8(addr::gMoveSelectionCursor + i);
            b.target_cursor = m.u8(addr::gMultiUsePlayerCursor);
            b.moves = m.u8(addr::gNumberOfMovesToChoose);
            break;
        }
    }

    g_menu_probe = 0;
    g_list_probe = false;
    g_list_task = 0xFF;
    g_field_probe = false;
    return s;
}

}  // namespace emerald::touch
