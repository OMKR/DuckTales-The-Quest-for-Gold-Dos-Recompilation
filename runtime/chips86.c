/* PC hardware: PIC, PIT (timer IRQ, PC speaker), keyboard controller,
 * joystick port, Tandy sound chip; frame clock and SDL input/audio. */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <string.h>

#include "cpu86.h"
#include "dos86.h"
#include "settings86.h"
#include "layout86.h"
#include "amigasnd.h"

int poll_count = 2000;
uint32_t poll_pc;
static int headless;

/* ---- PIT -------------------------------------------------------------------------- */
static uint32_t pit_div[3] = {65536, 65536, 65536};
static int pit_latch_lo[3], pit_lo_byte[3];
static uint32_t pit_lo[3];
static uint8_t port61;
static void tandy_write(uint8_t v);
static uint32_t ticks;            /* BIOS 18.2 Hz tick count (approximate) */

uint32_t bios_ticks(void) { return rw(0x40, 0x6C) | (rw(0x40, 0x6E) << 16); }

/* ---- keyboard -------------------------------------------------------------------------- */
/* Keyboard: SDL key events become XT scancodes in a queue; IRQ1 (INT 9)
 * is raised for each. Without a game handler the BIOS INT 9 (here)
 * reads port 60h and stores ASCII|scan<<8 in the BIOS keyboard buffer at
 * 0040:001E-003D (head 001A, tail 001C), which INT 16h, DOS and the game
 * itself read. */
static uint8_t scan_queue[64];
static uint8_t ascii_queue[64];
static int scan_head, scan_tail;
static uint8_t cur_scan, cur_ascii;

static const struct { SDL_Scancode sc; uint8_t xt; uint8_t ascii; } keymap[] = {
    {SDL_SCANCODE_ESCAPE, 0x01, 27}, {SDL_SCANCODE_1, 0x02, '1'}, {SDL_SCANCODE_2, 0x03, '2'},
    {SDL_SCANCODE_3, 0x04, '3'}, {SDL_SCANCODE_4, 0x05, '4'}, {SDL_SCANCODE_5, 0x06, '5'},
    {SDL_SCANCODE_6, 0x07, '6'}, {SDL_SCANCODE_7, 0x08, '7'}, {SDL_SCANCODE_8, 0x09, '8'},
    {SDL_SCANCODE_9, 0x0A, '9'}, {SDL_SCANCODE_0, 0x0B, '0'}, {SDL_SCANCODE_BACKSPACE, 0x0E, 8},
    {SDL_SCANCODE_TAB, 0x0F, 9}, {SDL_SCANCODE_Q, 0x10, 'q'}, {SDL_SCANCODE_W, 0x11, 'w'},
    {SDL_SCANCODE_E, 0x12, 'e'}, {SDL_SCANCODE_R, 0x13, 'r'}, {SDL_SCANCODE_T, 0x14, 't'},
    {SDL_SCANCODE_Y, 0x15, 'y'}, {SDL_SCANCODE_U, 0x16, 'u'}, {SDL_SCANCODE_I, 0x17, 'i'},
    {SDL_SCANCODE_O, 0x18, 'o'}, {SDL_SCANCODE_P, 0x19, 'p'}, {SDL_SCANCODE_RETURN, 0x1C, 13},
    {SDL_SCANCODE_LCTRL, 0x1D, 0}, {SDL_SCANCODE_A, 0x1E, 'a'}, {SDL_SCANCODE_S, 0x1F, 's'},
    {SDL_SCANCODE_D, 0x20, 'd'}, {SDL_SCANCODE_F, 0x21, 'f'}, {SDL_SCANCODE_G, 0x22, 'g'},
    {SDL_SCANCODE_H, 0x23, 'h'}, {SDL_SCANCODE_J, 0x24, 'j'}, {SDL_SCANCODE_K, 0x25, 'k'},
    {SDL_SCANCODE_L, 0x26, 'l'}, {SDL_SCANCODE_LSHIFT, 0x2A, 0}, {SDL_SCANCODE_Z, 0x2C, 'z'},
    {SDL_SCANCODE_X, 0x2D, 'x'}, {SDL_SCANCODE_C, 0x2E, 'c'}, {SDL_SCANCODE_V, 0x2F, 'v'},
    {SDL_SCANCODE_B, 0x30, 'b'}, {SDL_SCANCODE_N, 0x31, 'n'}, {SDL_SCANCODE_M, 0x32, 'm'},
    {SDL_SCANCODE_RSHIFT, 0x36, 0}, {SDL_SCANCODE_LALT, 0x38, 0}, {SDL_SCANCODE_SPACE, 0x39, ' '},
    {SDL_SCANCODE_F1, 0x3B, 0}, {SDL_SCANCODE_F2, 0x3C, 0}, {SDL_SCANCODE_F3, 0x3D, 0},
    {SDL_SCANCODE_F4, 0x3E, 0}, {SDL_SCANCODE_F5, 0x3F, 0}, {SDL_SCANCODE_F6, 0x40, 0},
    {SDL_SCANCODE_F7, 0x41, 0}, {SDL_SCANCODE_F8, 0x42, 0}, {SDL_SCANCODE_F9, 0x43, 0},
    {SDL_SCANCODE_F10, 0x44, 0}, {SDL_SCANCODE_HOME, 0x47, 0}, {SDL_SCANCODE_UP, 0x48, 0},
    {SDL_SCANCODE_PAGEUP, 0x49, 0}, {SDL_SCANCODE_LEFT, 0x4B, 0}, {SDL_SCANCODE_RIGHT, 0x4D, 0},
    {SDL_SCANCODE_END, 0x4F, 0}, {SDL_SCANCODE_DOWN, 0x50, 0}, {SDL_SCANCODE_PAGEDOWN, 0x51, 0},
    {SDL_SCANCODE_KP_8, 0x48, '8'}, {SDL_SCANCODE_KP_2, 0x50, '2'}, {SDL_SCANCODE_KP_4, 0x4B, '4'},
    {SDL_SCANCODE_KP_6, 0x4D, '6'}, {SDL_SCANCODE_KP_ENTER, 0x1C, 13},
};

/* Typematic repeat: like a real keyboard (the BIOS default), the last key
 * held down sends its make code again after 500 ms and then 10.9 times a
 * second (the map cursor, for one, only moves on these repeats). Timed by
 * the clock, not by frames: the frame rate follows the fps and speed
 * settings, and a short press repeated at 100 fps. */
static uint8_t rep_xt, rep_ascii;
static uint32_t rep_start, rep_sent;
static uint32_t frame_counter;
static uint32_t now_ms(void) { return headless ? frame_counter * 1000u / 60 : SDL_GetTicks(); }

static void key_queue(uint8_t xt, uint8_t ascii, int down)
{
    if ((scan_tail + 1) % 64 == scan_head) return;
    scan_queue[scan_tail] = xt | (down ? 0 : 0x80);
    ascii_queue[scan_tail] = down ? ascii : 0;
    scan_tail = (scan_tail + 1) % 64;
}

static void key_event(uint8_t xt, uint8_t ascii, int down)
{
    key_queue(xt, ascii, down);
    if (down) { rep_xt = xt; rep_ascii = ascii; rep_start = now_ms(); rep_sent = 0; }
    else if (xt == rep_xt) rep_xt = 0;
}

/* called once per frame */
static void key_repeat(void)
{
    if (!rep_xt) return;
    uint32_t t = now_ms() - rep_start;
    if (t < 500) return;
    uint32_t due = 1 + (t - 500) * 109 / 10000;   /* repeats due by now */
    if (rep_sent < due) {
        key_queue(rep_xt, rep_ascii, 1);
        rep_sent = due;
    }
}

/* BIOS INT 9: called for IRQ1 when the game has no handler of its own */
void bios_keyboard_irq(void)
{
    uint32_t scan = port_in(0x60, 1);
    uint32_t ascii = cur_ascii;
    port_out(0x61, port_in(0x61, 1) | 0x80, 1);       /* acknowledge */
    port_out(0x61, port_in(0x61, 1) & 0x7F, 1);
    if (scan & 0x80) return;
    uint32_t head = rw(0x40, 0x1A), tail = rw(0x40, 0x1C);
    uint32_t next = tail + 2 >= 0x3E ? 0x1E : tail + 2;
    if (next == head) return;                          /* buffer full */
    ww(0x40, tail, ascii | (scan << 8));
    ww(0x40, 0x1C, next);
}

static void run_script(uint32_t frame);
static void pump_events(void);

/* mode 1: wait and take, 0: take if any, -1: peek */
int key_get(int mode)
{
    for (;;) {
        uint32_t head = rw(0x40, 0x1A), tail = rw(0x40, 0x1C);
        if (head != tail) {
            uint32_t k = rw(0x40, head);
            if (mode >= 0) ww(0x40, 0x1A, head + 2 >= 0x3E ? 0x1E : head + 2);
            return (int)k;
        }
        if (mode != 1) return -1;
        hle_poll();
        if (!headless) SDL_Delay(1);
    }
}

/* ---- joystick port --------------------------------------------------------------------- */
/* PC analog joystick at port 201h. A write starts the timers; each axis bit
 * stays 1 for a time that grows with the stick position, and the game
 * counts its reads of the port until the bit drops (040E:0E5F). Here the
 * time is measured in reads, so the result does not depend on the speed of
 * the host: centre JOY_MID reads, the ends JOY_MIN / JOY_MAX. The game's
 * own calibration is replaced with matching limits (runtime/hooks86.c).
 * The stick is the game controller (pure joystick logic: left stick or
 * d-pad, any face button = fire), or with keyboard_mode the arrow keys and
 * Space/Ctrl. */
static int joy_enabled;
static uint32_t joy_reads;
static int joy_x, joy_y;               /* -1000 .. 1000 */
static int joy_fire1, joy_fire2;
/* a press stays visible until the game has read it once: menus read the
 * port only a few times a second */
static int joy_latch1, joy_latch2, joy_latch_seen;
/* In the menus a controller button gives one press: the game sees it for
 * one measurement of the port (from a write to 201h to the next) and not
 * again until it is released. Menus act on every read with the button
 * down, so holding it chose several investments at once. 0 = up,
 * 1 = new press, 2 = seen in this measurement, 3 = used. */
static int pulse1;
int menu_mode(void);
static int kb_left, kb_right, kb_up, kb_down, kb_fire;
static SDL_GameController *pad;

static void joy_update(void)
{
    int x = 0, y = 0, f1 = 0, f2 = 0;
    if (pad) {
        int dz = 32767 * settings.deadzone / 100;
        int ax = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
        int ay = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
        if (ax > dz || ax < -dz) x = ax * 1000 / 32767;
        if (ay > dz || ay < -dz) y = ay * 1000 / 32767;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) x = -1000;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) x = 1000;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP)) y = -1000;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) y = 1000;
        f1 = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A) ||
             SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_X);
        f2 = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B) ||
             SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_Y);
    }
    if (settings.keyboard_mode) {
        if (kb_left) x = -1000;
        if (kb_right) x = 1000;
        if (kb_up) y = -1000;
        if (kb_down) y = 1000;
        if (kb_fire) f1 = 1;
    }
    joy_x = x < -1000 ? -1000 : x > 1000 ? 1000 : x;
    joy_y = y < -1000 ? -1000 : y > 1000 ? 1000 : y;
    if (f1 && !joy_fire1) { joy_latch1 = 1; pulse1 = 1; }
    if (!f1) pulse1 = 0;
    if (f2 && !joy_fire2) joy_latch2 = 1;
    joy_fire1 = f1;
    joy_fire2 = f2;
}

static uint32_t joy_port_in(void)
{
    if (!joy_enabled) return 0xFF;
    uint32_t n = joy_reads++;
    uint32_t tx = (uint32_t)(JOY_MID + joy_x * (JOY_MAX - JOY_MID) / 1000);
    uint32_t ty = (uint32_t)(JOY_MID + joy_y * (JOY_MAX - JOY_MID) / 1000);
    int f1 = joy_fire1 || joy_latch1, f2 = joy_fire2 || joy_latch2;
    if (pad && menu_mode()) {
        if (pulse1 == 1) pulse1 = 2;
        f1 = pulse1 == 2 || (joy_latch1 && pulse1 != 3);
    }
    if (joy_latch1 || joy_latch2) joy_latch_seen = 1;
    return 0xC0 | (n < tx ? 1 : 0) | (n < ty ? 2 : 0) | (f1 ? 0 : 0x10) | (f2 ? 0 : 0x20);
}

/* ---- ports -------------------------------------------------------------------------------- */
uint32_t vga_port_in(uint32_t port);
void vga_port_out(uint32_t port, uint32_t v);

uint32_t port_in(uint32_t port, int size)
{
    if (size == 2) return port_in(port, 1) | (port_in(port + 1, 1) << 8);
    port &= 0xFFFF;
    switch (port) {
    case 0x21: return 0;
    case 0x40: case 0x41: case 0x42: {
        int c = port - 0x40;
        uint32_t v = (pit_div[c] - (SDL_GetTicks() * 1193 % pit_div[c])) & 0xFFFF;
        if (!pit_lo_byte[c]) { pit_lo_byte[c] = 1; return v & 0xFF; }
        pit_lo_byte[c] = 0;
        return v >> 8;
    }
    case 0x60: return cur_scan;
    case 0x61: return port61;
    case 0x201: return joy_port_in();
    }
    if (port >= 0x3B0 && port < 0x3E0) return vga_port_in(port);
    return 0xFF;
}

void port_out(uint32_t port, uint32_t v, int size)
{
    if (size == 2) { port_out(port, v & 0xFF, 1); port_out(port + 1, v >> 8, 1); return; }
    port &= 0xFFFF;
    v &= 0xFF;
    switch (port) {
    case 0x20: case 0x21: return;
    case 0x43: {
        int c = (v >> 6) & 3;
        if (c < 3) { pit_latch_lo[c] = 0; pit_lo_byte[c] = 0; }
        return;
    }
    case 0x40: case 0x41: case 0x42: {
        int c = port - 0x40;
        if (!pit_latch_lo[c]) { pit_lo[c] = v; pit_latch_lo[c] = 1; }
        else {
            pit_div[c] = pit_lo[c] | (v << 8);
            if (!pit_div[c]) pit_div[c] = 65536;
            pit_latch_lo[c] = 0;
        }
        return;
    }
    case 0x61: port61 = (uint8_t)v; return;
    case 0x201: joy_reads = 0;
        if (pulse1 == 2) pulse1 = 3;             /* the press has been seen */
        if (joy_latch_seen) joy_latch1 = joy_latch2 = joy_latch_seen = 0;
        return;
    case 0xC0: tandy_write((uint8_t)v); return;    /* Tandy sound chip */
    }
    if (port >= 0x3B0 && port < 0x3E0) vga_port_out(port, v);
}

/* ---- timing ---------------------------------------------------------------------------------- */
static uint64_t perf_freq, irq0_next, frame_next;

/* ---- speed keys -------------------------------------------------------------------------- */
/* As in the Amiga port: PgUp/PgDn (or keypad +/-) change the game speed,
 * with Ctrl the music speed, with Alt the frame rate limit; steps of 5,
 * 1 with Shift. The values are shown in the window title. F11 toggles
 * fullscreen, F12 saves a screenshot (screenshot_001.bmp, _002, ...: the
 * first number not in use, so earlier ones are kept). */
static void save_screenshot(void)
{
    char name[32];
    for (int n = 1; n < 10000; n++) {
        snprintf(name, sizeof name, "screenshot_%03d.bmp", n);
        FILE *f = fopen(name, "rb");
        if (!f) { video_save_bmp(name); return; }
        fclose(f);
    }
}

static void update_title(void)
{
    char t[160];
    snprintf(t, sizeof t, "DuckTales: The Quest for Gold (DOS) - speed %d%%  music %d%%  %d fps",
             settings.speed, settings.music, settings.fps);
    video_set_title(t);
}

static int speed_key(const SDL_KeyboardEvent *k)
{
    SDL_Scancode sc = k->keysym.scancode;
    int down = k->type == SDL_KEYDOWN;
    if (sc == SDL_SCANCODE_F11) { if (down) video_toggle_fullscreen(); return 1; }
    if (sc == SDL_SCANCODE_F12) { if (down) save_screenshot(); return 1; }
    int dir = sc == SDL_SCANCODE_PAGEUP || sc == SDL_SCANCODE_KP_PLUS ? 1
            : sc == SDL_SCANCODE_PAGEDOWN || sc == SDL_SCANCODE_KP_MINUS ? -1 : 0;
    if (!dir) return 0;
    if (!down) return 1;
    int step = (k->keysym.mod & KMOD_SHIFT) ? 1 : 5;
    if (k->keysym.mod & KMOD_CTRL) settings.music += dir * step;
    else if (k->keysym.mod & KMOD_ALT) settings.fps += dir * step;
    else settings.speed += dir * step;
    settings_clamp(&settings);
    update_title();
    return 1;
}

static void pump_events(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) exit(0);
        if ((e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) && speed_key(&e.key)) continue;
        if (e.type == SDL_CONTROLLERDEVICEADDED && !pad) pad = SDL_GameControllerOpen(e.cdevice.which);
        /* The controller is purely the PC joystick; the shoulder buttons only
         * adjust the game speed (as in the Amiga port), with Back held the
         * music speed. */
        if (e.type == SDL_CONTROLLERBUTTONDOWN &&
            (e.cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER ||
             e.cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) {
            int dir = e.cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER ? 5 : -5;
            SDL_GameController *c = SDL_GameControllerFromInstanceID(e.cbutton.which);
            if (c && SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK)) settings.music += dir;
            else settings.speed += dir;
            settings_clamp(&settings);
            update_title();
            continue;
        }
        if (e.type == SDL_CONTROLLERDEVICEREMOVED && pad &&
            e.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad))) {
            SDL_GameControllerClose(pad);
            pad = NULL;
        }
        if ((e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) && !e.key.repeat &&
            joy_enabled && settings.keyboard_mode) {
            /* keyboard mode: these keys are the joystick, not keys */
            int down = e.type == SDL_KEYDOWN, *k = NULL;
            switch (e.key.keysym.scancode) {
            case SDL_SCANCODE_LEFT: k = &kb_left; break;
            case SDL_SCANCODE_RIGHT: k = &kb_right; break;
            case SDL_SCANCODE_UP: k = &kb_up; break;
            case SDL_SCANCODE_DOWN: k = &kb_down; break;
            case SDL_SCANCODE_SPACE: case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: k = &kb_fire; break;
            default: break;
            }
            if (k) { *k = down; continue; }
        }
        if ((e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) && !e.key.repeat) {
            for (size_t i = 0; i < sizeof keymap / sizeof keymap[0]; i++)
                if (keymap[i].sc == e.key.keysym.scancode) {
                    key_event(keymap[i].xt, keymap[i].ascii, e.type == SDL_KEYDOWN);
                    break;
                }
        }
    }
}

/* time of the last scancode given to the game, in ms (headless: from the
 * frame count) */
static uint32_t key_time = 0x80000000u;
/* key presses given to the game, and their count when its key reader
 * (040E:19C5) last returned a key */
static uint32_t makes_given, makes_read;

int keyboard_recent(uint32_t ms)
{
    uint32_t now = headless ? frame_counter * 1000u / 60 : SDL_GetTicks();
    return now - key_time < ms;
}

int keyboard_unread(void) { return makes_given != makes_read; }
void keyboard_read(void) { makes_read = makes_given; }

/* one scancode per call: latch it in port 60h and raise IRQ1 */
static void deliver_keyboard(void)
{
    if (scan_head == scan_tail || !R.ifl) return;
    key_time = headless ? frame_counter * 1000u / 60 : SDL_GetTicks();
    cur_scan = scan_queue[scan_head];
    cur_ascii = ascii_queue[scan_head];
    scan_head = (scan_head + 1) % 64;
    if (!(cur_scan & 0x80)) makes_given++;
    raise_irq(9);
}

static void frame(void)
{
    frame_counter++;
    run_script(frame_counter);
    if (!headless) pump_events();
    joy_update();
    key_repeat();
    video_frame();
    static int retrace_acc;
    int retrace_pct(void);
    retrace_acc += retrace_pct();
    if (retrace_acc >= 100) { retrace_acc -= 100; video_vblank(); }
    if (getenv("DT_MAX_FRAMES") && frame_counter >= (uint32_t)atoi(getenv("DT_MAX_FRAMES"))) {
        extern int video_save_bmp(const char *);
        video_save_bmp("shot.bmp");
        fprintf(stderr, "stopping at frame %u, last loop %05X\n", frame_counter, poll_pc);
        ftrace_dump();
        { void vstat_dump(void); vstat_dump(); }
        exit(0);
    }
}

/* ---- sound: PC speaker and Tandy (SN76489) ----------------------------------------------- */
/* After every timer interrupt, the sound state (port 61h gates and PIT
 * channel 2 divisor; the Tandy chip's registers) is rendered for one PIT
 * channel 0 period into the SDL audio queue. The sound drivers change the
 * state only from that interrupt, so this follows them exactly. */
#define AUDIO_RATE 44100
#define TANDY_CLOCK 3579545.0
static SDL_AudioDeviceID audio_dev;
static double spk_phase, spk_carry, spk_dc;

static struct {
    uint16_t period[3];       /* 10-bit tone dividers */
    uint8_t att[4];           /* attenuation 0 (loud) - 15 (off) */
    uint8_t noise;            /* bit 2: white, bits 0-1: rate */
    int latch;                /* register selected by the last latch byte */
    double phase[4];
    int out[4];
    uint16_t lfsr;
} sn = {{0}, {15, 15, 15, 15}, 0, 0, {0}, {1, 1, 1, 1}, 0x4000};

static void tandy_write(uint8_t v)
{
    if (v & 0x80) {
        sn.latch = (v >> 4) & 7;
        int ch = sn.latch >> 1;
        if (sn.latch & 1) sn.att[ch] = v & 15;
        else if (ch < 3) sn.period[ch] = (uint16_t)((sn.period[ch] & 0x3F0) | (v & 15));
        else { sn.noise = v & 7; sn.lfsr = 0x4000; }
    } else {
        int ch = sn.latch >> 1;
        if (sn.latch & 1) sn.att[ch] = v & 15;
        else if (ch < 3) sn.period[ch] = (uint16_t)((sn.period[ch] & 15) | ((v & 0x3F) << 4));
    }
}

static double tandy_sample(void)
{
    static const double vol[16] = {1.0, .794, .631, .501, .398, .316, .251, .200,
                                   .158, .126, .100, .079, .063, .050, .040, 0};
    double mix = 0;
    for (int ch = 0; ch < 3; ch++) {
        double f = sn.period[ch] > 1 ? TANDY_CLOCK / 32 / sn.period[ch] : 0;
        if (f > 0 && f < AUDIO_RATE / 2) {
            sn.phase[ch] += f * 2 / AUDIO_RATE;    /* output flips twice per period */
            while (sn.phase[ch] >= 1) { sn.phase[ch] -= 1; sn.out[ch] = !sn.out[ch]; }
            mix += (sn.out[ch] ? 1 : -1) * vol[sn.att[ch]];
        }
    }
    int r = sn.noise & 3;
    double nf = r == 3 ? (sn.period[2] ? TANDY_CLOCK / 32 / sn.period[2] : 0)
                       : TANDY_CLOCK / 32 / (16 << r);
    sn.phase[3] += nf / AUDIO_RATE;
    while (sn.phase[3] >= 1) {
        sn.phase[3] -= 1;
        int fb = (sn.noise & 4) ? ((sn.lfsr ^ (sn.lfsr >> 1)) & 1) : (sn.lfsr & 1);
        sn.lfsr = (uint16_t)((sn.lfsr >> 1) | (fb << 14));
        sn.out[3] = sn.lfsr & 1;
    }
    mix += (sn.out[3] ? 1 : -1) * vol[sn.att[3]] * 0.7;
    return mix / 3;
}

static FILE *wav_out;     /* DT_WAV=file: raw 16-bit mono capture (testing) */

static void sound_tick(void)
{
    if (!audio_dev && !wav_out) return;
    if (audio_dev && SDL_GetQueuedAudioSize(audio_dev) > AUDIO_RATE / 5 * sizeof(int16_t)) return;
    spk_carry += (double)AUDIO_RATE * pit_div[0] / 1193182.0 * 100 / settings.music;
    int n = (int)spk_carry;
    spk_carry -= n;
    int16_t buf[1024];
    if (n > 1024) n = 1024;
    if (amigasnd_music()) {           /* the DOS driver runs silenced */
        int32_t mix[1024] = {0};
        amigasnd_render(mix, n, AUDIO_RATE, settings.music);
        for (int i = 0; i < n; i++) {
            int32_t o = mix[i] * settings.volume / 150;
            buf[i] = (int16_t)(o > 32767 ? 32767 : o < -32768 ? -32768 : o);
        }
        if (audio_dev) SDL_QueueAudio(audio_dev, buf, (Uint32)(n * sizeof(int16_t)));
        if (wav_out) fwrite(buf, sizeof(int16_t), (size_t)n, wav_out);
        return;
    }
    double freq = 1193182.0 / pit_div[2];
    int tone = (port61 & 3) == 3 && freq < 12000;
    int level = (port61 & 3) == 2;            /* speaker pulled by the data bit alone */
    double gain = 9000.0 * settings.volume / 100;
    int32_t voice[1024] = {0};               /* the Amiga voices over the DOS sound */
    if (amigasnd_active()) amigasnd_render(voice, n, AUDIO_RATE, settings.music);
    for (int i = 0; i < n; i++) {
        double v = 0;
        if (tone) {
            spk_phase += freq / AUDIO_RATE;
            spk_phase -= (int)spk_phase;
            v = spk_phase < 0.5 ? 0.5 : -0.5;
        } else if (level) v = 0.5;
        v += tandy_sample();
        spk_dc += (v - spk_dc) * 0.002;   /* DC blocker */
        double o = (v - spk_dc) * gain + voice[i] * settings.volume / 150.0;
        buf[i] = (int16_t)(o > 32767 ? 32767 : o < -32768 ? -32768 : o);
    }
    if (audio_dev) SDL_QueueAudio(audio_dev, buf, (Uint32)(n * sizeof(int16_t)));
    if (wav_out) fwrite(buf, sizeof(int16_t), (size_t)n, wav_out);
}

static void audio_init(void)
{
    if (getenv("DT_WAV")) wav_out = fopen(getenv("DT_WAV"), "wb");
    amigasnd_set_speech(settings.speech);
    if (settings.sound == SOUND_AMIGA && !amigasnd_init(settings.amiga_dir))
        settings.sound = SOUND_TANDY;
    /* the Amiga voices with the DOS sound too (needs the Amiga game) */
    else if ((settings.sound == SOUND_TANDY || settings.sound == SOUND_SPEAKER) && settings.speech &&
             settings.amiga_dir[0]) {
        amigasnd_voices_only(1);
        if (!amigasnd_init(settings.amiga_dir)) amigasnd_voices_only(0);
    }
    if (settings.sound == SOUND_OFF || headless) return;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return;
    SDL_AudioSpec want = {0}, have;
    want.freq = AUDIO_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 1024;
    audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (audio_dev) SDL_PauseAudioDevice(audio_dev, 0);
}

/* Game speed. The sound driver's timer interrupt counts the game's ticks
 * (DS:2225, 32 bits; every 12th interrupt at the 874 Hz the driver sets, so
 * 72.8 Hz) and the game paces itself by them. That interrupt also plays the
 * music, so its rate is the music speed; the tick count is set here from
 * the clock and the game speed instead, which keeps the two apart. */
#define GAME_TICK_HZ (1193182.0 / 1365 / 12)
int tick_pct(void);
static void game_clock(uint64_t now)
{
    static uint64_t last_time;
    static uint32_t written;
    static double acc;
    uint32_t cur = rw(LOAD_SEG + 0x120E, 0x2225) | (uint32_t)rw(LOAD_SEG + 0x120E, 0x2227) << 16;
    if (!last_time) { last_time = now; written = cur; return; }
    acc += (double)(now - last_time) / perf_freq * GAME_TICK_HZ * settings.speed / 100 * tick_pct() / 100;
    last_time = now;
    uint32_t inc = (uint32_t)acc;
    acc -= inc;
    /* the driver's own increments since the last write are taken back; a
     * value the game itself stored is kept as the new base */
    uint32_t base = cur - written <= 16 ? written : cur;
    written = base + inc;
    ww(LOAD_SEG + 0x120E, 0x2225, written & 0xFFFF);
    ww(LOAD_SEG + 0x120E, 0x2227, written >> 16);
}

/* Let n frames pass (driver functions with waits done in C). */
void wait_frames(int n)
{
    uint32_t end = frame_counter + (uint32_t)n;
    while ((int32_t)(frame_counter - end) < 0) {
        hle_poll();
        poll_count = 2000;
        if (!headless) SDL_Delay(1);
    }
}

void hle_poll(void)
{
    static int in_poll;
    poll_count = 2000;
    if (in_poll) return;
    in_poll = 1;
    if (headless) {
        static int n;
        if (++n >= 30) {      /* one IRQ0 per 30 polls, a frame per 4 */
            n = 0;
            raise_irq(8);
            sound_tick();
            deliver_keyboard();
            if (++ticks % 4 == 0) frame();
        }
        in_poll = 0;
        return;
    }
    uint64_t now = SDL_GetPerformanceCounter();
    uint64_t irq_period = perf_freq * pit_div[0] * 100 / ((uint64_t)1193182 * settings.music);
    int guard = 0;
    while (now >= irq0_next && guard++ < 8) {
        irq0_next += irq_period;
        raise_irq(8);
        sound_tick();
    }
    game_clock(now);
    if (now > irq0_next + irq_period * 8) irq0_next = now + irq_period;
    deliver_keyboard();
    if (now >= frame_next) {
        frame_next += perf_freq * 100 / ((uint64_t)settings.fps * (uint64_t)settings.speed);
        if (now > frame_next + perf_freq / 10) frame_next = now + perf_freq * 100 / ((uint64_t)settings.fps * (uint64_t)settings.speed);
        frame();
    }
    in_poll = 0;
}

/* ---- test script ---------------------------------------------------------------------------- */
static void run_script(uint32_t f)
{
    const char *s = getenv("DT_KEYS");
    if (!s) return;
    while (*s) {
        uint32_t at = (uint32_t)strtoul(s, (char **)&s, 10);
        if (*s++ != ':') return;
        char name[16] = {0};
        int n = 0;
        while (*s && *s != ',' && n < 15) name[n++] = *s++;
        if (*s == ',') s++;
        if (at != f) continue;
        int down = 1, up = 1;
        size_t l = strlen(name);
        if (l > 1 && name[l - 1] == '+') { up = 0; name[l - 1] = 0; }
        else if (l > 1 && name[l - 1] == '-') { down = 0; name[l - 1] = 0; }
        uint8_t xt = 0, ascii = 0;
        /* jleft/jright/jup/jdown/jfire: the joystick (as keyboard mode) */
        int *jk = !strcmp(name, "jleft") ? &kb_left : !strcmp(name, "jright") ? &kb_right
                : !strcmp(name, "jup") ? &kb_up : !strcmp(name, "jdown") ? &kb_down
                : !strcmp(name, "jfire") ? &kb_fire : NULL;
        if (jk) { *jk = down && !up ? 1 : !down ? 0 : *jk; continue; }
        if (!strcmp(name, "ret")) { xt = 0x1C; ascii = 13; }
        else if (!strcmp(name, "space")) { xt = 0x39; ascii = ' '; }
        else if (!strcmp(name, "esc")) { xt = 0x01; ascii = 27; }
        else if (!strcmp(name, "up")) xt = 0x48;
        else if (!strcmp(name, "down")) xt = 0x50;
        else if (!strcmp(name, "left")) xt = 0x4B;
        else if (!strcmp(name, "right")) xt = 0x4D;
        else if (!strcmp(name, "f12")) { save_screenshot(); continue; }
        else if (!strcmp(name, "shot")) { extern int video_save_bmp(const char *); char p[32];
            snprintf(p, sizeof p, "shot_%u.bmp", f); video_save_bmp(p); continue; }
        else if (strlen(name) == 1) {
            ascii = (uint8_t)name[0];
            for (size_t i = 0; i < sizeof keymap / sizeof keymap[0]; i++)
                if (keymap[i].ascii == ascii) xt = keymap[i].xt;
        }
        if (down) key_event(xt, ascii, 1);
        if (up) key_event(xt, ascii, 0);
    }
}

void chips_init(void)
{
    headless = getenv("DT_HEADLESS") != NULL;
    hle_trace = getenv("HLE_TRACE") != NULL;
    joy_enabled = settings.joystick;
    perf_freq = SDL_GetPerformanceFrequency();
    irq0_next = frame_next = SDL_GetPerformanceCounter();
    audio_init();
    if (!headless) update_title();
}
