// emerald_scene.h — per-frame classification of what the player is looking at.
//
// Inputs are read-only guest state (callbacks, tasks, menu/list/text structs,
// battle controllers) plus observe-only function-entry probes that record
// which menu-input routines actually ran since the previous frame (reviewed
// [[mod_function_hook]] entries in game.toml; the probes always decline, so
// the original guest body runs unchanged).

#pragma once

#include <cstdint>

#include "emerald_guest.h"

namespace emerald::touch {

enum class SceneKind : std::uint8_t {
    Unknown = 0, Boot, TitleMenus, Overworld, Battle, Bag, Party, Summary,
    Storage, Pokedex, Naming, Shop, Options, TrainerCard, Pokenav, EasyChat,
    FlyMap, Contest, Blender, Slots, Roulette, FactorySelect, PyramidBag,
    FrontierPass, Mail, Other,
};
const char* scene_name(SceneKind kind);

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool contains(float px, float py, float pad = 0.0f) const {
        return px >= x - pad && py >= y - pad && px < x + w + pad && py < y + h + pad;
    }
    bool empty() const { return w <= 0 || h <= 0; }
};

// sMenu-driven menus (start menu, multichoice, yes/no, context menus).
struct MenuState {
    bool live = false;          // a Menu_Process* routine ran last frame
    bool grid = false;
    bool wrap = false;
    std::uint8_t window = 0xFF;
    int left = 0, top = 0, cursor = 0, min = 0, max = 0;
    int option_w = 0, option_h = 0, columns = 1, rows = 1;
    Rect window_rect;           // native screen pixels
    Rect item(int index) const;
};

// ListMenu tasks (bag, shops, PC lists, scrolling multichoice).
struct ListState {
    bool live = false;          // ListMenu_ProcessInput ran last frame
    std::uint8_t task = 0xFF;
    std::uint8_t window = 0xFF;
    int total = 0, max_showed = 0, scroll = 0, selected = 0;
    int row_height = 16, up_text_y = 0, item_x = 0;
    Rect window_rect;
    Rect row(int visible_row) const;
};

enum class BattleControl : std::uint8_t {
    None = 0, Action, Move, Target, YesNo, MoveSwitch, SafariAction, Other,
};
const char* battle_control_name(BattleControl c);

struct BattleState {
    bool active = false;
    std::uint8_t battler = 0;
    BattleControl control = BattleControl::None;
    std::uint32_t type_flags = 0;
    int action_cursor = 0, move_cursor = 0, target_cursor = 0, moves = 4;
};

struct TextState {
    bool printing = false;      // a printer is emitting glyphs
    bool waiting = false;       // a printer waits for A/B (arrow shown)
    std::uint8_t window = 0xFF;
    bool script_wait_button = false;  // scrcmd waitbuttonpress
    // A dialogue-framed message window (menu.c WindowFunc_DrawDialogueFrame)
    // other than the live menu is on screen: the menu is a prompt answering
    // it (Yes/No, multichoice), not a free-standing menu.
    bool dialogue_box = false;
};

struct FieldState {
    bool overworld = false;     // CB2_Overworld is running
    bool input_ran = false;     // DoCB1_Overworld processed input last frame
    bool controls_locked = false;
    bool fading = false;
    bool script_running = false;
    bool free = false;          // the player may walk right now
};

struct SceneState {
    std::uint64_t frame = 0;
    SceneKind kind = SceneKind::Unknown;
    std::uint32_t cb1 = 0, cb2 = 0;
    FieldState field;
    MenuState menu;
    ListState list;
    BattleState battle;
    TextState text;
    bool start_menu = false;    // the field Start menu is open
    std::uint32_t task_mask = 0;   // bit i: gTasks[i] active
    std::uint32_t task_funcs[16] = {};
    // Stable identity of "the thing on screen" for coverage and abort checks.
    std::uint32_t signature() const;
};

// Observe-only probes, called from reviewed function-entry hooks.
void probe_menu_entry(std::uint32_t pc);
void probe_list_entry(std::uint8_t task_id);
void probe_field_input();
void register_scene_probes();   // constructor-time registration
void enable_scene_probes();     // after each mod activation pass

// Classify the current frame and consume the probe flags gathered since the
// previous call.
SceneState classify(const guest::Mem& m, std::uint64_t frame);

// Native screen rectangle of an allocated window (live BG scroll and map
// size). Empty when the window is not allocated.
Rect window_screen_rect(const guest::Mem& m, int window);

}  // namespace emerald::touch
