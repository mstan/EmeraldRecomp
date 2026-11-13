#include "emerald_object_view.h"
#include <algorithm>
#include <cstdlib>

namespace emerald {
namespace {
constexpr std::uint32_t kObjects = 0x02037350;
constexpr std::uint32_t kSprites = 0x02020630;
constexpr std::uint32_t kOffsetX = 0x02021BBC;
constexpr std::uint32_t kOffsetY = 0x02021BBE;
unsigned u16(const std::uint8_t* p) { return p[0] | (unsigned(p[1]) << 8); }
int s16(const std::uint8_t* p) { return static_cast<std::int16_t>(u16(p)); }
int s8(std::uint8_t b) { return static_cast<std::int8_t>(b); }
int unwrap(int raw, int reference, int modulus) {
    const int delta = ((raw - reference + modulus / 2) % modulus + modulus) % modulus;
    return reference + delta - modulus / 2;
}
constexpr int widths[3][4] = {{8,16,32,64},{16,32,32,64},{8,8,16,32}};
constexpr int heights[3][4] = {{8,16,32,64},{8,8,16,32},{16,32,32,64}};
struct Part { int x, y; unsigned a0, a1, a2; };
unsigned identity(unsigned group, unsigned map, unsigned local) {
    return group * 65536 + map * 256 + local;
}
}

void ObjectView::dormant(const ViewMemory& m, const FieldView& field) {
    const auto save = m.u32(0x03005D8C);
    if (!m.bytes(save, 0x159c) || !field.map_count()) return;
    const auto tick = m.u32(0x030022E0);
    if (tick - last_tick_ > 3) poses_.clear(); // rewind, load or skipped scene
    last_tick_ = tick;
    const auto visible_map = [&](unsigned group, unsigned number) -> const FieldMapRegion* {
        for (int i = 0; i < field.map_count(); ++i)
            if (field.map(i).group == group && field.map(i).number == number) return &field.map(i);
        return nullptr;
    };
    for (auto it = poses_.begin(); it != poses_.end();) {
        if (!visible_map(it->first >> 16, (it->first >> 8) & 255)) it = poses_.erase(it);
        else ++it;
    }
    // Preserve positions across software despawns and connected-map handoffs.
    // Active identities (including explicitly hidden actors) always suppress
    // templates, so neither duplicates nor story-hidden actors are introduced.
    std::array<unsigned, 16> active{};
    int active_count = 0;
    for (int i = 0; i < 16; ++i) {
        const auto* o = m.bytes(kObjects + i * 36, 36);
        if (!o || !(o[0] & 1) || (o[2] & 1)) continue;
        const auto key = identity(o[10], o[9], o[8]);
        active[active_count++] = key;
        const auto* region = visible_map(o[10], o[9]);
        const auto* s = o[4] < 64 ? m.bytes(kSprites + o[4] * 68, 68) : nullptr;
        const auto info = m.u32(0x08505620 + o[5] * 4);
        const auto* gfx = m.bytes(info, 36);
        if (!region || !s || !(s[62] & 1) || !gfx || o[5] >= 239) continue;
        const int w = s16(gfx+8), h = s16(gfx+10);
        if (w < 8 || h < 8 || w > 64 || h > 64) continue;
        int x = s16(s+32) + s16(s+36) + s8(s[40]);
        int y = s16(s+34) + s16(s+38) + s8(s[41]);
        if (s[62] & 2) {
            x += static_cast<std::int16_t>(m.u16(kOffsetX));
            y += static_cast<std::int16_t>(m.u16(kOffsetY));
        }
        if (!(o[1] & 0x40)) {
            x = unwrap(u16(s+2) & 511, x, 512);
            y = unwrap(u16(s) & 255, y, 256);
        }
        if (poses_.size() < 256 || poses_.count(key))
            poses_[key] = {x + w/2 + field.origin_x() - region->x*16,
                           y + h + field.origin_y() - region->y*16,
                           s16(o+12) - region->x, s16(o+14) - region->y,
                           o[5], unsigned(o[24] & 15), bool(o[1] & 0x20)};
    }
    const auto hidden_flag = [&](unsigned flag) {
        if (!flag) return false;
        const std::uint8_t* byte = nullptr;
        if (flag < 0x960) byte = m.bytes(save + 0x1270 + flag/8, 1);
        else if (flag >= 0x4000 && flag < 0x4080) byte = m.bytes(0x020375FC + (flag-0x4000)/8, 1);
        return !byte || (*byte & (1 << (flag & 7))) != 0;
    };
    for (int r = 0; r < field.map_count(); ++r) {
        const auto& region = field.map(r);
        const auto events = m.u32(region.header + 4);
        const auto* event_header = m.bytes(events, 8);
        if (!event_header || event_header[0] > 64) continue;
        // Current-map scripts may move/replace templates in the save block.
        const auto templates = r == 0 ? save + 0xC70 : m.u32(events + 4);
        const auto* all = m.bytes(templates, event_header[0] * 24);
        if (!all) continue;
        for (unsigned i = 0; i < event_header[0]; ++i) {
            const auto* t = all + i*24;
            const unsigned key = identity(region.group, region.number, t[0]);
            if (!t[0] || t[0] == 255 || t[2] || hidden_flag(u16(t+20)) ||
                std::find(active.begin(), active.begin()+active_count, key) != active.begin()+active_count) continue;
            // Disguises, berries and invisible/script actors need their own
            // effects/state; do not reveal their ordinary sprite underneath.
            const unsigned movement = t[9];
            if (movement >= 81 || movement == 11 || movement == 12 || movement == 57 ||
                movement == 58 || movement == 63 || movement == 76) continue;
            unsigned graphics = t[1], direction = 1;
            if (graphics >= 240) {
                if (r != 0) continue; // neighboring-map setup vars are not current
                graphics = m.u16(save + 0x139C + (0x10 + graphics - 240)*2) & 255;
            }
            int foot_x = s16(t+4)*16+8, foot_y = s16(t+6)*16+16;
            if (const auto* facing = m.bytes(0x085055CD + movement, 1)) direction = *facing;
            const auto cached = poses_.find(key);
            if (cached != poses_.end() && cached->second.template_x == s16(t+4) &&
                cached->second.template_y == s16(t+6)) {
                if (cached->second.hidden) continue;
                foot_x = cached->second.x; foot_y = cached->second.y;
                graphics = cached->second.graphics; direction = cached->second.direction;
            }
            if (graphics >= 239 || graphics == 69 || direction > 8) continue;
            const auto info = m.u32(0x08505620 + graphics*4);
            const auto* gfx = m.bytes(info, 36);
            if (!gfx) continue;
            const int w = s16(gfx+8), h = s16(gfx+10);
            if (w < 8 || h < 8 || w > 64 || h > 64 || w%8 || h%8) continue;
            const unsigned slot = gfx[12] & 15, tag = u16(gfx+2);
            const unsigned resident_tag = slot < 10 ? m.u16(0x0850BDE8 + slot*2) :
                                          slot == 10 ? m.u16(0x020375B6) : 0xFFFF;
            if (resident_tag != tag) continue;
            const auto* anim_index = m.bytes(0x0850DACC + direction, 1);
            if (!anim_index) continue;
            const unsigned anim = (gfx[12] & 0x40) ? 0 : *anim_index;
            const auto command = m.u32(m.u32(info+24) + anim*4);
            const auto* cmd = m.bytes(command, 4);
            if (!cmd || u16(cmd) >= 64) continue;
            const auto image_entry = m.u32(info+28) + u16(cmd)*8;
            const auto* image = m.bytes(m.u32(image_entry), w*h/2);
            if (!image || m.u16(image_entry+4) < w*h/2) continue;
            const int x = region.x*16 + foot_x - w/2 - field.origin_x();
            const int y = region.y*16 + foot_y - h - field.origin_y();
            if (x+w <= left_ || x >= left_+width_ || y+h <= top_ || y >= top_+height_) continue;
            const auto* priority_data = m.bytes(0x0850E634 + (t[8] & 15), 1);
            if (!priority_data || *priority_data > 3) continue;
            const unsigned priority = *priority_data;
            bool drawn = false;
            for (int py = std::max(0, top_-y); py < h && y+py < top_+height_; ++py)
                for (int px = std::max(0, left_-x); px < w && x+px < left_+width_; ++px) {
                    const int hx = x+px, hy = y+py;
                    if (hx >= 0 && hx < 240 && hy >= 0 && hy < 160) continue;
                    const int tx = (cmd[2] & 64) ? w-1-px : px;
                    const int ty = (cmd[2] & 128) ? h-1-py : py;
                    const unsigned offset = ((ty/8)*(w/8)+tx/8)*32 + (ty&7)*4+(tx&7)/2;
                    const unsigned index = (image[offset] >> ((tx&1)*4)) & 15;
                    if (!index) continue;
                    auto& dest = pixels_[(hy-top_)*width_+hx-left_];
                    if (!(dest.color & 0x8000) && dest.priority <= priority) continue;
                    dest = {static_cast<std::uint16_t>(u16(m.pal+0x200+slot*32+index*2) & 0x7FFF),
                            static_cast<std::uint8_t>(priority), 127};
                    drawn = true;
                }
            dormant_objects_ += drawn;
        }
    }
}

void ObjectView::weather(const ViewMemory& m) {
    // USA Weather.sprites.s2.fogHSprites and current/loaded weather fields,
    // described by the pinned pokeemerald field_weather.h. The guest owns
    // weather, scrolling, animation, palettes and blend coefficients.
    constexpr unsigned weather = 0x02038454;
    const auto* state = m.bytes(weather + 0x6d0, 3);
    if (!state || state[0] != 6 || !state[2]) return;
    const auto sprite = m.u32(weather + 0xa0);
    if (sprite < kSprites || sprite >= kSprites + 64*68 || (sprite-kSprites)%68) return;
    const auto* s = m.bytes(sprite,68);
    if (!s || !(s[62]&1) || (u16(s)&0xff00)!=0x0400 ||
        (u16(s+2)&0xc000)!=0xc000) return;
    const unsigned a2 = u16(s+4), priority = (a2>>10)&3;
    int count=0, order=-1, origin_x=0, origin_y=0;
    unsigned flip=0;
    for (int i=0;i<128;++i) {
        const auto* hw=m.oam+i*8;
        const unsigned a0=u16(hw),a1=u16(hw+2);
        if ((a0&0xff00)!=0x0400 || (a1&0xc000)!=0xc000 || u16(hw+4)!=a2) continue;
        int x=a1&511,y=a0&255;
        if (x>=256) x-=512;
        if (y>=160) y-=256;
        if (!count) { order=i;origin_x=x;origin_y=y;flip=a1&0x3000; }
        else if (((x-origin_x)&63) || ((y-origin_y)&63) || (a1&0x3000)!=flip) return;
        ++count;
    }
    // Only extend the verified, complete 5x4 grid of identical 64px fog
    // sprites. Repeating its original tile pattern adds no synthetic art.
    if (count!=20) return;
    const bool mapping_1d=(u16(m.io)&0x40)!=0;
    for (int y=top_;y<top_+height_;++y) for (int x=left_;x<left_+width_;++x) {
        if (x>=0 && x<240 && y>=0 && y<160) continue;
        auto& dest=pixels_[(y-top_)*width_+x-left_];
        if (!(dest.color&0x8000) && dest.priority*256+dest.order<=priority*256+unsigned(order)) continue;
        int tx=(x-origin_x)&63,ty=(y-origin_y)&63;
        if (flip&0x1000) tx=63-tx;
        if (flip&0x2000) ty=63-ty;
        const unsigned tile=(a2&1023)+(ty/8)*(mapping_1d?8:32)+tx/8;
        const unsigned offset=0x10000+tile*32+(ty&7)*4+(tx&7)/2;
        if (offset>=0x18000) return;
        const unsigned index=(m.vram[offset]>>((tx&1)*4))&15;
        if (!index) continue;
        dest={static_cast<std::uint16_t>(u16(m.pal+0x200+(a2>>12)*32+index*2)&0x7fff),
              static_cast<std::uint8_t>(priority),static_cast<std::uint8_t>(order),true};
    }
}

bool ObjectView::prepare(const ViewMemory& m, const FieldView& field, int width, int height) {
    ready_ = false;
    objects_ = verified_parts_ = 0;
    dormant_objects_ = 0;
    mismatch_.fill(0);
    width_ = std::clamp(width, 240, kMaxViewWidth);
    left_ = -(width_ - 240) / 2;
    height_ = std::clamp(height, 160, kMaxViewHeight);
    top_ = -(height_ - 160) / 2;
    if (field.status() != ViewStatus::Ready || !m.oam || !m.pal || !m.vram || !m.io)
    {
        if (field.status() != ViewStatus::Unverified) poses_.clear();
        return false;
    }
    pixels_.assign(width_ * height_, {});
    const int offset_x = static_cast<std::int16_t>(m.u16(kOffsetX));
    const int offset_y = static_cast<std::int16_t>(m.u16(kOffsetY));
    const bool mapping_1d = (u16(m.io) & 0x40) != 0;
    // Decode every visible object as well, so the hardware OAM continuously
    // checks our Sprite/Subsprite layout before we use it outside the screen.
    for (int id = 0; id < 16; ++id) {
        const auto* o = m.bytes(kObjects + id * 36, 36);
        if (!o || !(o[0] & 1) || (o[1] & 0x20)) continue; // inactive/script-hidden
        if (o[4] >= 64) return false;
        const auto address = kSprites + o[4] * 68;
        const auto* s = m.bytes(address, 68);
        if (!s || !(s[62] & 1)) return false;
        if ((s[62] & 4) && !(o[1] & 0x40)) continue; // other intentional hiding
        const unsigned a0 = u16(s), a1 = u16(s+2), a2 = u16(s+4);
        // Affine, bitmap, mosaic, and OBJ-window field effects are not NPCs.
        if ((a0 & 0x3F00) || (a0 >> 14) >= 3) continue;
        int x = s16(s+32) + s16(s+36) + s8(s[40]);
        int y = s16(s+34) + s16(s+38) + s8(s[41]);
        if (s[62] & 2) { x += offset_x; y += offset_y; }
        // Sprite simulation can already be one step ahead of the OAM that
        // VBlank published. Visible sprites retain those published coordinates
        // in Sprite.oam; use RAM positions only to unwrap their 9/8-bit fields.
        if (!(o[1] & 0x40)) {
            x = unwrap(a1 & 511, x, 512);
            y = unwrap(a0 & 255, y, 256);
        }
        const bool hflip = (a1 & 0x1000) != 0, vflip = (a1 & 0x2000) != 0;
        std::array<Part, 16> parts{};
        int count = 1;
        parts[0] = {x, y, a0, a1, a2};
        const auto tables = m.u32(address + 24);
        const int mode = s[66] >> 6;
        if (tables && mode) {
            const auto* table = m.bytes(tables + (s[66] & 63) * 8, 8);
            if (!table || table[0] > parts.size()) return false;
            count = table[0];
            const auto* data = m.bytes(m.u32(tables + (s[66] & 63) * 8 + 4), count * 4);
            if (!data && count) return false;
            for (int n = 0; n < count; ++n) {
                const auto* p = data + n * 4;
                const unsigned bits = u16(p+2), shape = bits & 3, size = (bits >> 2) & 3;
                if (shape >= 3) return false;
                const int dx = hflip ? -s8(p[0]) - widths[shape][size] : s8(p[0]);
                const int dy = vflip ? -s8(p[1]) - heights[shape][size] : s8(p[1]);
                parts[n] = {x - s8(s[40]) + dx, y - s8(s[41]) + dy,
                    (a0 & 0x3FFF) | (shape << 14), (a1 & 0x3FFF) | (size << 14),
                    (a2 & 0xFC00) | (((a2 & 1023) + ((bits >> 4) & 1023)) & 1023)};
                if (mode != 2) parts[n].a2 = (parts[n].a2 & ~0xC00u) | ((bits >> 14) << 10);
            }
        }
        ++objects_;
        for (int n = 0; n < count; ++n) {
            auto& p = parts[n];
            p.a0 = (p.a0 & ~255u) | (p.y & 255);
            p.a1 = (p.a1 & ~511u) | (p.x & 511);
            int order = 127;
            bool found = false;
            for (int idx = 0; idx < 128; ++idx) {
                const auto* hw = m.oam + idx * 8;
                const unsigned hw0 = u16(hw), hw1 = u16(hw+2), hw2 = u16(hw+4);
                const int hx = unwrap(hw1 & 511, p.x, 512), hy = unwrap(hw0 & 255, p.y, 256);
                // OAM publication can trail Sprite.oam too (the game builds
                // next frame's list during visible time). Bind by allocated
                // tile/palette, shape and size, allowing at most one 8px step.
                // Published coordinates and flips then remain authoritative.
                if ((hw0 & 0xFF00) == (p.a0 & 0xFF00) &&
                    (hw1 & 0xC000) == (p.a1 & 0xC000) && hw2 == p.a2 &&
                    std::abs(hx - p.x) <= 8 && std::abs(hy - p.y) <= 8) {
                    if (found) return false; // ambiguous identity must not draw
                    order = idx; found = true;
                }
            }
            const int part_width = widths[p.a0 >> 14][p.a1 >> 14];
            const int part_height = heights[p.a0 >> 14][p.a1 >> 14];
            const bool touches_native = p.x < 240 && p.x + part_width > 0 &&
                                        p.y < 160 && p.y + part_height > 0;
            // Emerald's offScreen flag includes a 16px halo. The hardware
            // OAM builder can already omit a sprite in that halo; it must not
            // invalidate every extended NPC while none of its pixels are native.
            if (!(o[1] & 0x40) && touches_native) {
                if (!found) {
                    mismatch_ = {unsigned(id),p.a0,p.a1,p.a2,0,0,0};
                    for (int idx = 0; idx < 128; ++idx) {
                        const auto* hw = m.oam + idx * 8;
                        if (u16(hw+4) == p.a2) {
                            mismatch_[4]=u16(hw); mismatch_[5]=u16(hw+2); mismatch_[6]=u16(hw+4); break;
                        }
                    }
                    return false;
                }
            }
            if (found) {
                ++verified_parts_;
                const auto* hw = m.oam + order * 8;
                p.a0 = u16(hw); p.a1 = u16(hw+2); p.a2 = u16(hw+4);
                p.x = unwrap(p.a1 & 511, p.x, 512);
                p.y = unwrap(p.a0 & 255, p.y, 256);
            }
            const int shape = p.a0 >> 14, size = p.a1 >> 14;
            const int w = widths[shape][size], h = heights[shape][size];
            const bool part_hflip = (p.a1 & 0x1000) != 0, part_vflip = (p.a1 & 0x2000) != 0;
            const int priority = (p.a2 >> 10) & 3, palette = p.a2 >> 12;
            for (int py = std::max(0, top_ - p.y); py < h && p.y + py < top_ + height_; ++py) {
                const int ty = part_vflip ? h - 1 - py : py;
                for (int px = 0; px < w; ++px) {
                    const int hx = p.x + px, out_x = hx - left_;
                    if (out_x < 0 || out_x >= width_ ||
                        (hx >= 0 && hx < 240 && p.y + py >= 0 && p.y + py < 160)) continue;
                    const int tx = part_hflip ? w - 1 - px : px;
                    const unsigned tile = (p.a2 & 1023) + (ty / 8) * (mapping_1d ? w / 8 : 32) + tx / 8;
                    const unsigned offset = 0x10000 + tile * 32 + (ty & 7) * 4 + (tx & 7) / 2;
                    if (offset >= 0x18000) return false;
                    const unsigned index = (m.vram[offset] >> ((tx & 1) * 4)) & 15;
                    if (!index) continue;
                    auto& dest = pixels_[(p.y + py - top_) * width_ + out_x];
                    if (!(dest.color & 0x8000) && dest.priority * 256 + dest.order <= priority * 256 + order) continue;
                    dest = {static_cast<std::uint16_t>(u16(m.pal + 0x200 + palette * 32 + index * 2) & 0x7FFF),
                            static_cast<std::uint8_t>(priority), static_cast<std::uint8_t>(order)};
                }
            }
        }
    }
    dormant(m, field);
    weather(m);
    ready_ = true;
    return true;
}

const gba::WsMarginObjPixel* ObjectView::row(int y, int* left, int* width) const {
    if (!ready_ || y < top_ || y >= top_ + height_ || !left || !width) return nullptr;
    *left = left_; *width = width_;
    return pixels_.data() + (y - top_) * width_;
}
} // namespace emerald
