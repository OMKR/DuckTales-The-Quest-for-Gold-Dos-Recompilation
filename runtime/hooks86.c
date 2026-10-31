/* Calls the recompiled code makes into the runtime at chosen instructions
 * (tools/analyze.py HOOKS): settings that change the game's own choices. */
#include <stdio.h>
#include <stdlib.h>
#include "cpu86.h"
#include "settings86.h"
#include "layout86.h"
#include "dos86.h"

#include "amigasnd.h"
#include "rival86.h"

int keyboard_keep_pending(void);   /* chips86.c */
int keyboard_flush_keep(int sc);
void keyboard_seen(void);
void keyboard_flushed(void);


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

/* the sound driver's "still playing" (far pointer DS:2028; the game waits
 * on it after the teleporter's failure sound, root 056B) */
static int driver_playing(void)
{
    CPU86 saved = R;
    uint16_t off = (uint16_t)rw(R.ds, 0x2028), seg = (uint16_t)rw(R.ds, 0x202A);
    push16(R.cs);
    push16(0xFFFF);
    RS_PUSH(R.cs, 0xFFFF);
    dispatch_far(seg, off);
    RS_POP();
    int playing = R.ax != 0;
    R = saved;
    return playing;
}

/* 02BB:013A, the campfire's picture is up (before every destination's
 * game). After a teleporter trip (DS:7900) the teleporter's sound (0x54,
 * root 04BF) was cut off here after a tenth of a second by the stop at
 * 0141 (the picture loads at once here, from a floppy it took seconds);
 * the Amiga game waits for its channel before its stop (node 0 0x386).
 * It plays to its end now (3.6 s on the Amiga, 2.4 s with Tandy), over
 * the campfire's picture. Then the Amiga voice. */
void hook_campfire(void)
{
    if (rb(R.ds, 0x7900))
        for (int k = 0; k < 30 * 60 && (amigasnd_music() ? amigasnd_playing(0x54) : driver_playing()); k++)
            wait_frames(1);
    amigasnd_event(AMIGASND_CAMPFIRE);
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

/* 040E:19A5 (far): flushes the keyboard (the BIOS buffer and the game's
 * key table DS:276C). Many screens loop: wait for the timer (040E:0DCC),
 * flush, draw, read the keys (the destination screen, 0CE1:1177-11EE: the
 * plane could not be chosen). On the PC most of a pass was drawing, so a
 * key pressed came after the flush and was read. Recompiled, the drawing
 * takes no time and every key comes during the wait, before the flush.
 * The flush is done here, but it leaves a key pressed that the game has
 * not looked at yet while it is waiting for one (since the last flush its
 * key reader has found no new press), and a key held down until it
 * repeats (on the PC a slow pass always saw one of its repeats; here they
 * are rarer than the passes). When the game has had a new press since the
 * last flush, it is acting on it, and the flush takes away all else, as
 * on the PC: the cave reads fire and a direction, walks to the next cave,
 * flushes (keys pressed during the walk made it choose again there).
 * No time limit. */
int hook_keyboard_flush(void)
{
    if (rival_probe) { pop16(); pop16(); return 1; }     /* the rival's test: no flush */
    if (!rb(R.ds, 0x27EC)) {          /* the game's key handler not installed: BIOS keys */
        if (!keyboard_keep_pending()) { keyboard_flushed(); return 0; }
        keyboard_flushed();
    } else {
        ww(0x40, 0x1A, rw(0x40, 0x1C));
        uint8_t shift = rb(R.ds, 0x27EE);
        for (int sc = 0; sc < 128; sc++) {
            int keep = keyboard_flush_keep(sc);
            wb(R.ds, 0x276C + (uint32_t)sc, (uint8_t)(keep ? keep | shift : 0));
        }
        keyboard_flushed();           /* a key survives one flush */
    }
    pop16();
    pop16();          /* retf */
    return 1;
}

/* 040E:1A35, exit of the key reader 040E:19C5: the game has looked at
 * the keys */
void hook_key_read(void)
{
    keyboard_seen();
}

/* The game's random numbers (040E:0EB8): x = ror3(x + 0x9248). Most seeds
 * lie on one cycle of 63331 values, but 2205 lie on short ones (716 values
 * down to fixed points), where the same few choices come round again and
 * again. 040E:0E0D, after the seed is set from the BIOS ticks: with
 * settings.fixed_random a seed off the main cycle is stepped with another
 * increment until it reaches it (off: as the PC game). */
static uint16_t rng_step(uint16_t x, uint16_t add)
{
    x = (uint16_t)(x + add);
    return (uint16_t)(x >> 3 | x << 13);
}

static int rng_main_cycle(uint16_t seed)
{
    static uint8_t known[8192];          /* bit per value: on the main cycle */
    static int ready;
    if (!ready) {
        /* 0 lies on the main cycle (its cycle is the long one) */
        uint16_t x = 0;
        do { known[x >> 3] |= (uint8_t)(1 << (x & 7)); x = rng_step(x, 0x9248); } while (x != 0);
        ready = 1;
    }
    return known[seed >> 3] >> (seed & 7) & 1;
}

void hook_random_seed(void)
{
    if (!settings.fixed_random) return;
    uint16_t x = rw(R.ds, 0x2229);
    while (!rng_main_cycle(x)) x = rng_step(x, 0x1235);
    ww(R.ds, 0x2229, x);
}

/* 015A:01F6, the map's destinations are about to be placed: 015A:00D7 takes
 * the 34 places from the start rand % 30 and a step rand % 5. Two
 * consecutive numbers of the game's generator are tied (for each start
 * only 2 of the 5 steps come), so of the 150 maps only 60 could appear.
 * With settings.fixed_random both are taken from one hash of the
 * generator's state: all 150, about evenly. */
void hook_map_shuffle(void)
{
    if (!settings.fixed_random) return;
    uint32_t h = rw(R.ds, 0x2229) * 0x9E3779B1u;
    h ^= h >> 15;
    h *= 0x85EBCA77u;
    h ^= h >> 13;
    h %= 150;
    ww(R.ss, R.bp - 4, (uint16_t)(h / 5));          /* start */
    ww(R.ss, R.bp - 0x60, (uint16_t)(h % 5));       /* step */
}

/* Draw-then-wait animations (core86.c tick_pct): the ticks slowed while
 * they run, by the drawing time the original PC added to each step:
 * the opening dialogue (02F5:0168, 0338: two screen copies and ten
 * sprites, then 7 ticks), the title's menus (02F5:0AEB, 0D89), the
 * destination screen (overlay 2, 0C17:02E4: a screen copy every tick) and
 * the cave's fall (overlay 6, 0C17:160D-1661). */
void tick_scale_set(int pct);
void hook_slow_dialogue(void) { tick_scale_set(80); }
void hook_slow_menu(void) { tick_scale_set(85); }
void hook_slow_pause(void) { tick_scale_set(80); }
void hook_slow_destination(void) { tick_scale_set(55); }
void hook_slow_fall(void) { tick_scale_set(40); }
void hook_slow_end(void) { tick_scale_set(0); }

/* Overlay 2, 0CE1:026F, after the code wheel check: the game's copy tests
 * BP (always non-zero) instead of the result in AX. With copy protection
 * on, the original test is restored (the opcode variant is chosen by the
 * byte in memory, tools/analyze.py PATCHED_OPCODES). */
void hook_code_wheel_result(void)
{
    wb(LOAD_SEG + 0xCE1, 0x270, settings.copy_protection ? 0xC0 : 0xED);
}




