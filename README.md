# DuckTales: The Quest for Gold (DOS) – native port

A native port of the 1990 PC/DOS game *DuckTales: The Quest for Gold* for
Windows, Linux and macOS. The original program is **statically recompiled**
from x86-16 machine code to portable C. It runs on a small SDL2 runtime that
provides the PC hardware the game expects: EGA/CGA/Tandy video, the PC
speaker and Tandy sound chips, the keyboard, the joystick and DOS.

As an option, the port can also use the **Amiga version's** graphics, music,
sound effects and speech, taken from your own copy of the Amiga game.

> **This repository contains no part of the game.** You need your own copy of
> the original DOS game. The game is converted and compiled on your own
> computer from that copy. Do not share the game files or anything built from
> them (see [Legal](#legal)).

## Features

- Runs the original game logic unchanged. It is recompiled code, not an
  emulator and not a remake.
- Graphics: **EGA**, **CGA**, **Tandy** (as the original supports) or
  **Amiga 32 colours**. The Amiga mode shows the Amiga version's pictures
  and palettes, including its colour cycling and per-minigame status bars.
- Sound: **Tandy 3-voice**, **PC speaker**, **Amiga** or off.
  - The Amiga option runs the Amiga version's own sound engine with its own
    instruments: music, effects and speech, read from your Amiga game. No
    sound files are converted.
  - The Amiga version's speech can be switched on or off, and it can also
    be heard with the Tandy and PC speaker sound.
- Game speed, frame rate limit and music tempo can be set separately, also
  during play.
- Game controllers (XInput etc.) act as the PC joystick. A keyboard-as-joystick
  mode is also available.
- The code wheel check (copy protection) can be switched off.
- The jungle minigame can take the nephews in the Amiga order (green,
  blue, red) instead of the DOS order (red, blue, green).
- Settings program: game folder selection, import of the Amiga disk images
  (`.adf`/`.ipf`), a one-click build, and adding the Amiga sound later.
- Windows (x64; ARM64 through emulation), Linux (x86_64, aarch64) and macOS
  (Intel, Apple Silicon).

## What you need

- The DOS game: `DUCKTALE.EXE`, `IT.DRV` and the `*.ARC` files.
- Optional, for the Amiga graphics and sound options: the Amiga version,
  as its two disk images (`.adf`, or `.ipf`), which the settings program
  imports, or as a folder holding the `DT1` and `DT2` disk contents.
- Linux/macOS: SDL2 and Python 3.8 or newer. On Windows everything is
  downloaded by the installer.

## Getting started (release package)

The release package (`dist/DuckTalesDOS`) contains the settings program,
the installer and the sources, but no game material. Make it from the
repository with `python make_dist.py` (Windows, Linux or macOS; no game
files needed), or use a published release.

**Windows**

1. Run `settings.exe` and drop your game folder onto its window, or copy the
   game files into the `files` folder. "Game folder" turns green when
   `DUCKTALE.EXE` and `IT.DRV` are found.
2. Optional: set "Amiga game folder" to your Amiga copy, or import its two
   disk images: drop the `.adf` files (or `.ipf`) on the settings window,
   or choose **Import Amiga disk image**. They are extracted into the Amiga
   game folder. The Amiga graphics only need the folder. The Amiga sound is
   built into the game: set the folder before step 3, or later choose
   **Add Amiga sound**.
3. Choose **Build game from game files**. A console window runs the
   installer.
4. Choose **Save and start game**.

**Linux / macOS**

```sh
# Debian/Ubuntu: sudo apt install libsdl2-2.0-0
# Fedora:        sudo dnf install SDL2
# macOS:         brew install sdl2
./install.sh            # or: ./install.sh /path/to/game
./settings              # then "Save and start game"
```

The first run downloads the build tools into `toolchain/` and verifies their
SHA-256 checksums: Zig 0.16.0 (C compiler), Capstone 5.0.7 and, on Windows
only, an embeddable Python 3.12. It then recompiles your copy of the game and
compiles it. This takes a few minutes. Run it again if you change the game
folders.

## Controls

| Key / button | Action |
|---|---|
| Arrow keys, Space | as in the game's manual (office: hold a direction and press Space) |
| PgUp / PgDn (keypad + / −) | game speed |
| Ctrl + PgUp / PgDn | music speed |
| Alt + PgUp / PgDn | frame rate limit (steps of 5, 1 with Shift) |
| F11 / F12 | fullscreen / screenshot (`screenshot_001.bmp`, `_002`, …) |
| Controller stick or d-pad, A/X, B/Y | joystick, fire buttons 1 and 2 (in menus one press per button press) |
| Controller LB / RB, Back + LB / RB | game speed, music speed |

Changes made during play are not saved. Use the settings program
(`ducktales_dos.ini`) for that. A held key repeats after 0.5 s, about
11 times a second, whatever the frame rate.

## How it works

- **Unpacking.** `DUCKTALE.EXE` is EXEPACK-compressed (`tools/unexepack.py`).
  It carries 10 Microsoft LINK overlays that are called through `INT 3Fh`.
  `IT.DRV` holds the compressed video and sound driver modules.
- **Recompilation.** `tools/analyze.py` finds the code: functions, jump
  tables, overlay entries and self-modifying driver code.
  `tools/recomp.py` turns every instruction into C on a global CPU state and
  a 1 MB memory. Hooks at chosen addresses connect the game to the runtime,
  for example for settings, the copy protection, the joystick and the Amiga
  options.
- **Runtime** (`runtime/`). Implements:
  - DOS and BIOS services and the overlay loader;
  - video (EGA planes, CGA, Tandy, Hercules);
  - the PIT timer, the PC speaker and the Tandy SN76489;
  - keyboard, joystick and pacing.
- **Amiga graphics** (`runtime/amiga86.c`). Every call to the EGA driver is
  repeated on 8-bit surfaces. The Amiga version's picture is decoded for each
  picture the game loads. EGA colours drawn by the game are mapped to the
  scene's Amiga palette.
- **Amiga sound** (`runtime/amigasnd.c`, `tools/amiga/sndgen.py`). The Amiga
  program's root code, which holds the sound engine and the instrument
  loaders, is recompiled from your Amiga copy with the companion Amiga
  recompiler. It runs in its own small Amiga memory with a Paula emulation
  and a few exec/dos calls. The DOS game's sound requests are passed to it;
  the sound numbers are the same in both versions. The voices, which the
  DOS game never asks for, are started from what the DOS game shows (its
  texts, pictures and a few events), and the game waits for each as the
  Amiga version does.

[NOTES.md](NOTES.md) has the detailed findings: file formats, driver entry
points, palettes, sound engine, timing, and the reasons behind each fix.

## Building from source (development)

Both scripts are Python and run on Windows (also from cmd), Linux and macOS.
They need Python 3 with the `capstone` and `ziglang` packages
(`pip install capstone ziglang`).

```sh
python make_dist.py     # -> dist/DuckTalesDOS (no game files needed; checks that no game material is included)
python build.py         # game in files/ -> build/ducktales_dos, build/settings
```

- SDL2 on Windows: the SDL2 MinGW development files. They are downloaded
  into `toolchain/` on first use (official release, checksum pinned), or
  taken from `SDL_DIR`.
  `make_dist.py` uses them on every platform, because it cross-compiles the
  Windows settings program.
- SDL2 on Linux/macOS: `build.py` uses the system SDL2 (`pkg-config sdl2`).
- Amiga sound option: `build.py` uses the Amiga game folder in `AMIGA`
  (default `../ducktales/files`).
- `.ipf` disk image import: `make_dist.py` adds `CAPSImg.dll` (the SPS
  decoder) from `CAPS_DLL` (default `../ducktales/build/CAPSImg.dll`) if
  present; `.adf` images work without it.

`build.sh` and `make_dist.sh` are wrappers for the same scripts.

Testing aids (environment variables): `DT_HEADLESS=1`, `DT_MAX_FRAMES=n`,
`DT_KEYS="frame:key,…"` (scripted input, `shot` / `f12` save a screenshot),
`DT_WAV=file` (raw audio capture).

## Repository layout

| Path | Contents |
|---|---|
| `tools/` | unpacker, analyser, recompiler (x86-16 → C) |
| `tools/amiga/` | Amiga 68000 recompiler and `sndgen.py` (Amiga sound code) |
| `runtime/` | SDL2 runtime: CPU helpers, DOS/BIOS, video, sound, Amiga modes, settings |
| `launcher/` | settings program, Amiga disk image import (`diskimage.c`) |
| `dist_src/` | installer (`install.py`, `install.bat`, `install.sh`) and the package README |
| `NOTES.md` | technical notes |

Not in the repository (made from the game, keep them local): `files/`,
`unpacked/`, `gen/`, `build/`, `dist/`, `build_tmp/`, `toolchain/`.

## Legal

*DuckTales* and its characters are trademarks of Disney. The game was
developed by Incredible Technologies. This project is not affiliated with or
endorsed by them. It contains no game code or data. It only provides tools
that convert a copy you own. The game files, and everything generated from
them (unpacked program, generated C, the compiled game), must not be
distributed.

The source code of this project is released under the [MIT licence](LICENSE).
Included components keep their own licences: SDL2 (zlib licence), the SPS
decoder library CAPSImg used for `.ipf` images (free for non-commercial
use), and the public-domain font8x8 fonts. The release package lists them
in `licenses/`.
