# EmeraldRecomp — Pokémon Emerald, Recompiled

> _This recompilation is a **byproduct of developing
> [gbarecomp](https://github.com/mstan/gbarecomp)** — the games are the proving ground, the framework is the goal.
> **These are in-development previews, not finished ports — expect rough
> edges**, and depth will keep landing over months, not days. My time for any
> one title is limited, so I ask for your patience. Contributions are welcome —
> testing, issues, and PRs to the game or framework all help and will
> accelerate this game's polish. More on the why at:
> [Recomp + AI: 5 Months Later »](https://1379.tech/recomp-ai-5-months-later/)_

Static recompilation of **Pokémon Emerald** (Game Boy Advance) to native PC, built
on the [`gbarecomp`](https://github.com/mstan/gbarecomp) framework.

Its Gen3 siblings live in
[`FireRedLeafGreenRecomp`](https://github.com/mstan/FireRedLeafGreenRecomp) (FireRed + LeafGreen) and
[`RubySapphireRecomp`](https://github.com/mstan/RubySapphireRecomp) (Ruby + Sapphire).

> ### Status — playable bring-up (v0.0.1), and self-improving
>
> This is a **static-recompilation base + runner**, not a finished port. Emerald
> **boots through the BIOS intro to the title screen and into gameplay**. It is
> **early** — not every code path is statically recompiled yet, and content has
> not been exhaustively tested. (Emerald's RTC and its larger battle/contest engine
> are the notable deltas from the other Gen3 games.)
>
> **It gets better the more you play.** Any code path the static recompiler hasn't
> covered runs through a built-in **interpreter the first time it's hit**, then is
> **JIT-compiled to native** (in-process, no toolchain needed) and **remembered on
> disk** — so the next launch runs it natively from the start. Interpreted once,
> native ever after; coverage grows toward fully-native as the game is played. See
> [How it self-improves](#how-it-self-improves).

---

## Screenshots

| Pokémon Emerald — title screen | Pokémon Emerald — a wild encounter |
|---|---|
| ![Pokémon Emerald — title screen, native recompiled build](docs/screenshots/emerald-title.png) | ![Pokémon Emerald — a wild encounter, running natively](docs/screenshots/emerald-gameplay.png) |

*Native recompiled builds (no emulator), captured running the original ROM.*

---

## What "static recompilation" means here

The ROM's **ARM7TDMI machine code is statically translated to native C** — every
function the game runs becomes a real generated C function. Unlike most recomp
projects, **the GBA BIOS is recompiled and executed too** (not HLE'd or stubbed),
so the boot sequence and interrupt/SWI handlers run as real recompiled code. The
rest of the console — the PPU (graphics), APU + M4A sound engine, DMA, timers, the
cartridge flash save chip + RTC, and hardware I/O — is modeled by the `gbarecomp`
runtime.

Only **symbol metadata** (function names, addresses, sizes) from the
[`pret/pokeemerald`](https://github.com/pret/pokeemerald) decompilation enters this
repo — never its C source, build output, or toolchain. **The ROM is never
redistributed**; you supply your own legally-dumped copy.

## ROM

| Target          | Game            | ROM (USA) | SHA-1                                      | Debug port |
|-----------------|-----------------|-----------|-------------------------------------------|------------|
| `EmeraldRecomp` | Pokémon Emerald | USA       | `f3ae088181bf583e55daf962a92bb46f4f1d07b7` | 19892      |

The runtime **refuses to launch on an unrecognized ROM** — the SHA-1 must match.

## Quick start

1. Download the latest `EmeraldRecomp-windows-x64` zip from
   [Releases](../../releases) and extract it (or build from source — see below).
2. Run `EmeraldRecomp`.
3. Supply your own **legally-obtained** Pokémon Emerald (USA) ROM when prompted.
   The path is cached next to the exe for future launches.
4. Play. Early on you may briefly see the interpreter warm up new code paths; once
   warmed (and cached), they run native.

## Multiplayer: Link Cable and Wireless Adapter

EmeraldRecomp supports **two-player Emerald ↔ Emerald netplay** with a virtual
**Link Cable** or **Wireless Adapter**. Each peer simulates both GBAs and the
local link hardware; `recomp-net` exchanges controller inputs using delay-sync
or rollback. LAN / Direct IP and Internet lobbies use the same simulation.

1. Use matching builds and the supported Emerald USA ROM on both machines.
2. Open **Netplay**, create or join a lobby, and use a separate trainer save
   for each player.
3. The host selects **Lobby Settings → Connection type**: **Link Cable**
   (default) or **Wireless Adapter**. All players inherit the host's choice.
4. Start the session, then use the corresponding upstairs Pokémon Center
   desk. Wireless Adapter enables the **Union Room**; the original game's
   progression and party requirements still apply.

Cable multiplayer has been play-tested. Wireless currently has validated Union
Room entry, mutual discovery and contact, deterministic replay, and input
netplay under simulated latency/jitter. Full wireless trades/battles and all
RFU recovery behavior have not yet been qualified. See the engine's
[wireless validation and limits](https://github.com/mstan/gbarecomp/blob/main/docs/WIRELESS.md).

Wireless is available in current source builds; the previously published
**v0.1.0** binaries contain cable support. Merging source does not replace those
release downloads. FireRed/LeafGreen/Ruby/Sapphire cross-version linking,
larger lobbies and Single-Pak multiboot are future work.

## Controls

| GBA button | Keyboard      |
|------------|---------------|
| D-Pad      | Arrow keys    |
| A          | Z             |
| B          | X             |
| Start      | Enter         |
| Select     | Backspace     |

Save states: **Shift+F1–F9** save to a slot, **F1–F9** load it.

## How it self-improves

`gbarecomp`'s coverage is honest: a path that wasn't statically recompiled is
**bridged through the interpreter** the first time, *loudly*, then healed:

- **First hit:** the interpreter runs the missed function (correct, just not
  native) and the runtime records it.
- **Heal:** the function is **JIT-compiled to native in-process** via a
  toolchain-less backend (sljit) — no compiler required on your machine.
- **Persist:** the healed path is written to a per-ROM cache
  (`recomp_cache/<rom-sha1>/`), so **the next launch re-JITs it up front** and it
  runs native from the start.

The result is a game that converges toward fully-native execution the more it's
played, and **stays** improved across launches. A handful of instruction patterns
the JIT can't lower yet stay on the interpreter (precision over recall); those are
emitter gaps that close over time. Self-improvement is on by default; set
`GBARECOMP_SELFHEAL_RECOMPILE=0` for a pure-interpreter run.

## Building from source

An opt-in [overworld widescreen experiment](docs/WIDESCREEN_EXPERIMENT.md)
adds Fit to window, 16:9, 21:9 and 32:9 choices through the Mods catalog. This
version expands scenery and live NPC visibility. Starting with v0.0.6, Fit also
fills portrait windows, overworld menus anchor to the viewport edges, and door
animations retain expanded scenery. Distant object spawning and
field effects retain the game's original limits; unsupported scenes use the
original centered view. Mod 0.2.0 is bundled with v0.0.6, disabled by default.

**Prerequisites (Windows):** [MSYS2](https://www.msys2.org/) with the mingw64
toolchain (`gcc`/`g++`), CMake 3.16+, Ninja, and SDL2 (mingw64 package). Builds
are invoked from PowerShell with the mingw64 toolchain on `PATH`.

**1. Clone this repo next to `gbarecomp`** (the game repo builds against the
sibling engine checkout on `main`):

```
git clone https://github.com/mstan/gbarecomp.git
git clone https://github.com/mstan/EmeraldRecomp.git
cd EmeraldRecomp
```

**2. Supply your ROM** at `variants/emerald/roms/emerald_usa.gba` (SHA-1 above).
ROMs are gitignored and never committed.

**3. Recompile + build.** The committed `variants/emerald/symbols/*.toml` are the
importer output, so you can regenerate the C and build directly:

```
# from PowerShell, mingw64 on PATH
gba_recompile --rom variants/emerald/roms/emerald_usa.gba \
              --config variants/emerald/symbols/emerald_usa.toml \
              --out variants/emerald/generated
cmake -S . -B build -G Ninja
cmake --build build --target EmeraldRecomp
```

(`gba_recompile` is built from the `gbarecomp` checkout; see that repo's README.)
The recompiled translation unit is large — expect a multi-minute compile.

## License

PolyForm Noncommercial 1.0.0 — see [`LICENSE`](LICENSE). Third-party
components retain their own licenses.

## Legal

This project contains **no copyrighted ROM data, no Nintendo BIOS, and no decomp
source** — only original recompiler/runtime code and symbol metadata. **You must
supply your own legally-dumped ROM** (and BIOS, where the runtime requires one).
Pokémon and Emerald are trademarks of Nintendo / Game Freak / The Pokémon Company;
this project is an unaffiliated, non-commercial preservation and research effort.

---

<p align="center">
  <sub><b>R.A.I.D. — Retro AI Development</b> · a Discord for AI-assisted retro reverse-engineering, decomp &amp; recomp</sub>
</p>

<p align="center">
  <a href="https://discord.gg/Ad9BwSzctP"><img src=".github/raid-discord.png" alt="Join the Retro AI Development (R.A.I.D.) Discord" width="200"></a>
</p>
