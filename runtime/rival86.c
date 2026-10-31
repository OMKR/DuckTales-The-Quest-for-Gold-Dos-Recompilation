/* The plane's rival under the plane's collision rules (settings.rival_collisions).
 *
 * In the plane game (overlay 4) the rival's plane, when it races to the
 * same place (DS:5ED4), is a scripted progress: the scenery function
 * 0C17:0F78 moves it 2 pixels per scenery column drawn (DS:4D4C within the
 * 80-pixel column DS:8D7C), draws it at a fixed height (object DS:5D12,
 * PLANE.ARC 59) and lands it in the last three columns; nothing hits it.
 *
 * Here it obeys the player's rules. The game's own collision test 0C17:261F
 * is run for the rival: the data segment and the CPU and FPU are saved,
 * the player's box (DS:36C8-36D4, from the per-angle offsets at DS:0F6A,
 * 0F7A, 0F8A, 0F9A), position, angle and column are set to the rival's,
 * the test runs with its drawing, sounds and keyboard flush held back
 * (rival_probe), and what it changed in the player's state (angle, crash
 * flags, position, speed) tells whether it was hit; then everything is put
 * back. A hit spins the plane through two turns, dips it and holds it in
 * place meanwhile. The rival sees ahead with the same test and, three
 * times in four, climbs or dives out of the way.
 *
 * The turning frames are made from the rival's own sprite, turned 22.5
 * degrees a frame anticlockwise as the player's 16 frames (PLANE.ARC 61)
 * turn: each pixel of a 32x32 frame takes the colour most of its 8x8
 * sub-samples fall on, in the DOS colours and in the Amiga pens. They are
 * kept in DOS memory at D000:0000, a sprite strip like the player's. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cpu86.h"
#include "settings86.h"
#include "dos86.h"
#include "layout86.h"
#include "rival86.h"

void fpu_save(void);
void fpu_restore(void);
const uint8_t *amiga_sprite_px(uint32_t lin, int *fw, int *h, int *frames, int *arc);
void amiga_add_sprite(uint32_t lin, int fw, int h, int frames, uint8_t *px, int arc);

int rival_probe;

#define DGROUP_LIN(off) (LIN(R.ds, 0) + (off))
#define OVL_CS (LOAD_SEG + 0x0C17) /* the overlays' code segment */
#define RIVAL_OBJ 0x5D12        /* the rival's sprite object */
#define PLAYER_OBJ 0x6F3E       /* the player's turning sprite (16 frames, 32x32) */
#define TURN_SEG 0xD000         /* the turning frames' DOS data */
#define FRAMES 16
#define SIZE 32
#define LIFT 10                 /* rows from the rival's top to a turning frame's top */
#define CRUISE 0x18             /* its height in the game */
#define SPIN_STEPS 32           /* two turns, back level */
#define DIP 16
#define GRACE 12
#define LOOK_AHEAD 3            /* points ahead it looks at, 40 pixels apart */

static struct {
    uint32_t source;            /* the rival sprite's DOS data the frames were made from */
    int made;
    uint16_t col, rel;          /* DS:8D7C, DS:4D4C at the start of a scenery step */
    int spin, grace, dip;
    int target;                 /* height it climbs or dives to, 0 = none */
    uint16_t hit_col, hit_type; /* the obstacle last hit (the game's DS:FBC, FBE for the rival) */
    uint16_t seen_col, seen_type, seen_dodge;   /* the obstacle ahead and the decision */
    uint8_t obj[0x14];          /* the rival's object while a turning frame is drawn */
    int patched;                /* obj holds it: put back after the drawing */
    uint32_t rng;
} rv;

static int rival_on(void)
{
    return settings.rival_collisions && rb(R.ds, 0x5ED4) != 0;
}

static uint32_t rnd(void)
{
    if (!rv.rng) rv.rng = 0x9E3779B9u ^ (uint32_t)time(NULL);
    rv.rng ^= rv.rng << 13;
    rv.rng ^= rv.rng >> 17;
    rv.rng ^= rv.rng << 5;
    return rv.rng;
}

/* ---- turning frames ------------------------------------------------------------------- */
static void turn(const uint8_t *src, int sw, int sh, int stride, uint8_t clear, int k, uint8_t *dst, int dstride)
{
    const int S = 8;
    double a = -k * M_PI / 8, c = cos(a), s = sin(a);
    double cy = (sh - 1) / 2.0, cx = (sw - 1) / 2.0, oc = (SIZE - 1) / 2.0;
    for (int y = 0; y < SIZE; y++)
        for (int x = 0; x < SIZE; x++) {
            int votes[256] = {0}, opaque = 0, best = -1;
            for (int sy = 0; sy < S; sy++)
                for (int sx = 0; sx < S; sx++) {
                    double px = x - 0.5 + (sx + 0.5) / S - oc, py = y - 0.5 + (sy + 0.5) / S - oc;
                    int ix = (int)floor(c * px + s * py + cx + 0.5), iy = (int)floor(-s * px + c * py + cy + 0.5);
                    uint8_t v = ix >= 0 && iy >= 0 && ix < sw && iy < sh ? src[iy * stride + ix] : clear;
                    if (v != clear) { votes[v]++; opaque++; if (best < 0 || votes[v] > votes[best]) best = v; }
                }
            dst[y * dstride + x] = opaque * 2 >= S * S ? (uint8_t)best : clear;
        }
}

static void make_frames(void)
{
    uint32_t o = RIVAL_OBJ;
    int wb = rw(R.ds, o + 4), h = rw(R.ds, o + 6), stride = rw(R.ds, o + 8);
    uint32_t lin = LIN(rw(R.ds, o + 0xE), rw(R.ds, o + 0xC)), psize = rw(R.ds, o + 0x10);
    rv.source = lin;
    rv.made = 1;
    int w = wb * 8;
    if (w <= 0 || h <= 0 || w > 64 || h > 64) return;
    /* the DOS colours, 16 = clear (mask plane) */
    uint8_t src[64 * 64];
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t i = lin + (uint32_t)(y * stride + x / 8);
            int bit = 7 - x % 8, c = 16;
            if ((mem[(i + 4 * psize) & 0xFFFFF] >> bit) & 1) {
                c = 0;
                for (int p = 0; p < 4; p++) c |= ((mem[(i + p * psize) & 0xFFFFF] >> bit) & 1) << p;
            }
            src[y * 64 + x] = (uint8_t)c;
        }
    uint8_t frame[SIZE * SIZE];
    uint32_t base = (uint32_t)TURN_SEG << 4, row = FRAMES * SIZE / 8, plane = row * SIZE;
    memset(mem + base, 0, 5 * plane);
    for (int k = 0; k < FRAMES; k++) {
        turn(src, w, h, 64, 16, k, frame, SIZE);
        for (int y = 0; y < SIZE; y++)
            for (int x = 0; x < SIZE; x++) {
                uint8_t c = frame[y * SIZE + x];
                if (c == 16) continue;
                uint32_t i = base + y * row + k * (SIZE / 8) + x / 8;
                uint8_t bit = (uint8_t)(0x80 >> (x % 8));
                for (int p = 0; p < 4; p++) if (c >> p & 1) mem[i + p * plane] |= bit;
                mem[i + 4 * plane] |= bit;
            }
    }
    /* the Amiga pens (0 = clear) */
    int afw, ah, aframes, arc;
    const uint8_t *apx = amiga_sprite_px(lin, &afw, &ah, &aframes, &arc);
    if (apx && afw <= 64 && ah <= 64) {
        uint8_t *px = malloc(FRAMES * SIZE * SIZE);
        if (!px) return;
        for (int k = 0; k < FRAMES; k++) turn(apx, afw, ah, afw * aframes, 0, k, px + k * SIZE, FRAMES * SIZE);
        amiga_add_sprite(base, SIZE, SIZE, FRAMES, px, arc);
    }
}

/* ---- the game's collision test, for the rival ---------------------------------------- */
static uint8_t ds_copy[0x10000];

static float rf(uint32_t off) { float f; memcpy(&f, mem + DGROUP_LIN(off), 4); return f; }
static void wf(uint32_t off, float f) { memcpy(mem + DGROUP_LIN(off), &f, 4); }

/* Is a plane at angle `angle`, its 32x32 frame's top left at (x, y)
 * within scenery column `col`, hit by that column's obstacles? `dedupe`
 * keeps the rival's last hit (the game hits an obstacle once). */
static int probe(int col, int x, int y, int angle, int dedupe, uint16_t *hcol, uint16_t *htype)
{
    if (col < 0 || col >= (int)rw(R.ds, 0x5CE6) + 2) return 0;
    uint32_t ds = DGROUP_LIN(0);
    CPU86 saved = R;
    int saved_poll = poll_count, saved_rs = rs_top;
    memcpy(ds_copy, mem + ds, 0x10000);
    fpu_save();

    /* the column's obstacles at its own origin, the plane relative to it */
    int first = (int16_t)rw(R.ds, 0x4F44 + 4 * col);
    if (first >= 0)
        for (uint32_t e = 0x79F2 + 14u * (uint32_t)first; rb(R.ds, e) == (uint32_t)col; e += 14)
            ww(R.ds, e + 2, 0);
    ww(R.ds, 0x36CE, (uint16_t)(x + (int8_t)rb(R.ds, 0x0F6A + angle)));
    ww(R.ds, 0x36D4, (uint16_t)(x + (int8_t)rb(R.ds, 0x0F8A + angle)));
    ww(R.ds, 0x36CA, (uint16_t)(y + (int8_t)rb(R.ds, 0x0F7A + angle)));
    ww(R.ds, 0x36C8, (uint16_t)(y + (int8_t)rb(R.ds, 0x0F9A + angle)));
    wf(0x6D18, (float)x);
    wf(0x6D3E, (float)y);
    ww(R.ds, 0x974C, (uint16_t)angle);
    ww(R.ds, 0x0FB4, (uint16_t)col);
    ww(R.ds, 0x0FBC, dedupe ? rv.hit_col : 0xFFFF);
    ww(R.ds, 0x0FBE, dedupe ? rv.hit_type : 0xFFFF);
    wb(R.ds, 0x5ED4, 0);                         /* not the rival's arrival test */
    ww(R.ds, 0x0FAA, 0);
    ww(R.ds, 0x0FAC, 0);
    wb(R.ds, 0x0FB6, 0); wb(R.ds, 0x0FB7, 0); wb(R.ds, 0x0FB8, 0);
    ww(R.ds, 0x0FC0, 0);
    float speed = rf(0x0FAE);

    rival_probe = 1;
    poll_count = 1 << 30;                        /* no timer, frame or keyboard in between */
    push16(OVL_CS);                              /* far call to 0C17:261F */
    push16(0xFFFF);
    RS_PUSH(OVL_CS, 0xFFFF);
    dispatch_far(OVL_CS, 0x261F);
    RS_POP();
    int result = R.ax;
    rival_probe = 0;

    int hit = result == 1 || rb(R.ds, 0x0FB6) || rb(R.ds, 0x0FB7) || rb(R.ds, 0x0FB8) ||
              rw(R.ds, 0x0FC0) || rw(R.ds, 0x974C) != (uint32_t)angle ||
              rf(0x6D18) != (float)x || rf(0x6D3E) != (float)y || rf(0x0FAE) != speed;
    if (hit) {
        /* the obstacle: the game notes its column and type for most hits */
        *hcol = rw(R.ds, 0x0FBC) == 0xFFFF ? (uint16_t)col : (uint16_t)rw(R.ds, 0x0FBC);
        *htype = (uint16_t)rw(R.ds, 0x0FBE);
    }

    memcpy(mem + ds, ds_copy, 0x10000);
    fpu_restore();
    R = saved;
    poll_count = saved_poll;
    rs_top = saved_rs;
    return hit;
}

/* the rival at a height, `ahead` pixels on: hit? (the column it is in and the next) */
static int hit_at(int ahead, int top, int dedupe, uint16_t *hcol, uint16_t *htype)
{
    int col = rw(R.ds, 0x8D7C), rel = rw(R.ds, 0x4D4C) + ahead, angle = 0;
    col += rel / 80;
    rel %= 80;
    int y = top - LIFT;
    if (probe(col, rel, y, angle, dedupe, hcol, htype)) return 1;
    return rel + SIZE > 80 && probe(col + 1, rel - 80, y, angle, dedupe, hcol, htype);
}

/* ---- hooks ---------------------------------------------------------------------------- */
/* 0C17:0F84, the scenery step begins: where the rival is */
void hook_rival_step_begin(void)
{
    if (rival_probe || !rival_on()) return;
    rv.col = (uint16_t)rw(R.ds, 0x8D7C);
    rv.rel = (uint16_t)rw(R.ds, 0x4D4C);
}

/* 0C17:132B, the scenery step ends: spin, dodge, hit */
void hook_rival_step_end(void)
{
    if (rival_probe) return;
    if (!rival_on()) {
        rv.made = 0;
        rv.spin = rv.grace = rv.dip = rv.target = 0;
        rv.hit_col = rv.seen_col = 0xFFFF;
        return;
    }
    int len = rw(R.ds, 0x5CE6), col = rw(R.ds, 0x8D7C);
    int landing = col >= len - 3;
    int y = (int16_t)rw(R.ds, RIVAL_OBJ + 2);
    if (rv.spin) {
        ww(R.ds, 0x8D7C, rv.col);                /* held in place while it spins */
        ww(R.ds, 0x4D4C, rv.rel);
        rv.spin++;
        if (rv.dip < DIP && !landing) { rv.dip++; y++; }
        if (rv.spin > SPIN_STEPS) { rv.spin = 0; rv.grace = GRACE; }
    } else if (!landing) {
        uint16_t hcol, htype;
        if (rv.grace) rv.grace--;
        else if (hit_at(0, y, 1, &hcol, &htype)) {
            rv.hit_col = hcol;
            rv.hit_type = htype;
            rv.spin = 1;
            rv.dip = 0;
            rv.target = 0;
        }
        if (!rv.spin) {
            /* an obstacle ahead: decide once whether to dodge it */
            int threat = 0;
            for (int k = 1; k <= LOOK_AHEAD && !threat; k++)
                if (hit_at(40 * k, y, 0, &hcol, &htype)) threat = 1;
            if (threat && !rv.target && (hcol != rv.seen_col || htype != rv.seen_type)) {
                rv.seen_col = hcol;
                rv.seen_type = htype;
                rv.seen_dodge = rnd() % 4 != 0;
                if (rv.seen_dodge) {
                    /* the nearest clear height, above or below */
                    for (int d = 8; d <= 96 && !rv.target; d += 8)
                        for (int sgn = -1; sgn <= 1 && !rv.target; sgn += 2) {
                            int ty = y + sgn * d, clear = ty >= 4 && ty <= 0x70;
                            for (int k = 0; k <= LOOK_AHEAD && clear; k++)
                                if (hit_at(40 * k, ty, 0, &hcol, &htype)) clear = 0;
                            if (clear) rv.target = ty;
                        }
                }
            }
            int goal = rv.target ? rv.target : CRUISE, ny = y;
            if (y < goal) ny += y + 2 <= goal ? 2 : 1;
            else if (y > goal) ny -= y - 2 >= goal ? 2 : 1;
            /* back to its height only when the way there is clear (not
             * into the obstacle it has just dodged) */
            if (!rv.target && ny != y)
                for (int k = 0; k <= 2 * LOOK_AHEAD; k++)
                    if (hit_at(20 * k, ny, 0, &hcol, &htype)) { ny = y; break; }
            y = ny;
            if (rv.target && y == rv.target && !threat) rv.target = 0;
        }
    }
    if (!landing) ww(R.ds, RIVAL_OBJ + 2, (uint16_t)y);
}

/* 0C17:1139, the rival is about to be drawn (AX = its object): while it
 * spins, its turning frame */
void hook_rival_draw_before(void)
{
    if (rival_probe || !rival_on() || R.ax != RIVAL_OBJ) return;
    uint32_t lin = LIN(rw(R.ds, RIVAL_OBJ + 0xE), rw(R.ds, RIVAL_OBJ + 0xC));
    if (!rv.made || (rv.source != lin && lin != (uint32_t)TURN_SEG << 4)) make_frames();
    if (!rv.spin) return;
    for (int i = 0; i < 0x14; i++) rv.obj[i] = (uint8_t)rb(R.ds, RIVAL_OBJ + i);
    rv.patched = 1;
    ww(R.ds, RIVAL_OBJ + 4, SIZE / 8);                        /* a strip like the player's */
    ww(R.ds, RIVAL_OBJ + 6, SIZE);
    ww(R.ds, RIVAL_OBJ + 8, FRAMES * SIZE / 8);
    ww(R.ds, RIVAL_OBJ + 0xA, rw(R.ds, PLAYER_OBJ + 0xA));
    ww(R.ds, RIVAL_OBJ + 0x10, FRAMES * SIZE / 8 * SIZE);
    ww(R.ds, RIVAL_OBJ + 0xC, 0);
    ww(R.ds, RIVAL_OBJ + 0xE, TURN_SEG);
    ww(R.ds, RIVAL_OBJ + 2, (uint16_t)((int16_t)rw(R.ds, RIVAL_OBJ + 2) - LIFT));
    wb(R.ds, RIVAL_OBJ + 0x12, (uint8_t)(rv.spin % FRAMES));
}

/* 0C17:113E, after the drawing (passed for every column drawn): the
 * rival's own object back */
void hook_rival_draw_after(void)
{
    if (rival_probe || !rv.patched) return;
    rv.patched = 0;
    for (int i = 0; i < 0x14; i++) wb(R.ds, RIVAL_OBJ + i, rv.obj[i]);
}
