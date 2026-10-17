# DuckTales: The Quest for Gold (DOS, 1990) – native port

Same goal as `../ducktales` (Amiga): statically recompile the original program
to portable C on an SDL2 runtime. The game files (`files/`) and everything
derived from them (`unpacked/`, generated code) are copyrighted and must not
be distributed.

## Original files (`files/`)

- `DUCKTALE.EXE` (165 218 bytes) – the program.
- `IT.DRV` (27 815 bytes) – 7 compressed driver modules (video and sound;
  the game supports CGA, EGA, Tandy/TGA graphics).
- `*.ARC` – data, same names as on the Amiga (`main.arc`, `plane.arc`, …),
  sizes differ (PC graphics).

## DUCKTALE.EXE structure

- Compiler: **Microsoft C 5.x** ("MS Run-Time Library - Copyright (c) 1988"),
  plus "Library code by Jim Weisz".
- The load image is **EXEPACK** compressed: `tools/unexepack.py` →
  `unpacked/DUCKTALE.EXE` (87 664 bytes, 659 relocations, entry 040E:46C3,
  SS:SP 1B89:0800). The 103 431 bytes appended after the load image are
  kept in `unpacked/DUCKTALE.EXE.tail`.
- The tail holds **10 Microsoft LINK overlays** (MZ blocks, overlay numbers
  1–10, 0.5–26 KB each, with their own relocations). Calls into overlays go
  through `INT 3Fh` (`CD 3F <ovl> <offset:u16>`); the entry segment 040E is
  the overlay manager (installs the INT 3Fh vector) before the C startup.
- DGROUP (DS) = segment 120E.
- Relocated segments: 0000, 0124, 015A, 01C4, … 120E (31 values).
- DOS calls: 116 × INT 21h, 7 × INT 10h, 1 × INT 33h (mouse), 2 × INT 13h.
  Joystick calibration text ("Move joystick upper left corner…").

## IT.DRV

    u16 count (7)
    u32 offset[count]
    module: u32 packed size, u16 4, u16 unpacked size, u16 0xFFFF, …, packed data

The game loads the modules at run time ("InitAV: Error loading video
driver." / "…sound driver."), so it contains the decompressor and calls the
drivers through a far pointer table.

## Findings so far

- **Overlays**: the INT 3Fh handler (040E:459E) takes `n` and an offset;
  `DGROUP:3234` holds the segment per n, `DGROUP:3270` the overlay number
  (n = 1..29; 10 overlays, overlay 8 has 10 segments). All overlays load at
  segment **0C17** (overlay area 0C17–120D, empty in the root image). Loader:
  `tools/dosexe.py`.
- **8087 emulator**: MSC emits `INT 34h–3Bh` for the ESC opcodes D8h–DFh,
  `INT 3Ch` for ESC with a segment override (here always CS) and `INT 3Dh` for
  FWAIT. The analyzer decodes them as x87 instructions (~500 sites).
- **Analysis** (`tools/analyze.py`): 397 functions, ~98 % of overlay code
  decoded, 10 switch tables, 13 INT 3Fh call sites, 6 interrupt handlers.
  Remaining unresolved: far calls through the **driver jump tables**
  (`lcall [0x1fd4..0x2030]`, ~1 100 sites) and a few `jmp/call word ptr`.
- **IT.DRV**: each module = 16-byte header (u32 packed, u16 4, u16 size,
  FFFFh, …) + LZW data (9–12-bit codes LSB first, 256 = reset, 257 = end) —
  same LZW as the game uses for its `.ARC` graphics (040E:0EFD).
  `tools/itdrv.py` unpacks them to `unpacked/drv/drv0..6.bin`. All say
  "Driver by Jim Weisz". After loading, the driver's first words (a table of
  near offsets) are copied to `DGROUP:1FD4` (video, 15 entries, far ptrs)
  and `DGROUP:2010` (sound, 9 entries); the game calls them with
  `lcall [table]`.
- **Driver choice** (040E:0617): video mode `DS:204A` = 0 CGA, 1 EGA,
  2 Tandy, 3 Hercules/MCGA (?) → module `DS:20F8[mode]` = 0, 1, 3, 2;
  sound `DS:204B` = 0 PC speaker, 1 Tandy → module `DS:20FC[n]` = 5, 4, 6.
  The game asks "Do you have a joystick? (Y/N)" and calibrates it.

## Plan

1. x86-16 analysis: functions, far/near calls, jump tables, INT 3Fh overlay
   calls (all 10 overlays), entry points through relocations.
2. Unpack IT.DRV with the game's own algorithm; identify which module is
   which driver (EGA, CGA, Tandy video; PC speaker, Tandy sound).
3. Static recompiler x86-16 → C (registers, segment:offset memory, flags).
   Overlays: INT 3Fh copies the overlay into the overlay area like the
   original manager, then calls the recompiled code.
4. Runtime: DOS (files, memory, vectors), BIOS (INT 10h/16h), PIT timer
   (INT 8), keyboard (port 60h/INT 9), joystick (port 201h), EGA hardware
   (planar VRAM, ports 3C4h/3CEh) and PC speaker/Tandy sound → SDL2.
   Recompile the EGA video driver from IT.DRV rather than reimplementing
   its API, if possible.
5. Settings program, installer and distribution package like the Amiga port.

## Self-modifying driver code

- Drivers are identified at run time by an FNV-1a hash of their first 16
  bytes (the entry table); the game patches bytes after it.
- The drivers store immediates into their own instructions
  (`mov cs:[imm], reg`). `Analyzer.patched_bytes()` finds stores that land
  inside decoded instructions; recomp reads such immediates/displacements
  from memory at run time (`rw(R.cs, addr)`).
- Rewritten opcodes are listed in `PATCHED_OPCODES` (EGA line drawer:
  inc/dec ax at 0650h, jmp short / or ax,ax at 0659h); recomp emits a branch
  on the byte in memory for each variant.
- Sound driver (PC speaker, module 5) dispatches through a jump table
  (`mov bx, cs:[bx+08CBh]; jmp bx`) and stored near pointers
  (`mov [m], imm` + `call [m]`); both are found by
  `driver_pointer_candidates()`.

## Status

Title screen, difficulty menu, Scrooge's office and the security (code
wheel) screen work in EGA mode 0Dh. PC speaker sound is rendered per timer
tick. Input: the game's own INT 9 handler fills a key state table
(DS:276C) for registered scan codes.

## Settings, sound devices, package

- `runtime/settings86.c` reads `ducktales_dos.ini` next to the program
  (speed %, sound 0/1/2 = off/PC speaker/Tandy, volume, fullscreen, scale,
  copy_protection, gamedir relative to the program folder).
- Hooks into the generated code (`HOOKS` / `RETURN_HOOKS` in analyze.py,
  implemented in `runtime/hooks86.c`):
  - 040E:0909 `hook_sound_choice` sets DS:204B (sound driver) from the
    settings; the game itself picks Tandy only on a Tandy 1000.
  - overlay 2 0C17:07AD `hook_code_wheel` returns AX=1 (answer accepted)
    when copy protection is off.
- Tandy sound: SN76489 at port C0h (3 tone voices + noise, 3.579545 MHz),
  mixed with the PC speaker per timer tick. `DT_WAV=file` captures the
  output as raw 16-bit mono 44.1 kHz (also headless).
- `launcher/settings.c` → settings.exe; `make_dist.py` → dist/DuckTalesDOS
  (no game material; `install.bat`/`install.sh` → `src/tools/install.py`
  unpacks, recompiles and compiles the user's copy).

## Joystick

- Port 201h is emulated with the timing measured in port reads (the game
  counts reads in 040E:0E5F): JOY_MIN/MID/MAX = 20/200/380, so results do
  not depend on host speed.
- Hooks: 040E:0AFA `hook_joystick_question` answers "Do you have a
  joystick?" from the settings (DS:03BB); 040E:0B25
  `hook_joystick_calibration` sets the thresholds (DS:5AD8/88CC x,
  5ADE/8922 y) a quarter of the range in from the limits, as the game's
  own calibration does.
- The game reads the joystick only a few times a second in menus, so a fire
  press is latched until one read has seen it.
- Input: SDL game controller (left stick/d-pad, A/X fire 1, B/Y fire 2) or,
  with keyboard_mode, arrows + Space/Ctrl. Test script: jleft/jright/jup/
  jdown/jfire with +/- in DT_KEYS.

## Graphics card choice

`graphics` in the ini: 0 EGA, 1 CGA, 2 Tandy, 3 Hercules (experimental,
not offered by the settings program). The game detects the card itself;
the runtime answers its probes as the chosen card would:
- EGA: INT 10h AH=12h BL=10h answers; EGA ports only for EGA.
- CGA: no EGA answer, CRTC at 3D4h → mode 04h. Palette via INT 10h AH=0Bh
  / port 3D9h. The self-modifying jump 040E:10EF has a 4th target (1263h)
  for CGA.
- Tandy: model byte F000:FFFE = FFh and FC00:0000 = 21h, INT 10h AH=05h
  AL=80h-83h page registers, port 3DFh (CRT/CPU page), 3DAh/3DEh palette.
  Mode 09h (320x200x16, 4 banks of 160-byte rows). Video memory is the top
  of RAM: DOS ends at 9C00h (BIOS reserves 16 KB), the game draws into
  9000h-9FFFh and selects the CRT page; B800h is a window on the CPU page.
- Hercules: CRTC at 3B4h, 3BAh bit 7 toggles, 3B8h mode control; 720x348
  graphics rendered from B000h/B800h. The driver addresses screen-sized
  buffers as a 32 KB interleaved page while the game allocates 28000 bytes,
  so such allocations get 32 KB (DOS memory extends to B000h). Sprites and
  text are right, but backgrounds come out as noise - unresolved.

## Copy protection and keyboard timing

- This copy of the game tests BP instead of AX after the code wheel check
  (overlay 2, 0CE1:026F `or bp, bp`), so every answer passed. With
  copy_protection on, `hook_code_wheel_result` restores `or ax, ax`
  (PATCHED_OPCODES variants; the variant byte is now the first byte in
  which the variants differ). Off: `hook_code_wheel` skips the question.
- Keyboard: typematic repeat (500 ms, then ~15/s) for the held key.
- 040E:19A5 flushes the keyboard. Loops such as the destination screen
  flush right after waiting for a timer tick and then read a key; on the
  original PC the pass was mostly drawing, recompiled it is mostly waiting,
  so the flush swallowed the keys (the plane could not be chosen).
  `hook_keyboard_flush` keeps a key pressed in the last 150 ms, but only
  when the game's key reader has not returned it yet (hook_key_read at
  040E:1A35). Keeping a read key made one press move or fire twice: the
  cave reads a key, moves, then flushes. A held key comes again with the
  typematic repeat, as on the PC.

## Game speed, frame rate, music speed

- The PC speaker / Tandy sound driver's timer interrupt (874 Hz) plays the
  music and every 12th interrupt counts the game's ticks (DS:2225, 32-bit,
  72.8 Hz); the game paces everything by that count (040E:0DCC).
- `music` (%) scales the timer interrupt rate: music tempo.
- `speed` (%): `game_clock()` (chips86.c) advances DS:2225 from the host
  clock at 72.8 Hz × speed, taking back the driver's own increments, so
  the game speed is independent of the music.
- The scales (overlay 3) pace their steps by game ticks (040E:0DCC after
  each flip): the tick rate is 40 % there (core86.c tick_pct()).
- Minigames that pace by vertical retrace (mountain, cave) draw a full
  screen per step: on the PCs of the time a step took two retraces. In
  overlays other than 1, 2, 4 (plane), 10 a retrace comes every second
  frame (core86.c retrace_pct()).
- `fps`: vertical retrace (port 3DAh bit 3) at that rate; the game waits
  for it before drawing/flipping. The window is also presented at it.
- Controller fire in the menus (not overlays 3-9): one press is seen for
  one joystick measurement (port 201h write to write), then not until the
  button is released (chips86.c pulse1). Menus act on every read with the
  button down; holding it picked several investments at once. Minigames
  keep the held button.
- Key repeat (typematic, chips86.c key_repeat) is timed by the clock:
  500 ms, then 10.9/s (BIOS default). It was counted in frames, which
  follow fps x speed, so at 100 fps or 150 % a short press repeated.
- A key pressed during the title screen skips the intro (original
  behaviour, 05F5:0A7D checks DS:7979).

## DOS memory blocks

The emulated DOS arena had room for 64 blocks without a check. The game
keeps 55-61 blocks in normal play (each destination info screen, flight and
location loads graphics), so visiting most destinations overflowed the table
and corrupted the allocator: an allocation returned 07DD (inside the game's
code) and the LZW decoder overwrote the self-modifying jump at 040E:10EF
("FATAL: patched jump" when the plane game loaded). The table now holds 1024
blocks (real DOS has no such limit) and overflow is fatal. The patched-jump
error now prints the bad displacement and location.
- In-game keys (chips86.c `speed_key`): PgUp/PgDn or keypad +/- = game
  speed, Ctrl = music speed, Alt = frame rate limit, Shift = step 1;
  F11 fullscreen, F12 screenshot.bmp. Values in the window title.
- Controller: LB/RB = game speed -/+5, Back+LB/RB = music speed.

## Startup speed

- No vsync: the recompiled CPU runs in the same thread as presenting, and
  waiting for the monitor on every frame left the game little time (the
  title picture took 3-4 s to decode and appear). Frames are paced by the
  frame rate limit.
- Vertical retrace (3DAh bit 3) is one per presented frame (`video_vblank`),
  seen by the next 4 reads; a clock-phase retrace could fall between the
  game's reads and was seen only ~30 times a second. While the game polls
  3DAh, poll_count is lowered so the frame clock is checked often.

## Amiga graphics (stage 1: decoding) - tools/amigagfx.py

- Amiga .arc files: same container and headers as DOS; header word 5 = 1
  selects the RLE that DOS also has (040E:13CE). Decoded: 5 bitplanes
  (32 colours) by 8-pixel column (column, plane 0..4, `height` bytes each);
  sprite strips hold all frames side by side. DOS pictures: LZW, 4 bits per
  pixel chunky.
- Entries line up with the DOS archives except main (A 42 / D 43), exp,
  jg, mt, mac.
- Palettes: 12-bit RGB tables of 32 words in the Amiga program, mostly
  0xB066-0xB5E6 step 0x40. Full-screen pictures get one by comparing with
  the DOS picture; partial backgrounds (e.g. ps.arc) and sprites still need
  the right palette (next: log palette loads from the Amiga port).

## Amiga graphics mode - runtime/amiga86.c
- One palette for the whole screen and sprites in their own Amiga colour
  indices, with no remapping: the Amiga game has a single 200-line view
  (its palette log shows no split) and draws every picture/sprite with the
  scene palette, status bar included (map: 0xB126, plane: 0xB1E6). EGA
  black is always colour 0 (black in every palette); the votes had turned
  it brown/white in some scenes (jungle status bar, cave map).
- Mountain and jungle: the Amiga code (node 6 0x23E, node 9 0x14E4) sets
  colours 17-20 to the playing nephew's (green 062/082/0B3/4D8, blue
  005/008/11C/44D, red 500/800/C00/D44); the DOS game sets EGA colour 5/13
  (2/10, 9/1, 4/12), which is read to choose them.
- Status bar pictures from another archive: the Amiga minigame archives
  have their own versions as extra entries (hats, day box), used by size,
  frames paired by colour with the DOS ones (the DOS game picks a
  nephew's hat by frame; the jungle's Amiga strip has another order).
  The money bag (MAIN.ARC 23) has none: in the mountain and the jungle its
  pens are moved to scene pens by tables read off the Amiga's screens
  (bag_pens[], not found in its files); elsewhere the DOS picture is drawn
  in the scene's colours. The money bar (bag pen 9) gets a frame of its
  own colour, as on the Amiga whatever the money: jungle blue (26) around
  red (21), mountain light blue (25) around blue (26). The money (EGA
  yellow text) is white (pen 1) in the minigames, as on the Amiga.
  Jungle: the status frame (EGA 6) is the Amiga's pen 17 (dark green);
  in the mountain it is pen 21, which the learned colours give already.
- Driver drawing calls go to the buffer of cs:001E, a page or RAM buffer.
- Colour cycling: the Amiga's 50 Hz interrupt (0x617C) runs mode $2C26
  through the table at data 0x2BFE (colour list, count, first pen; one step
  per 127/$2C2C frames). The campfire (mode 4, speed 15) turns pens 28-31
  through F00 F70 FF7 000; done for MAIN.ARC 31 in amiga_render.
- The cave map: the game draws its frame sprite (36C80h), draws the rooms
  over it (pixels: grey 7, slime green 2, ...) and copies that screen area
  back into the sprite's buffer, then draws the sprite each frame. The
  copy now updates the Amiga sprite inside its shape (copy_rect); before,
  the loaded frame picture hid the rooms.
- Single pixels (driver 0515: the cave map) use the nearest colour to the
  EGA colour itself (EGA_PIXEL), not the learned one: the map marks rooms
  next to a pit (slime) in EGA green, which the learned colours turned
  brown.
- Archive alignment: an entry of another size is paired with the one at
  the same position when the next entries match again (EXP.ARC 17 is
  136x112 / 144x102; before, the cave views 17-20 got their neighbours'
  pictures and 21 none, shown in EGA; JG.ARC 15-18 likewise).
- All 273 DOS pictures/sprites have an Amiga pair that decodes (checked
  against both archives). JG.ARC 16 (water ripples under the hippos) has
  25 byte columns for 3 frames of 80 pixels: the frames are taken as the
  header gives them, the rest transparent (it was drawn in EGA).
- Jungle nephew order (jungle_order): DOS plays red, blue, green (overlay
  8, 0C17:12FD: AX = 4 - turn counter DS:5CFC), Amiga green, blue, red. A
  hook at 0C17:1301 swaps 1 and 3; hooks around the hat draw (0E4C:0236 /
  023E, frame at [DS:5ADC]+12h) mirror the hat frames so the status bar
  follows. Works in every graphics mode.
- Sprites with more DOS frames than Amiga ones, each its own way. MT.ARC 7
  (the goat, 8 / 4): the missing frames are made when the strip loads
  (synth_frames). The frames both have teach
  the DOS -> Amiga pens: a pixel takes the pen of the same detail in a
  known frame (a 7x7 DOS window matching in at least 47 of 49 pixels
  within 6 pixels: the eye and the muzzle come along), else the most
  common pen of its 5x5 / 3x3 / 1x1 EGA neighbourhood. Before, they were
  drawn from the EGA data. PS.ARC 32 (the special monkey's head, 6 / 5;
  monkey_frames): the DOS frame 5 is frame 0 with the head one row lower
  (the lever stays) and a tuft of black dots on top; the new frame does
  the same to the Amiga frame 0 (rows 0-3 kept, the shaft in row 4, the
  head moved down, the tuft dots in pen 31).
- The inventor's picture (MAIN.ARC 41: plane broken, Bombasium) was shown
  in EGA: the Amiga MAIN.ARC's offset table ends with a wrong value, so
  its last entry was not read. The last entry now runs to the end of the
  file (arc_entry, amigagfx.arc_entries); palette forced to table entry 15.
- Treasure found (MAIN.ARC 35, node 1 0xD62): set 0xB with 0x77 or 0x27,
  or set 0x18 with 0x49, at random. The scales' result waits for its voice
  (hook_scales_result). Voices that wait: on their own channel (the
  cave's 0x48 plays on channel 1), plus stops and the theme (1) wait.
- Voices with Tandy / PC speaker: with speech on and an Amiga game folder,
  the Amiga engine runs in voices_only mode beside the DOS driver (its
  other requests are dropped there); the speaker driver's request entry
  (module 5, 0x408) is forwarded too. Its output is mixed over the DOS
  sound.
- Treasure screen: the DOS code (root 0C3D, after the picture and the
  money) requests sound 0x49, its fanfare. In the Amiga engine 0x49 is a
  voice, and with the cave's set 0x18 still loaded it said the cave's
  "Wow, look at that" again over the treasure's own comment. The game's
  0x49 within 15 s of the treasure picture is not passed to the Amiga
  engine (the comment's own 0x49 is, own_request).
- Speech setting (speech, settings "Amiga speech"): off drops voice
  requests (0x77, 0x71) and the triggers in amigasnd.c.
- The treasure chest (MAIN.ARC 35) is forced to table entry 18 (0xB4E6);
  the comparison chose a grey-blue one that looked like EGA.
- amigasnd.c: AllocMem sizes and Read lengths are kept inside the 16 MB
  Amiga memory (a heap corruption crash, c0000374, was seen once; not
  reproduced in 3 x 40000-frame random-input runs).
- Map: the frame (MAIN.ARC 20) is forced to the map's/office's palette
  (table entry 3, 0xB126; picture_palette[]), the comparison picked a
  wrong one. A destination's info screen with its title in blue on green is
  the game's own marker for the place where the Bombasium (needed for the
  teleporter) is, not a colour error.

graphics = 4 (GFX_AMIGA): the game sees an EGA card and its EGA driver
still runs, but the screen is shown from surfaces with 12-bit RGB pixels
(no palette limit) on which every drawing call of the driver is repeated:
- pictures (loader hook 040E:199F): the Amiga entry of the same archive
  (archives aligned by type and height), full-screen pictures with the
  Amiga palette that best matches the DOS picture, small ones (tiles,
  icons) with one palette per archive (all its small pictures compared);
- sprites: Amiga strips (frames side by side), colour 0 transparent,
  shown in the screen's palette (their own colour indices);
- 01EF copy, 035D sprite, 01B0 clear, 0515 pixel, 069B/073F lines, 081F/0909
  text; 0101/0155 page copy/wipe done in C (EGA planes too) with waits;
- the game's own EGA colours (fills, text, pictures without an Amiga
  version) become the Amiga colour learned from the picture pairs for the
  screen's palette (majority vote), else the nearest colour.
- an all-black EGA palette (fade) shows black.
- palettes: the Amiga program's table of 23 palettes (0xB066-0xB5E6, 64
  bytes apart). Comparing with the DOS pictures picks the wrong one for most
  minigame scenes, so `scene_palette[]` fixes them per archive (seen by
  running the Amiga port with DT_PALLOG=file, which logs every palette it
  shows: jungle 0xB326, mountain 0xB226, nature photography 0xB266, cave
  0xB2E6, plane 0xB1E6, scales 0xB1A6, money bin 0xB526).
  Colours learned for fills/text are voted for that palette.
0589 (lines in any direction: the climbers' ropes) rewrites its own code;
the EGA driver draws it and the pixels it changed are copied to the surface
(amiga_driver_after). Not done: CGA/Tandy with Amiga pictures.

- Pixels are palette indices (0-31) or EGA(c) for the game's own EGA
  colours; a page has one palette (the scene's). EGA colours: the colour
  learned from picture pairs only if at least 70 % of the votes (flat 3x3
  areas) agree, else the nearest palette colour (black: colour 0).
- 24-row placeholders at the bottom of scene pictures (one colour + size
  label) are cleared to black.
- Game speed also scales the frame rate (vertical retrace), which the game
  paces many things by.

## Amiga sound engine (research)

Sound requests use the same numbers in both versions (DOS sound driver
call 0415 / Amiga f00_057d2(n)): 0 stop, 1 title, 5, 6, 8 (engine, only
stores the value), 9, 31, 33, 36 ... Amiga-only 100-127 = speech (DOS never
asks; ~25 call sites, e.g. 119 x6, 110 x3, 103-109 in overlay 7).

Amiga program (DT1/DuckTales, code hunk 0 / data hunk 1):
- request table: hunk0+$6200, 130 longs -> descriptor {channel, priority,
  bytecode...} (entry 6 points into the data hunk). Request: if priority >=
  channel's current ($3b), channel restarts at the bytecode.
- 4 channel structs at data $2a12, $2a5e, $2aaa, $2af6 (ptrs at $29fa):
  +0 period (16.16), +4 bytecode ptr, +8 duration counter, +c transpose,
  +e slide, +12 octave/transpose, +14 vibrato step, +18/19 vibrato timer,
  +1a/1e/1f/20/21/22 envelope (ptr, mode, idx, step, target, level),
  +23, +24 call-stack depth, +2a call stack, +37/38 vibrato delay,
  +39 retrigger flag, +3a instrument, +3b priority, +3c/+40 envelope
  tables, +44/+45/+46 volume, +48/+49 tempo accumulator/tempo.
- tick (f00_058bc, vertical blank): per channel, tempo accumulator; when the
  duration counter ends, interpret bytecode (f00_05a72): bytes < $80 =
  note (low nibble pitch via period table data $29de: 1ac 194 17d 168 153
  140 12e 11d 10d fe f0 e2, high nibble octave) + duration byte; $80-$ab =
  commands via table data $292e (handlers $5aca-$5e64: $85 call (long
  addr), $86 return, $84 jump, $87 octave/transpose, $8f volume, $90 tempo,
  $91 slide, $95 vibrato, $9a sample (entry, period, ...) etc.).
  Envelope (f00_0544c) modes via table data $2b6e. Volume = level -
  $45 - $46 - global $2b8c, /4 -> AUDxVOL.
- instruments: table data $27e4 (= -$581a(a4)) of block-list pointers
  {count, AUDxLEN, AUDxLC, ...}; the audio interrupt handlers
  ($55d6/$5646/$56b6/$5726) feed the next block of the list. Built by
  loaders: $49a0 at startup (dt1.as entries 11-19 + 6-octave synthetic
  waveforms $48d4 from a 256-byte table), $46ce splits samples into 3
  octaves; scene loaders replace instrument sets.
- .as files: same container as .arc. Type 3 = raw 8-bit sample, type 6 =
  instrument (header with loop info), type 5 = 4-bit delta (decoded length
  = 2 x packed), streamed in 1 KB blocks (speech, ids 100+).

## Amiga sound option - runtime/amigasnd.c, tools/amiga/sndgen.py
- Not a re-implementation: the Amiga program's root node (301 functions,
  sound engine and instrument loaders) is recompiled by sndgen.py (the
  Amiga port's recompiler) to gen/amigasnd_gen.c, with its runtime names
  prefixed am_ (amigasnd_rt.h) so they live beside the DOS runtime. It runs
  in its own 16 MB Amiga memory; amigasnd.c gives it the hunk loader, Paula
  (4 channels, DMACON/INTENA/INTREQ, block interrupts to 0x55D6-0x5726) and
  exec/dos calls (memory as a bump heap, Open/Read/Seek of dt1.as/dt2.as).
- Start-up as the Amiga game: $B39A = sound table, $2B4A = A4, two
  AllocMem(0x3E80) buffers, dt1.as/dt2.as handles at $A7CE/$A7D2, then
  0x5868 (reset), 0x4AF0 (synthetic instruments), 0x5594, set 0. DMACON and
  INTENA master bits start set (the OS leaves them on).
- DOS side: Sound = Amiga (3) runs the Tandy driver silenced; its request
  entry (driver 4, 0x415, sound number at SP+4) is passed to 0x57D2. The
  player 0x58BC ticks at 50 Hz x music speed while rendering.
- Instrument sets: the DOS game has no equivalent of 0x4D04. The set
  follows the DOS overlay (each minigame is one; archives are no use, the
  mountain opens PLANE.ARC too): 3 scales, 4 plane 5, 5 mountain 4, 6 cave
  0x15, 7 photography 0xA, 8 jungle 8, 9 money bin 0xC. Overlays 1, 2, 10
  (title, office, map): sound 1 loads set 0, sounds 3/4/5/31 set 1.
- Voices: most sets hold one or a few voice samples (type 5); the Amiga
  code loads one and requests 0x77 (plays the set's first sample; 120-123
  the 2nd-5th). The DOS game requests 0x77 itself in the mountain and the
  jungle (behind its sound-on flag DS:3BE). The others are triggered here:
  office Launchpad texts (sets 3/0xD/0xE, by the drawn string), map
  pictures MAIN.ARC 36/37 (0x1E), 39 (9/0x11/0x12 at random), 38
  (0xF/0x10); cave sounds 0x47/0x48/0x49/0x4B load 0x28/0x16/0x18/7
  first; plane landed (hook ovl 4 0C17:0F27, result [bp-0Ch] = 0: sets
  6/0x13/0x14); scales result (hook ovl 3 0C17:0A94, DS:5AE2 player vs
  DS:54F2 rival: 0x1A/0x1F/0x20 or 0x1D/0x21/0x23-0x26). The rival's
  taunt texts play the jingle 0x6E. The campfire (MAIN.ARC 31, node 0 0x33E) loads
  set 0x19 and plays it with 0x71.
- The Amiga game waits for each voice (tst.l -$55E8(a4) = channel 0's
  program pointer, data 0x2A16); the DOS game does not. While a voice
  plays, set loads and channel-0 requests (stops, music) wait in amigasnd.c
  and follow when it ends (at most 20 s). An overlay switch (the next scene,
  e.g. the plane after Launchpad) waits for the voice (ovl_load). Sounds 103-109 (photography) and
  110/111 are Amiga-only effects/jingles without DOS triggers.
- Without the Amiga game at build time the file compiles to stubs and the
  option falls back to Tandy.

