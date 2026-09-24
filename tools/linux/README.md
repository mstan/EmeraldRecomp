# Pokémon Emerald - GBA static recompilation (Linux x86_64 AppImage) v@VERSION@

An optimized native port: the game's ARM7TDMI code is statically recompiled to
native code with the [gbarecomp](https://github.com/mstan/gbarecomp) framework;
the real GBA BIOS is recompiled and executed.

## You supply the ROM and BIOS

Nothing copyrighted ships in this AppImage. On first run the launcher asks for:

- your legally-obtained **Pokémon Emerald (USA)** ROM (`.gba`), SHA-1
  `f3ae088181bf583e55daf962a92bb46f4f1d07b7`
- a **GBA BIOS** dump (`gba_bios.bin`, 16 KiB).

The file picker uses zenity or kdialog (install one if the picker does not
appear), or pass them once on the command line:

    ./EmeraldRecomp-linux-x86_64-v@VERSION@.AppImage --rom /path/emerald.gba --bios /path/gba_bios.bin

## Running

    chmod +x EmeraldRecomp-linux-x86_64-v@VERSION@.AppImage
    ./EmeraldRecomp-linux-x86_64-v@VERSION@.AppImage

Requires FUSE 2 (`libfuse2`) like any AppImage; without it, run with
`--appimage-extract-and-run`. Your settings, saves and mod choices live in
`~/.local/share/EmeraldRecomp` (override with `EMERALDRECOMP_HOME`).

## Widescreen mod

Open **Mods**, enable **Overworld Widescreen (Experimental)**, choose **Fit to
window**, **16:9**, **21:9** or **32:9** and apply. The overworld then fills
landscape and portrait windows with extra scenery and live NPCs; battles keep
their native 3:2 view.
