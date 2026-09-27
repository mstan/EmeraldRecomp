## Two-player netplay (cable since v0.1.0; wireless in v0.1.1)

Open the launcher's Netplay tab to host or join a two-player session. Both
players need this release, the same USA game ROM and their own GBA BIOS.
Choose your save file before starting. The host chooses Link Cable or Wireless
Adapter in Lobby Settings; both peers use that choice. Emerald saves must
already have access to the corresponding multiplayer area. Use the upstairs
Pokémon Center cable club for cable sessions or the Union Room for wireless.

Both input-delay and rollback modes are available over recomp-net Internet/LAN
sessions. Cable multiplayer has been play-tested. Wireless Union Room entry,
mutual discovery and contact have been validated, including deterministic
replay under simulated latency; full wireless trades/battles and all recovery
paths remain experimental. Single-Pak multiboot and cross-version Pokémon
linking are not supported yet.

The Netplay **Your display** setting offers native, 16:9, 21:9, 32:9 and adaptive widescreen.
Each player can choose a different view or window size. These views preserve
the synchronized game state. Gameplay-changing single-player mods are separate
from the netplay view setting.

During netplay, Shift+F1 or closing the game once saves and leaves the entire
paired session. Closing again aborts. Session checkpoints preserve both GBAs;
they do not replace either player's original cartridge save. Checkpoint resume
currently uses the command-line option --netplay-resume.
