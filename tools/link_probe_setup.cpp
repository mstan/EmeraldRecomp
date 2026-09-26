#include "multiplayer_session.h"
#include "../src/emerald_ram_dispatch.h"

void gba_link_probe_setup(gbarecomp::GbaInstance& instance) {
    instance.execution.ram_dispatch=&emerald::ram_dispatch;
    // All copied flash callbacks handled by this game are Thumb. ARM RAM code
    // (including IntrSIO32) uses the ordinary generated dispatch table.
    instance.execution.ram_dispatch_filter=[](std::uint32_t,int thumb) { return thumb; };
}
