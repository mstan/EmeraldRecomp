#include "multiplayer_launch.h"
#include "emerald_extended_view.h"
#include "emerald_multiplayer.h"

gbarecomp::GbaNetplayLaunch gba_view_probe_game() {
    gbarecomp::GbaNetplayLaunch game;
    game.program_id="emerald-usa-native-probe";
    game.setup_instance=emerald::setup_link_instance;
    game.view_policy={true,true,emerald::install_netplay_view,
        emerald::update_extended_view,emerald::reset_extended_view};
    return game;
}
