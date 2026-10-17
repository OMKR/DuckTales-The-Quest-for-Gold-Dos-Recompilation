/* Calls the recompiled code makes into the runtime at chosen instructions
 * (tools/analyze.py HOOKS): settings that change the game's own choices. */
#include <stdio.h>
#include <stdlib.h>
#include "cpu86.h"
#include "settings86.h"
#include "layout86.h"
#include "dos86.h"

#include "amigasnd.h"

int keyboard_recent(uint32_t ms);   /* chips86.c */
int keyboard_unread(void);
void keyboard_read(void);

/* Overlay 8 (jungle), 0C17:1301: AX = 4 - turn counter (DS:5CFC) picks
 * the nephew who plays (3 red, 2 blue, 1 green; his EGA colours). The
 * Amiga version plays them green, blue, red: with jungle_order, 1 and 3
 * swap places. */
void hook_jungle_nephew(void)
{
    if (settings.jungle_order && R.ax >= 1 && R.ax <= 3) R.ax = (uint16_t)(4 - R.ax);
}

/* Overlay 8, 0E4C:0236 / 023E: around drawing one of the status bar's hats
 * (object DS:5ADC, frame at +12h; frames 2, 1, 0 = red, blue, green): in
 * the Amiga order the frames are mirrored, so the hats follow who plays. */
static void flip_hat_frame(void)
{
    if (!settings.jungle_order) return;
    uint32_t o = rw(R.ds, 0x5ADC) + 0x12;
    uint8_t f = rb(R.ds, o);
    if (f <= 2) wb(R.ds, o, (uint8_t)(2 - f));
}
void hook_jungle_hat_before(void) { flip_hat_frame(); }
void hook_jungle_hat_after(void) { flip_hat_frame(); }

/* Overlay 4, 0C17:0F27: the plane has landed (the flight's result at
 * [bp-0Ch], 0 = landed safely): the Amiga version's pilot speaks. */
void hook_plane_landed(void)
{
    if (rw(R.ss, R.bp - 0xC) == 0) amigasnd_event(AMIGASND_LANDED);
}

/* Overlay 3, 0C17:0A94: the scales have settled: the player's money
 * (DS:5AE2, where the minigames' takings are added, 040E:104B) against the
 * rival's (DS:54F2). */
void hook_scales_result(void)
{
    int32_t player = (int32_t)(rw(R.ds, 0x5AE2) | (uint32_t)rw(R.ds, 0x5AE4) << 16);
    int32_t rival = (int32_t)(rw(R.ds, 0x54F2) | (uint32_t)rw(R.ds, 0x54F4) << 16);
    amigasnd_event(player > rival ? AMIGASND_SCALES_WON : AMIGASND_SCALES_LOST);
    /* the Amiga game shows the result when the voice has been spoken */
    for (int k = 0; k < 20 * 60 && amigasnd_speaking(); k++) wait_frames(1);
}

/* 040E:0909, after the hardware detection: DS:204B selects the sound
 * driver (0 = PC speaker, 1 = Tandy 3-voice). The game picks Tandy only on
 * a Tandy 1000; the settings choose it here. With Amiga sound the Tandy
 * driver runs (silenced) and its sound requests go to the Amiga engine. */
void hook_sound_choice(void)
{
    wb(R.ds, 0x204B, settings.sound >= SOUND_TANDY ? 1 : 0);
}

/* Overlay 2, 0C17:07AD (far): the code wheel question. With the copy
 * protection off, return as if the right answer was given. */
int hook_code_wheel(void)
{
    if (settings.copy_protection) return 0;
    R.ax = 1;
    pop16();
    pop16();          /* retf */
    return 1;
}

/* 040E:0AFA (far, returns AL): "Do you have a joystick? (Y/N)". The
 * answer comes from the settings (DS:03BB = 1: joystick controls). */
int hook_joystick_question(void)
{
    SET_LO(R.ax, settings.joystick ? 1 : 0);
    wb(R.ds, 0x3BB, settings.joystick ? 1 : 0);
    pop16();
    pop16();          /* retf */
    return 1;
}

/* 040E:0B25 (far): joystick calibration. The game reads the upper left and
 * lower right corners and sets its thresholds a quarter of the range in
 * from each; the emulated port's limits are known, so they are set here
 * the same way. */
int hook_joystick_calibration(void)
{
    if (!rb(R.ds, 0x3BB)) return 0;
    uint32_t q = (JOY_MAX - JOY_MIN) / 4;
    ww(R.ds, 0x5AD8, JOY_MIN + q);   /* x: left below this */
    ww(R.ds, 0x88CC, JOY_MAX - q);   /* x: right above this */
    ww(R.ds, 0x5ADE, JOY_MIN + q);   /* y: up */
    ww(R.ds, 0x8922, JOY_MAX - q);   /* y: down */
    pop16();
    pop16();          /* retf */
    return 1;
}

/* 040E:1A35, exit of the key reader 040E:19C5: AX = the key, 0 = none */
void hook_key_read(void)
{
    if (R.ax) keyboard_read();
}

/* Overlay 2, 0CE1:026F, after the code wheel check: the game's copy tests
 * BP (always non-zero) instead of the result in AX. With copy protection
 * on, the original test is restored (the opcode variant is chosen by the
 * byte in memory, tools/analyze.py PATCHED_OPCODES). */
void hook_code_wheel_result(void)
{
    wb(LOAD_SEG + 0xCE1, 0x270, settings.copy_protection ? 0xC0 : 0xED);
}

/* 040E:19A5 (far): flushes the keyboard (BIOS buffer and the game's key
 * table). Many screens flush right after waiting for the next timer tick
 * and then read a key; on the original slow PC most of a loop pass was
 * drawing, so a key usually arrived after the flush. Recompiled, the pass
 * is nearly all waiting and the flush would swallow almost every key, so a
 * key pressed in the last 150 ms is kept, if the game has not read it yet:
 * a key it has read is flushed, as on the PC (the cave reads a key, moves,
 * flushes; keeping a read key moved or fired twice per press). */
int hook_keyboard_flush(void)
{
    if (!keyboard_recent(150) || !keyboard_unread()) return 0;
    pop16();
    pop16();          /* retf */
    return 1;
}



