// emerald_guest.h — read-only guest facts for the Emerald touch policy.
//
// Addresses come from the pinned pret/pokeemerald build that reproduces the
// USA ROM (sha1 f3ae0881…, variants/emerald/symbols/pokeemerald_emerald_syms.txt);
// struct offsets are from that revision's headers. Thumb function addresses
// are listed with the Thumb bit clear, as they appear in function pointers
// once masked. Everything here READS guest memory; the touch policy steers
// the game only through synthesized key presses.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace emerald::guest {

namespace addr {
// Core
constexpr std::uint32_t gMain = 0x030022C0;            // callback1 +0, callback2 +4
constexpr std::uint32_t gTasks = 0x03005E00;           // 16 x 0x28
constexpr std::uint32_t gWindows = 0x02020004;         // 32 x 12
constexpr std::uint32_t sTextPrinters = 0x020201B0;    // 32 x 0x24
constexpr std::uint32_t gPaletteFade = 0x02037FD4;
constexpr std::uint32_t gSaveBlock1Ptr = 0x03005D8C;
constexpr std::uint32_t gSaveBlock2Ptr = 0x03005D90;
constexpr std::uint32_t gSprites = 0x02020630;         // 64 x 0x44
constexpr std::uint32_t gSpriteCoordOffsetX = 0x02021BBC;
constexpr std::uint32_t gSpriteCoordOffsetY = 0x02021BBE;
// Menus
constexpr std::uint32_t sMenu = 0x0203CD90;            // menu.o, 12 bytes
constexpr std::uint32_t sYesNoWindowId = 0x0203CD9F;
constexpr std::uint32_t sStartMenuWindowId = 0x0203CD8C;
constexpr std::uint32_t sStartMenuCursorPos = 0x0203760E;
constexpr std::uint32_t sNumStartMenuActions = 0x0203760F;
constexpr std::uint32_t sCurrentStartMenuActions = 0x02037610;
constexpr std::uint32_t sProcessInputDelay = 0x02039F90; // script_menu.o
constexpr std::uint32_t gMenuCallback = 0x03005DF4;      // start_menu.c
// Field
constexpr std::uint32_t gPlayerAvatar = 0x02037590;
constexpr std::uint32_t gObjectEvents = 0x02037350;    // 16 x 0x24
constexpr std::uint32_t gBackupMapLayout = 0x03005DC0; // width, height, map*
constexpr std::uint32_t gMapHeader = 0x02037318;
constexpr std::uint32_t gFieldCamera = 0x03005DD0;
constexpr std::uint32_t sHorizontalCameraPan = 0x03000E28;
constexpr std::uint32_t sVerticalCameraPan = 0x03000E2A;
constexpr std::uint32_t sLockFieldControls = 0x03000F2C;
constexpr std::uint32_t sGlobalScriptContextStatus = 0x03000E38;
constexpr std::uint32_t sGlobalScriptContext = 0x03000E40;
constexpr std::uint32_t sFieldMessageBoxMode = 0x020375BC;
// Battle
constexpr std::uint32_t gBattleTypeFlags = 0x02022FEC;
constexpr std::uint32_t gBattlerControllerFuncs = 0x03005D60; // 4 x ptr
constexpr std::uint32_t gActiveBattler = 0x02024064;
constexpr std::uint32_t gBattlersCount = 0x0202406C;
constexpr std::uint32_t gBattlerPositions = 0x02024076;
constexpr std::uint32_t gActionSelectionCursor = 0x020244AC;  // [4]
constexpr std::uint32_t gMoveSelectionCursor = 0x020244B0;    // [4]
constexpr std::uint32_t gMultiUsePlayerCursor = 0x03005D74;
constexpr std::uint32_t gNumberOfMovesToChoose = 0x03005D78;
constexpr std::uint32_t gBattleMons = 0x02024084;             // 4 x 0x58
constexpr std::uint32_t gBattle_BG0_X = 0x02022E14;
constexpr std::uint32_t gBattle_BG0_Y = 0x02022E16;
constexpr std::uint32_t gMoveNames = 0x0831977C;              // 13 bytes each
constexpr std::uint32_t gTypeNames = 0x0831AE38;              // 7 bytes each
constexpr std::uint32_t gBattleMoves = 0x0831C898;            // 12 bytes each
// Bag / party
constexpr std::uint32_t gBagPosition = 0x0203CE58;
constexpr std::uint32_t gBagMenu = 0x0203CE54;
constexpr std::uint32_t gPartyMenu = 0x0203CEC8;
constexpr std::uint32_t gPlayerPartyCount = 0x020244E9;
}  // namespace addr

namespace fn {
constexpr std::uint32_t CB1_Overworld = 0x08085E04;
constexpr std::uint32_t CB2_Overworld = 0x08085E5C;
constexpr std::uint32_t CB1_OverworldLink = 0x08086BD8;
constexpr std::uint32_t CB2_ReturnToField = 0x080860C8;
constexpr std::uint32_t DoCB1_Overworld = 0x08085DAC;
constexpr std::uint32_t BattleMainCB2 = 0x08038420;
constexpr std::uint32_t CB2_InitBattle = 0x08036760;
constexpr std::uint32_t CB2_HandleStartBattle = 0x08036FAC;
constexpr std::uint32_t HandleInputChooseAction = 0x08057588;
constexpr std::uint32_t HandleInputChooseMove = 0x08057BFC;
constexpr std::uint32_t HandleInputChooseTarget = 0x08057824;
constexpr std::uint32_t PlayerHandleYesNoInput = 0x080599D4;
constexpr std::uint32_t HandleMoveSwitching = 0x08058138;
constexpr std::uint32_t SafariHandleInputChooseAction = 0x081593D8;
constexpr std::uint32_t Menu_ProcessInput = 0x0819856C;
constexpr std::uint32_t Menu_ProcessInputNoWrap = 0x081985D8;
constexpr std::uint32_t ProcessMenuInput_other = 0x08198658;
constexpr std::uint32_t Menu_ProcessInputNoWrapAround_other = 0x081986C4;
constexpr std::uint32_t Menu_ProcessInputNoWrapClearOnChoose = 0x08198C58;
constexpr std::uint32_t Menu_ProcessGridInput = 0x08199334;
constexpr std::uint32_t ListMenuDummyTask = 0x081AE458;
constexpr std::uint32_t ListMenu_ProcessInput = 0x081AE604;
constexpr std::uint32_t WaitForAorBPress = 0x0809AC98;
constexpr std::uint32_t IsFieldMessageBoxHidden = 0x0809833C;
constexpr std::uint32_t Task_HandleYesNoInput = 0x080E215C;
constexpr std::uint32_t Task_HandleMultichoiceInput = 0x080E2058;
constexpr std::uint32_t HandleStartMenuInput = 0x0809FAC4;
constexpr std::uint32_t Task_ShowStartMenu = 0x0809FA34;
constexpr std::uint32_t CB2_BagMenuRun = 0x081AAD5C;
constexpr std::uint32_t Task_BagMenu_HandleInput = 0x081ABD28;
constexpr std::uint32_t CB2_UpdatePartyMenu = 0x081B01B0;
constexpr std::uint32_t CB2_MainMenu = 0x0802F6B0;
constexpr std::uint32_t Task_HandleMainMenuInput = 0x0803024C;
constexpr std::uint32_t CB2_InitTitleScreen = 0x080AA7A4;
constexpr std::uint32_t TitleScreenMainCB2 = 0x080AAB2C;   // title_screen.o MainCB2
constexpr std::uint32_t OptionMenuMainCB2 = 0x080BA4B0;    // option_menu.o MainCB2
constexpr std::uint32_t SummaryMainCB2 = 0x081BFAB4;       // pokemon_summary_screen.o
constexpr std::uint32_t CB2_PokeStorage = 0x080C7D54;
constexpr std::uint32_t CB2_Pokedex = 0x080BB774;
constexpr std::uint32_t CB2_NamingScreen = 0x080E4F58;
constexpr std::uint32_t CB2_BuyMenu = 0x080DFD64;
constexpr std::uint32_t CB2_TrainerCard = 0x080C2710;
constexpr std::uint32_t CB2_Pokenav = 0x081C7400;
constexpr std::uint32_t CB2_EasyChatScreen = 0x0811A278;
constexpr std::uint32_t CB2_ContestMain = 0x080D823C;
constexpr std::uint32_t CB2_PlayBlender = 0x08081898;
constexpr std::uint32_t CB2_SlotMachine = 0x0812A670;
constexpr std::uint32_t CB2_Roulette = 0x08140238;
constexpr std::uint32_t CB2_FlyMap = 0x081248D4;
constexpr std::uint32_t CB2_PyramidBag = 0x081C501C;
constexpr std::uint32_t CB2_FrontierPass = 0x080C5438;
constexpr std::uint32_t CB2_FactorySelect = 0x0819A4C8;
constexpr std::uint32_t CB2_MailRead = 0x08121C64;
}  // namespace fn

// Struct offsets (pokeemerald c2dcc629).
namespace off {
constexpr std::uint32_t kTaskSize = 0x28, kTaskFunc = 0, kTaskActive = 4, kTaskData = 8;
constexpr std::uint32_t kWindowSize = 12;   // bg, left, top, width, height, pal, base(u16), data*
constexpr std::uint32_t kTextPrinterSize = 0x24, kTextPrinterActive = 0x1B, kTextPrinterState = 0x1C;
constexpr std::uint32_t kTextPrinterWindow = 0x04;  // printerTemplate.windowId
// ListMenu inside Task data (task + 8).
constexpr std::uint32_t kListItems = 0x08, kListTotal = 0x14, kListMaxShowed = 0x16,
                        kListWindow = 0x18, kListItemX = 0x1A, kListCursorX = 0x1B,
                        kListUpTextY = 0x1C, kListPadding = 0x1E, kListFont = 0x1F,
                        kListScroll = 0x20, kListSelected = 0x22;
constexpr std::uint32_t kObjectEventSize = 0x24;
}  // namespace off

// Maximum glyph height per font id (text.c sFontInfos).
inline int font_height(unsigned font_id) {
    static const std::uint8_t heights[] = {12, 16, 14, 14, 14, 14, 16, 16, 8};
    return font_id < sizeof(heights) ? heights[font_id] : 16;
}

// Decode a Gen III string (charmap.txt subset used by names) into UTF-8.
std::string decode_text(const std::uint8_t* src, std::size_t max_len);

// Side-effect-free reads over the bus's backing stores.
struct Mem {
    const std::uint8_t* ewram = nullptr;
    const std::uint8_t* iwram = nullptr;
    const std::uint8_t* rom = nullptr;
    std::size_t rom_size = 0;
    const std::uint8_t* vram = nullptr;
    const std::uint8_t* io = nullptr;
    const std::uint8_t* oam = nullptr;
    const std::uint8_t* pal = nullptr;

    static Mem current();
    bool ok() const { return ewram && iwram && rom && io; }

    const std::uint8_t* ptr(std::uint32_t a, std::size_t n) const {
        if (a >= 0x02000000u && a + n <= 0x02040000u && ewram) return ewram + (a - 0x02000000u);
        if (a >= 0x03000000u && a + n <= 0x03008000u && iwram) return iwram + (a - 0x03000000u);
        if (a >= 0x08000000u && rom && a - 0x08000000u + n <= rom_size) return rom + (a - 0x08000000u);
        if (a >= 0x06000000u && a + n <= 0x06018000u && vram) return vram + (a - 0x06000000u);
        return nullptr;
    }
    std::uint8_t u8(std::uint32_t a) const { auto* p = ptr(a, 1); return p ? p[0] : 0; }
    std::int8_t s8(std::uint32_t a) const { return static_cast<std::int8_t>(u8(a)); }
    std::uint16_t u16(std::uint32_t a) const {
        auto* p = ptr(a, 2);
        return p ? static_cast<std::uint16_t>(p[0] | (p[1] << 8)) : 0;
    }
    std::int16_t s16(std::uint32_t a) const { return static_cast<std::int16_t>(u16(a)); }
    std::uint32_t u32(std::uint32_t a) const {
        auto* p = ptr(a, 4);
        return p ? (std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
                    (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24)) : 0;
    }
    std::uint16_t io16(std::uint32_t off) const {
        return io ? static_cast<std::uint16_t>(io[off] | (io[off + 1] << 8)) : 0;
    }
    // Function pointer with the Thumb bit cleared.
    std::uint32_t func(std::uint32_t a) const { return u32(a) & ~1u; }
};

}  // namespace emerald::guest
