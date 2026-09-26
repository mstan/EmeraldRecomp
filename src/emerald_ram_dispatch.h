#pragma once
#include "runtime_arm.h"

extern "C" void gf_ReadFlash1(void);
extern "C" void gf_ReadFlash_Core(void);

namespace emerald {
inline bool ram_matches_rom(uint32_t ram_pc, uint32_t rom_pc, uint32_t size) {
    for (uint32_t offset=0; offset<size; ++offset)
        if (bus_read_u8(ram_pc+offset)!=bus_read_u8(rom_pc+offset)) return false;
    return true;
}
// The ROM routines are position independent. Byte validation and the live
// callback guard keep reused stack slots from invoking stale native code.
// Shared by the regular runner and each independent multiplayer instance.
inline int ram_dispatch(uint32_t pc, int thumb) {
    constexpr uint32_t read_flash1=0x082E1A6C, callback=0x03007844;
    constexpr uint32_t read_core=0x082E1AB0, core_size=0x22;
    if (!thumb) return 0;
    if (bus_read_u32(callback)==(pc|1u) && ram_matches_rom(pc,read_flash1,4)) {
        gf_ReadFlash1(); return 1;
    }
    if (ram_matches_rom(pc,read_core,core_size)) {
        gf_ReadFlash_Core(); return 1;
    }
    return 0;
}
}
