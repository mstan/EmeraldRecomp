#include "emerald_extended_view.h"
#include "emerald_object_view.h"
#include "emerald_ui_view.h"

#include <cstdio>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <memory>

using emerald::ViewStatus;
void require(bool pass, const char* message) {
    if (!pass) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}

struct Fixture {
    std::vector<std::uint8_t> ewram = std::vector<std::uint8_t>(0x40000);
    std::vector<std::uint8_t> iwram = std::vector<std::uint8_t>(0x8000);
    std::vector<std::uint8_t> rom = std::vector<std::uint8_t>(0x10000);
    std::vector<std::uint8_t> vram = std::vector<std::uint8_t>(0x18000);
    std::vector<std::uint8_t> io = std::vector<std::uint8_t>(0x400);
    emerald::ViewMemory memory() const {
        return {ewram.data(), iwram.data(), rom.data(), rom.size(), vram.data(), io.data()};
    }
    void word(std::uint32_t addr, unsigned value) {
        auto* p = const_cast<std::uint8_t*>(memory().bytes(addr, 2));
        require(p != nullptr, "bad fixture address");
        p[0] = value; p[1] = value >> 8;
    }
    void dword(std::uint32_t addr, unsigned value) { word(addr, value); word(addr + 2, value >> 16); }
    void reg(int addr, unsigned value) { io[addr] = value; io[addr+1] = value >> 8; }
    Fixture() {
        dword(0x030022c4, 0x08085e5d);
        dword(0x02037318, 0x08000100);
        dword(0x08000108, 0x08000200); // border
        dword(0x08000110, 0x08000300); // primary
        dword(0x08000114, 0x08000318); // secondary
        dword(0x0800030c, 0x08001000);
        dword(0x08000310, 0x08005000);
        dword(0x08000324, 0x08003000);
        dword(0x08000328, 0x08005400);
        dword(0x03005dc0, 40);
        dword(0x03005dc4, 32);
        dword(0x03005dc8, 0x02000000);
        dword(0x03005d8c, 0x02010000);
        word(0x02010000, 10); word(0x02010002, 5);
        // Unique metatile per column. Explicit NORMAL layout: BG3 garbage,
        // BG2 bottom four tiles, BG1 upper four (including flip/palette bits).
        for (int id = 0; id < 512; ++id) {
            for (int q = 0; q < 8; ++q) word(0x08001000 + id*16 + q*2, 0x1400 + id*8 + q);
        }
        for (int y = 0; y < 32; ++y) for (int x = 0; x < 40; ++x)
            word(0x02000000 + (y*40+x)*2, x);
        reg(0, 0x0e00);
        for (int bg = 1; bg <= 3; ++bg) reg(8 + bg*2, (27+bg)*256 + bg);
        ring();
    }
    void ring(int origin_x = 20, int origin_y = 10, int rx = 0, int ry = 0) {
        for (int y = 0; y < 32; ++y) for (int x = 0; x < 32; ++x) {
            const int wx = origin_x + x, wy = origin_y + y;
            const int id = wx / 2, q = (wx & 1) + (wy & 1)*2;
            const unsigned entries[] = {unsigned(0x1400+id*8+q+4), unsigned(0x1400+id*8+q), 0x3014};
            for (int bg = 0; bg < 3; ++bg) {
                int addr = (28+bg)*0x800 + (((y+ry)&31)*32+((x+rx)&31))*2;
                vram[addr] = entries[bg]; vram[addr+1] = entries[bg] >> 8;
            }
        }
    }
};

int main() {
    {
        Fixture npc;
        npc.rom.resize(0x600000);
        npc.dword(0x08000100, 25); npc.dword(0x08000104, 18);
        npc.dword(0x0800010c, 0x08008000);
        npc.dword(0x0203731c, 0x08006800); npc.word(0x08006800, 1);
        const unsigned t = 0x02010c70, sprite = 0x02020630;
        npc.word(t, 0x0101); // local 1, graphics 1
        npc.word(t+4, 5); npc.word(t+6, 12); npc.word(t+8, 0x803);
        npc.dword(0x08505624, 0x08009600);
        npc.word(0x08009602, 0x1100); npc.word(0x08009608, 16); npc.word(0x0800960a, 32);
        npc.word(0x0800960c, 2); npc.word(0x0850bdec, 0x1100);
        npc.dword(0x08009618, 0x08009700); npc.dword(0x08009700, 0x08009710);
        npc.word(0x08009710, 0); npc.dword(0x0800961c, 0x08009720);
        npc.dword(0x08009720, 0x0800a000); npc.word(0x08009724, 256);
        for (int i = 0; i < 256; i += 2) npc.word(0x0800a000+i, 0x1111);
        npc.word(0x085055d5, 1); // face south; animation index zero
        npc.word(0x0850e636, 0x0200); // elevation 3 -> priority 2
        std::vector<std::uint8_t> oam(0x400), pal(0x400);
        auto memory = npc.memory(); memory.oam = oam.data(); memory.pal = pal.data();
        pal[0x242] = 31;
        npc.reg(0, 0x1e40); npc.dword(0x030022e0, 100);
        emerald::FieldView field;
        auto objects = std::make_unique<emerald::ObjectView>();
        require(field.prepare(memory, 240, 854) == ViewStatus::Ready, "dormant NPC map failed");
        const auto check = [&](int x, bool visible, const char* message) {
            int left, width;
            const auto* row = objects->row(208, &left, &width);
            require(row && ((row[x-left].color == 31) == visible), message);
        };
        const auto saved_ram = npc.ewram;
        require(objects->prepare(memory, field, 240, 854) && objects->dormant_objects() == 1,
                "unspawned current-map NPC missing");
        check(32, true, "dormant ROM image or map coordinates wrong");
        require(npc.ewram == saved_ram, "dormant renderer changed guest memory");
        // An active sprite owns this identity, then its last position survives
        // native despawning. No duplicate appears at the template coordinates.
        npc.ewram[0x37350] = 1; npc.ewram[0x37351] = 0x40;
        npc.ewram[0x37355] = 1; npc.ewram[0x37358] = 1;
        npc.ewram[0x37368] = 1;
        npc.word(0x0203735c, 12); npc.word(0x0203735e, 19);
        npc.word(sprite, 0x8000); npc.word(sprite+2, 0x8000); npc.word(sprite+4, 0x2800);
        npc.word(sprite+32, 56); npc.word(sprite+34, 224);
        npc.word(sprite+40, 0xf0f8); npc.word(sprite+62, 3);
        npc.dword(0x030022e0, 101);
        require(objects->prepare(memory, field, 240, 854) && !objects->dormant_objects(), "active NPC duplicated");
        npc.ewram[0x37350] = 0; npc.dword(0x030022e0, 102);
        objects->prepare(memory, field, 240, 854);
        check(48, true, "despawn discarded last known NPC position");
        check(32, false, "cached NPC duplicated at spawn point");
        npc.word(t+4, 8); objects->prepare(memory, field, 240, 854);
        check(80, true, "scripted template relocation lost to cached NPC pose");
        check(48, false, "scripted template relocation left stale NPC");
        npc.word(t+4, 5);
        npc.word(t+20, 1); npc.ewram[0x11270] = 2;
        npc.dword(0x030022e0, 103); objects->prepare(memory, field, 240, 854);
        require(!objects->dormant_objects(), "story flag resurrected hidden NPC");
        npc.ewram[0x11270] = 0; npc.dword(0x030022e0, 50);
        objects->prepare(memory, field, 240, 854);
        check(32, true, "savestate rewind retained stale NPC pose");
        check(48, false, "savestate rewind duplicated NPC pose");
        npc.word(t+8, 0x4c03); objects->prepare(memory, field, 240, 854);
        require(!objects->dormant_objects(), "invisible movement template was revealed");
    }
    {
        Fixture maps;
        maps.rom.resize(0x500000);
        maps.dword(0x08000100, 25); maps.dword(0x08000104, 18);
        maps.dword(0x0800010c, 0x08008000);
        maps.dword(0x02037324, 0x08006000);
        maps.dword(0x08006000, 4); maps.dword(0x08006004, 0x08006100);
        maps.dword(0x08486578, 0x08007000);
        maps.dword(0x08007000, 0x02037318);
        for (int direction = 1; direction <= 4; ++direction) {
            const unsigned header = 0x08009000 + direction * 0x100;
            const unsigned layout = header + 0x20, data = 0x0800a000 + (direction-1)*0x400;
            const unsigned connection = 0x08006100 + (direction-1)*12;
            maps.word(connection, direction);
            maps.dword(connection+4, direction % 2 ? -3 : 3);
            // North/east use +3; south/west use -3, covering signed offsets.
            maps.word(connection+8, direction*256);
            maps.dword(0x08007000+direction*4, header);
            maps.dword(header, layout);
            maps.dword(layout, 20); maps.dword(layout+4, 20);
            maps.dword(layout+12, data);
            maps.dword(layout+16, 0x08000300); maps.dword(layout+20, 0x08000318);
            for (int y = 0; y < 20; ++y) for (int x = 0; x < 20; ++x)
                maps.word(data + (y*20+x)*2, direction*50+x+2*y);
        }
        // A reciprocal east/west link exercises graph cycle elimination.
        maps.dword(0x0800940c, 0x08006200);
        maps.dword(0x08006200, 1); maps.dword(0x08006204, 0x08006210);
        maps.word(0x08006210, 3); maps.dword(0x08006214, -3);
        maps.word(0x08006218, 0);
        // A second northern map lies within an especially tall viewport.
        maps.dword(0x0800920c, 0x08006300);
        maps.dword(0x08006300, 1); maps.dword(0x08006304, 0x08006310);
        maps.word(0x08006310, 2); maps.dword(0x08006314, 0);
        maps.word(0x08006318, 5*256); maps.dword(0x08007014, 0x08009500);
        maps.dword(0x08009500, 0x08009520);
        maps.dword(0x08009520, 20); maps.dword(0x08009524, 10);
        maps.dword(0x0800952c, 0x0800b000);
        maps.dword(0x08009530, 0x08000300); maps.dword(0x08009534, 0x08000318);
        maps.word(0x0800b000 + 7*20*2, 321);
        // Missing cells in the padded grid and beyond both vertical ends.
        maps.word(0x02000000 + (10*40+0)*2, 0x3ff);
        maps.word(0x02000000 + (10*40+34)*2, 0x3ff);
        const auto ewram = maps.ewram, iwram = maps.iwram, rom = maps.rom;
        emerald::FieldView field;
        require(field.prepare(maps.memory(), 569, 854) == ViewStatus::Ready, "connected field rejected");
        const auto check = [&](int x, int y, unsigned id, const char* message) {
            std::uint16_t tile = 0;
            require(field.tile(2, (x-10)*16, (y-5)*16, &tile) && tile == 0x1400+id*8, message);
        };
        check(10, -8, 110, "north connection became border beyond padding");
        check(10, -16, 321, "second visible connected map was not resolved");
        check(10, 34, 74, "south connection became border beyond padding");
        check(0, 10, 175, "west connection offset wrong");
        check(34, 10, 202, "east connection offset wrong");
        require(maps.ewram == ewram && maps.iwram == iwram && maps.rom == rom,
                "connected view mutated guest maps");
        // A live mutation in the copied connection strip must beat ROM.
        maps.word(0x02000000 + (10*40+34)*2, 42);
        field.prepare(maps.memory(), 569, 854);
        check(34, 10, 42, "ROM replaced a live connection edit");
        // Reject unavailable graphics and stale connections immediately.
        maps.dword(0x08009234, 0x08000300);
        field.prepare(maps.memory(), 569, 854);
        check(10, -8, 0, "unloaded neighboring tileset was interpreted as current graphics");
        maps.dword(0x02037324, 0);
        field.prepare(maps.memory(), 569, 854);
        check(10, 34, 0, "removed connection retained cached scenery");
    }
    {
        Fixture portrait;
        emerald::FieldView field;
        require(field.prepare(portrait.memory(), 240, 427) == ViewStatus::Ready, "portrait field rejected");
        std::uint16_t tile;
        require(field.tile(2, 0, -133, &tile) && field.tile(2, 239, 293, &tile), "portrait bounds missing");
        require(!field.tile(2, 0, -134, &tile) && !field.tile(2, 0, 294, &tile), "portrait bounds exceeded");
        // Real Start menu: 7x14 interior at (22,1), with a one-tile frame.
        portrait.reg(8, 27 * 256);
        portrait.word(0x02020004, 22 * 256);
        portrait.word(0x02020006, 1 + 7 * 256);
        portrait.word(0x02020008, 14 + 15 * 256);
        portrait.word(0x0202000a, 0x139);
        portrait.dword(0x0202000c, 0x02021000);
        const int at = 27 * 0x800 + (32 + 22) * 2;
        portrait.vram[at] = 0x39; portrait.vram[at+1] = 0xf1;
        emerald::UiView ui;
        ui.prepare(portrait.memory(), field, 240, 427);
        int sx = 0, sy = 0;
        // The owner-selected portrait layout centres this menu: its interior
        // centre is y=64, so it moves down 16 native pixels to view centre 80.
        require(ui.sample(0, 176, 24, &sx, &sy) == 1 && sx == 176 && sy == 8, "portrait Start menu not centred");
        require(ui.sample(0, 176, 8, &sx, &sy) == -1, "old menu location not suppressed");
        require(field.prepare(portrait.memory(), 569) == ViewStatus::Ready, "wide menu field rejected");
        ui.prepare(portrait.memory(), field, 569, 160);
        require(ui.sample(0, 341, 8, &sx, &sy) == 1 && sx == 176 && sy == 8, "wide Start menu not right anchored");
        // Hiding the published window must remove its mapping immediately.
        portrait.vram[at] = portrait.vram[at+1] = 0;
        ui.prepare(portrait.memory(), field, 569, 160);
        require(ui.sample(0, 341, 8, &sx, &sy) == -1, "hidden menu retained stale placement");
    }
    {
        Fixture door;
        door.rom.resize(0x500000);
        door.word(0x08497174, 13);
        door.word(0x08497176, 0x100); // ordinary one-metatile door
        door.dword(0x0849717c, 0x08009000);
        for (int i = 0; i < 12; i += 2) door.word(0x08009000 + i, 0x0202);
        const unsigned entries[] = {0, 0x2000, 0x23fc};
        for (int bg = 0; bg < 3; ++bg) {
            const int offset = (28 + bg) * 0x800 + (6 * 32 + 6) * 2;
            door.vram[offset] = entries[bg]; door.vram[offset+1] = entries[bg] >> 8;
        }
        emerald::FieldView field;
        require(field.prepare(door.memory(), 569) == ViewStatus::Ready, "animated door caused fallback");
        door.vram[30 * 0x800 + (6 * 32 + 6) * 2] ^= 1;
        require(field.prepare(door.memory(), 569) == ViewStatus::Unverified, "unrelated door-shaped corruption accepted");
    }
    // An NPC straddling the native edge, with one 16x32 subsprite. The live
    // guest OAM is the validation source; then exercise the software culler.
    {
        Fixture npc;
        std::vector<std::uint8_t> oam(0x400), pal(0x400);
        auto memory = npc.memory(); memory.oam = oam.data(); memory.pal = pal.data();
        npc.reg(0, 0x1e40);
        npc.ewram[0x37350] = 1;
        const unsigned sprite = 0x02020630;
        npc.word(sprite, 0x8000); npc.word(sprite+2, 0x81f0); npc.word(sprite+4, 0x0800);
        npc.word(sprite+32, -8); npc.word(sprite+34, 16);
        npc.word(sprite+40, 0xf0f8); // center-to-corner (-8,-16)
        npc.word(sprite+62, 3); npc.word(sprite+66, 0x8041);
        npc.dword(sprite+24, 0x08006000);
        npc.word(0x08006008, 1); npc.dword(0x0800600c, 0x08006018);
        npc.word(0x08006018, 0xf0f8); npc.word(0x0800601a, 0x800a);
        std::copy_n(npc.ewram.data()+0x20630, 6, oam.data());
        std::fill_n(npc.vram.begin()+0x10000, 256, 0x11);
        pal[0x202] = 31;
        emerald::FieldView field;
        // Large host framebuffer stays off the small Windows stack.
        auto objects = std::make_unique<emerald::ObjectView>();
        require(field.prepare(memory, 569) == ViewStatus::Ready, "NPC fixture field failed");
        require(objects->prepare(memory, field, 569) && objects->verified_parts() == 1,
                "visible NPC failed OAM validation");
        int left, width;
        const auto* row = objects->row(8, &left, &width);
        require(row && row[-16-left].color == 31 && row[-16-left].priority == 2,
                "NPC lost its margin pixels or subsprite priority");
        require(row[-left].color == 0x8000, "NPC shadow wrote into native center");
        // Next-frame Sprite.oam may lead hardware by a pixel and a turn.
        npc.word(sprite+2, 0x91f1);
        require(objects->prepare(memory, field, 569), "bounded OAM publish lag rejected");
        row = objects->row(8,&left,&width);
        require(row[-16-left].color == 31 && row[-left].color == 0x8000,
                "next-frame coordinates replaced published NPC position");
        npc.word(sprite+2, 0x81f0);
        // Recycled/mismatched OAM must fail closed immediately.
        // Put part of this sprite inside the native screen for the check.
        npc.word(sprite+32, 0); npc.word(sprite+2, 0x81f8);
        oam[2] = 0xf8;
        oam[4] ^= 1;
        require(!objects->prepare(memory, field, 569) && !objects->row(8,&left,&width),
                "stale OAM did not invalidate NPC shadow");
        npc.ewram[0x37351] = 0x40; // offScreen, NOT script-invisible
        npc.ewram[0x20630+62] = 7;
        npc.word(sprite+32, -40);
        require(objects->prepare(memory, field, 569), "software-culled live NPC rejected");
        row = objects->row(8,&left,&width);
        require(row[-48-left].color == 31, "software-culled NPC disappeared");
        // The same loaded NPC above the native screen must appear in portrait,
        // without copying its pixels into the original 240x160 center.
        npc.word(sprite+32, 80); npc.word(sprite+34, -8);
        require(field.prepare(memory, 240, 427) == ViewStatus::Ready &&
                objects->prepare(memory, field, 240, 427), "portrait NPC decode failed");
        row = objects->row(-20, &left, &width);
        require(row && row[72].color == 31, "vertical NPC margin missing");
        row = objects->row(0, &left, &width);
        require(row && row[72].color == 0x8000, "vertical NPC touched native center");
        npc.word(sprite+32, -40); npc.word(sprite+34, 16);
        field.prepare(memory, 569);
        npc.ewram[0x37351] |= 0x20;
        require(objects->prepare(memory, field, 569), "script-hidden NPC fixture failed");
        row = objects->row(8,&left,&width);
        require(row[-48-left].color == 0x8000, "script-hidden NPC resurrected");
        npc.ewram[0x37350] = 0;
        require(objects->prepare(memory, field, 569) && objects->objects() == 0,
                "removed object left a stale sprite");
        field.prepare(memory, 240);
        require(!objects->prepare(memory, field, 569) && !objects->row(8,&left,&width),
                "NPC layer survived native/non-field reset");
    }
    Fixture f;
    emerald::FieldView view;
    require(view.prepare(f.memory(), 240) == ViewStatus::Native, "native mode not inert");
    const auto ewram = f.ewram, iwram = f.iwram, rom = f.rom, vram = f.vram, io = f.io;
    for (int width : {241, 284, 373, 569}) {
        require(view.prepare(f.memory(), width) == ViewStatus::Ready, "field mapping rejected");
        std::uint16_t tile = 0;
        require(view.tile(1, 240, 0, &tile) && tile == 0x14cc, "right margin wrapped instead of showing unseen map");
        require(!view.tile(0, 240, 0, &tile), "UI background repeated into margins");
        require(!view.tile(1, 5000, 0, &tile) && !view.tile(1, 240, 160, &tile), "output bounds not enforced");
    }
    require(f.ewram == ewram && f.iwram == iwram && f.rom == rom && f.vram == vram && f.io == io,
            "renderer mutated guest state");
    // Camera RAM steps ahead of displayed VRAM; verification must recover.
    f.word(0x02010000, 11);
    require(view.prepare(f.memory(), 569) == ViewStatus::Ready, "one-metatile VBlank lag not resolved");
    f.word(0x02010000, 14);
    require(view.prepare(f.memory(), 569) == ViewStatus::Unverified, "unverified camera did not fail closed");
    f.word(0x02010000, 10);
    // Nonzero subpixel scrolling and a ring wrap must retain the same map.
    f.iwram[0xe22] = 30; f.iwram[0xe23] = 28;
    for (int bg=1; bg<=3; ++bg) { f.reg(0x10+bg*4, 247); f.reg(0x12+bg*4, 229); }
    f.ring(20, 10, 30, 28);
    require(view.prepare(f.memory(), 569) == ViewStatus::Ready, "wrapped camera mapping failed");
    std::uint16_t tile = 0;
    require(view.tile(2, -8, 0, &tile) && tile == 0x1449, "negative margin floor or quadrant wrong");
    f.reg(0, 0x6e00);
    require(view.prepare(f.memory(), 569) == ViewStatus::Ready, "ordinary field UI windows rejected");
    f.reg(0, 0x0e80);
    require(view.prepare(f.memory(), 569) == ViewStatus::Unsupported, "forced-blank transition not suppressed");
    f.reg(0, 0x0e00);
    f.dword(0x03005dc8, 0x0203ffff);
    require(view.prepare(f.memory(), 569) == ViewStatus::Unsupported, "invalid map pointer accepted");
    f.dword(0x030022c4, 0x08000001);
    require(view.prepare(f.memory(), 569) == ViewStatus::NonField, "battle/menu retained stale world");
    require(!view.tile(1, 240, 0, &tile), "stale margin survived fallback");
    require(!f.memory().bytes(0x0203ffff, 2) && !f.memory().bytes(0x0800ffff, 2) &&
            !f.memory().bytes(0xfffffff0, 32), "region boundary reads not rejected");
    Fixture secondary;
    secondary.word(0x02000000 + (5*40+25)*2, 0xCE00); // secondary id 512, collision/elevation
    secondary.word(0x08003000, 0x2222);
    secondary.word(0x08003008, 0x4444);
    secondary.word(0x08005400, 0x2000); // SPLIT layers
    require(view.prepare(secondary.memory(), 569) == ViewStatus::Ready, "secondary tileset rejected");
    require(view.tile(1, 240, 0, &tile) && tile == 0x4444, "secondary top layer wrong");
    require(view.tile(2, 240, 0, &tile) && tile == 0, "split middle layer not transparent");
    require(view.tile(3, 240, 0, &tile) && tile == 0x2222, "secondary bottom layer wrong");
    secondary.word(0x08005400, 0x1000); // COVERED layers
    require(view.prepare(secondary.memory(), 569) == ViewStatus::Ready, "covered metatile rejected");
    require(view.tile(1, 240, 0, &tile) && tile == 0, "covered top layer not transparent");
    require(view.tile(2, 240, 0, &tile) && tile == 0x4444, "covered middle layer wrong");
    secondary.word(0x02000000 + (5*40+25)*2, 0x3FF);
    secondary.word(0x08000200, 7);
    require(view.prepare(secondary.memory(), 569) == ViewStatus::Ready, "authored border rejected");
    require(view.tile(1, 240, 0, &tile) && tile == 0x143C, "undefined cell did not use authored border");
    std::puts("Emerald field view: unseen tiles, camera lag/wrap, bounds, fallback, and read-only checks passed");
}
