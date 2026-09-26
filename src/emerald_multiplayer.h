#pragma once
#include "multiplayer_session.h"
#include "emerald_ram_dispatch.h"

namespace emerald {
inline void setup_link_instance(gbarecomp::GbaInstance& instance) {
    instance.execution.ram_dispatch=&ram_dispatch;
    // Copied flash callbacks are Thumb; generated ARM IntrSIO32 can batch.
    instance.execution.ram_dispatch_filter=[](std::uint32_t,int thumb) { return thumb; };
}
}
