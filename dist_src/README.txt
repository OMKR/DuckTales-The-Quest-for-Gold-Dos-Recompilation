DuckTales: The Quest for Gold (PC/DOS, 1990) - native port
==========================================================

This package contains NO part of the game. You need your own copy of the
original DOS game (the folder with DUCKTALE.EXE, IT.DRV and the .ARC
files). The game is converted and compiled on your own computer from your
own copy.

Supported: Windows (x64; ARM64 through x64 emulation), Linux (x86_64,
aarch64) and macOS (Intel, Apple Silicon).

Getting started
---------------
Windows:
1. Copy the game's files into the "files" folder here, or run settings.exe
   and drop your game folder onto its window. "Game folder" turns green
   when DUCKTALE.EXE and IT.DRV are found.
2. Choose "Build game from game files". A console window runs install.bat.
3. Choose "Save and start game".

Linux / macOS:
1. Install SDL2 if needed:
     Debian/Ubuntu: sudo apt install libsdl2-2.0-0
     Fedora:        sudo dnf install SDL2
     macOS:         brew install sdl2
   Python 3.8 or newer is also needed (usually installed).
2. Copy the game's files into the "files" folder, then in a terminal:
     ./install.sh
   This builds the settings program and the game.
3. Run ./settings and choose "Save and start game".

What the installer does
-----------------------
On the first run it downloads the build tools into the toolchain folder
(about 100 MB) from their official sites and verifies their checksums:
  Zig 0.16.0 (C compiler)       https://ziglang.org
  Capstone 5.0.7                https://pypi.org/project/capstone
  Python 3.12.10 (Windows only) https://www.python.org
It then converts the game's program into C and compiles the game. This
takes a few minutes. Run it again whenever you change the game folder.
If a build fails with strange errors, delete the build_tmp folder and
build again.

Settings
--------
These can be changed in the settings program (ducktales_dos.ini):
- Game speed (% of the original), frame rate limit (fps), music speed
  (% of the original tempo, independent of the game speed).
- Graphics: EGA, CGA or Tandy, as the original game supports, or
  "Amiga 32 colours": the pictures of the Amiga version (needs your copy
  of the Amiga game, see below).
- Sound: Tandy 3-voice, PC speaker, "Amiga" or off. Amiga speech on/off.
  Volume.
- Fullscreen, window size, controls, controller dead zone, keyboard mode.
- Jungle nephew order: DOS (red, blue, green) or Amiga (green, blue, red).
- The code wheel check (copy protection).

The Amiga version
-----------------
The Amiga options use your copy of the Amiga game. Import its two disk
images: drop the .adf files (or .ipf, read with CAPSImg.dll) on the
settings window, or choose "Import Amiga disk image" and type the path.
They are extracted into the Amiga game folder (DT1 and DT2). A folder that
already holds DT1 and DT2 can be set as "Amiga game folder" instead.

- Amiga graphics need only the folder.
- "Amiga" sound plays the Amiga version's music, effects and speech with
  its own sound program and instruments. Its code is built from your Amiga
  copy: set the Amiga game folder before "Build game from game files", or
  choose "Add Amiga sound" later.
- With Amiga speech on, the Amiga voices are heard with the Tandy and PC
  speaker sound too (after "Add Amiga sound").

Controls
--------
During the game: PgUp/PgDn (or keypad +/-) = game speed, Ctrl+PgUp/PgDn =
music speed, Alt+PgUp/PgDn = frame rate limit (steps of 5, 1 with Shift;
shown in the window title), F11 = fullscreen, F12 = screenshot
(screenshot_001.bmp, _002, ...). On a game controller LB/RB change the
game speed, Back+LB/RB the music speed. Changes made during the game are
not saved; use the settings program for that.

Controls = keyboard: arrow keys and Space as in the game's manual. In
Scrooge's office, hold a direction and press Space.

Controls = joystick: a game controller (XInput etc.) is the PC joystick:
left stick or d-pad moves, A/X = fire button 1, B/Y = fire button 2. The
game does not ask about the joystick or calibrate it. With "Keyboard mode
= joystick" the arrow keys and Space/Ctrl move the joystick instead. In
the menus a controller button gives one press each time it is pressed.

Notes
-----
- The game folder, the Amiga game folder, the build_tmp folder and the
  ducktales_dos program are your copies of the games or made from them.
  Do not share them.
- The source code of this project is under the MIT licence (LICENSE). The
  licences of included components are in the licenses folder.
