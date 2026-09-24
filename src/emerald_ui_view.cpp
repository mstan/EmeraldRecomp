#include "emerald_ui_view.h"
#include <algorithm>

namespace emerald {
namespace {
unsigned u16(const std::uint8_t* p) { return p[0] | (unsigned(p[1]) << 8); }
int signed_scroll(unsigned n) { return int((n + 256) & 511) - 256; }
}
void UiView::prepare(const ViewMemory& m, const FieldView& field, int width, int height) {
    count_ = 0;
    active_ = field.status() == ViewStatus::Ready && m.io && m.vram;
    if (!active_) return;
    const unsigned cnt = u16(m.io + 8);
    if (cnt & 0xC080) { active_ = false; return; }
    const int left = (width - 240) / 2, right = width - 240 - left;
    const int top = (height - 160) / 2, bottom = height - 160 - top;
    const int scroll_x = signed_scroll(u16(m.io + 0x10));
    const int scroll_y = signed_scroll(u16(m.io + 0x12));
    const unsigned screen = ((cnt >> 8) & 31) * 0x800;
    for (int i = 0; i < 32; ++i) {
        const auto* w = m.bytes(0x02020004 + i * 12, 12);
        if (!w || w[0] != 0 || !w[3] || !w[4] || w[1] + w[3] > 30 ||
            w[2] + w[4] > 20 || !m.u32(0x02020004 + i * 12 + 8)) continue;
        // Allocated but hidden windows share BG0 with active ones. Require
        // a published interior tile before assigning an anchor rectangle.
        const auto base = u16(w + 6);
        bool visible = false;
        for (int ty = 0; ty < w[4] && !visible; ++ty) for (int tx = 0; tx < w[3]; ++tx) {
            const auto entry = u16(m.vram + screen + ((w[2] + ty) * 32 + w[1] + tx) * 2);
            if ((entry & 1023) == base + ty * w[3] + tx && (entry >> 12) == w[5]) {
                visible = true; break;
            }
        }
        if (!visible) continue;
        const bool at_left = w[1] <= 2, at_right = w[1] + w[3] >= 28;
        const bool at_top = w[2] <= 2, at_bottom = w[2] + w[4] >= 19;
        const int dx = at_left == at_right ? 0 : at_right ? right : -left;
        const int dy = at_top ? -top : at_bottom ? bottom : 0;
        if (!dx && !dy) continue;
        // Standard frames occupy one tile column left of the window; dialogue
        // frames (menu.c WindowFunc_DrawDialogueFrame and its custom-tile
        // variants) occupy two (tilemapLeft - 2 and - 1). Their outer column
        // repeats one vertical-edge tile down every interior row, while window
        // interiors never repeat a tile, so the repeat identifies the frame.
        int frame_left = 1;
        if (w[1] >= 2) {
            const auto edge = u16(m.vram + screen + (w[2] * 32 + w[1] - 2) * 2);
            bool repeated = (edge & 1023) != 0;
            for (int ty = 1; ty < w[4] && repeated; ++ty)
                repeated = u16(m.vram + screen + ((w[2] + ty) * 32 + w[1] - 2) * 2) == edge;
            if (repeated) frame_left = 2;
        }
        const int x = std::max(0, (int(w[1]) - frame_left) * 8);
        const int y = std::max(0, (int(w[2]) - 1) * 8);
        windows_[count_++] = {x - scroll_x, y - scroll_y,
            std::min(240, (w[1] + w[3] + 1) * 8) - x,
            std::min(160, (w[2] + w[4] + 1) * 8) - y, dx, dy};
    }
}

int UiView::sample(int bg, int x, int y, int* sx, int* sy) const {
    if (!active_ || bg != 0) return 0;
    const auto inside = [](const Rect& r, int px, int py) {
        return px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h;
    };
    // Destination rectangles take precedence when movement overlaps source.
    for (int i = count_ - 1; i >= 0; --i) {
        const auto& r = windows_[i];
        if (inside(r, x - r.dx, y - r.dy)) { *sx = x - r.dx; *sy = y - r.dy; return 1; }
    }
    for (int i = 0; i < count_; ++i) if (inside(windows_[i], x, y)) return -1;
    return x < 0 || x >= 240 || y < 0 || y >= 160 ? -1 : 0;
}
}
