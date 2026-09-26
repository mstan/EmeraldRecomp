#include "multiplayer_session.h"
#include "../src/emerald_ram_dispatch.h"

void gba_link_probe_setup(gbarecomp::GbaInstance& instance) {
    instance.execution.ram_dispatch=&emerald::ram_dispatch;
}
