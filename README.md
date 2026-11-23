# PicoCalc File Manager

A Midnight Commander–style file manager for the **ClockworkPi PicoCalc** fitted with a
**Raspberry Pi Pico 2 W (RP2350)**. It is built as a standard UF2 and runs from
[UF2Loader](https://github.com/pelrun/uf2loader).

## Features

- Two file panels. By default the inactive panel shrinks to a single line, leaving the
  whole screen for the active one. Ctrl+L switches to a split view with the panels stacked.
- Copy, move, rename and delete files and folders (recursively), and make new folders.
- Tag several entries to work on them at once. Copy and move ask what to do when a file already exists.
- Text editor: create, view and edit text files up to 192 KB. Supports find, go to line and CRLF line endings.
- Hex editor/viewer for files of any size. Edit in hex or ASCII, append bytes, undo,
  search for text or hex bytes, and jump to an offset.
- Online updates: checks GitHub releases over Wi‑Fi and saves the newest build to
  `/pico2-apps/picocalc_file_manager.uf2`.
- Long operations show a `/ - \ |` spinner and can be cancelled with Esc.
- Supports FAT32 and exFAT, with UTF‑8 long file names.

## Installation

1. Install [UF2Loader](https://github.com/pelrun/uf2loader) on the PicoCalc.
2. Download `picocalc_file_manager.uf2` from the
   [latest release](https://github.com/kicomoco/UF2PicoCalcFileManager/releases/latest).
3. Copy it to `/pico2-apps/` on the SD card.
4. Select it in the UF2Loader menu. To get back to the loader, hold **Up**, **F1** or **F5** while powering on.

## Keys

| Key | Action |
| --- | --- |
| Up / Down, Shift+Up / Shift+Down | Move the cursor / page up and down |
| Shift+Tab / Shift+Del | Jump to the first / last entry |
| Enter | Open a folder, or view a file (binary files open in the hex viewer) |
| Right / Left, Backspace | Enter a folder / go to the parent folder |
| Tab | Switch panels |
| Space | Tag or untag (`+` tags all, `-` untags all, `*` inverts) |
| a–z, 0–9 | Jump to the next name starting with that character |
| F1 | Help |
| F2 | Rename |
| F3 / F4 | View / edit as text |
| F5 / F6 | Copy / move (the other panel's path is pre-filled) |
| F7 | Make a folder |
| F8 or Del | Delete |
| F9 | Menu (sorting, hidden files, Wi‑Fi, updates, restart…) |
| F10 | Hex editor |
| Ctrl+N | New file |
| Ctrl+L | Toggle the split view |
| Ctrl+U | Swap panels |
| Ctrl+O | Point the other panel at this folder |
| Ctrl+R | Refresh / remount the SD card |

The PicoCalc keyboard turns **Shift+F1…F5** into **F6…F10**.

For copy and move, an existing folder as the target means the items go into that folder.
Any other target is used as the new name. Relative paths are resolved from the current folder.

## Wi‑Fi and updates

Open **F9 → Wi‑Fi settings** to scan for a network or type one in, then enter its password.
Then use **F9 → Check for updates**. The app downloads the latest release over HTTPS, with the
certificate checked against bundled root CAs. The new file is checked to be a valid UF2 and then
replaces `/pico2-apps/picocalc_file_manager.uf2`. After it restarts, choose it in UF2Loader.

Settings are saved to `/picocalc_file_manager.cfg`, including the Wi‑Fi password in plain text.

## Building

You need the [Pico SDK](https://github.com/raspberrypi/pico-sdk) 2.2.0 (with the
`cyw43-driver`, `lwip` and `mbedtls` submodules) and an `arm-none-eabi` GCC toolchain.

```sh
cmake -B build -DPICO_SDK_PATH=/path/to/pico-sdk -DFM_VERSION=1.2.3
cmake --build build -j
# -> build/picocalc_file_manager.uf2
```

`tools/gen_ca_certs.py` regenerates `src/ca_certs.h` if GitHub's certificate authorities change.

## Releases

Each push to `main` runs [`.github/workflows/release.yml`](.github/workflows/release.yml). It
bumps the patch version from the latest `vX.Y.Z` tag, builds the UF2 with that version built in,
tags the commit and publishes a GitHub release. Put `#minor` or `#major` in the commit message to
bump those parts instead.

## Licence notes

This project uses [FatFs](http://elm-chan.org/fsw/ff/) (© ChaN, BSD-style), lwIP, Mbed TLS,
and the public-domain `font8x8` font.
