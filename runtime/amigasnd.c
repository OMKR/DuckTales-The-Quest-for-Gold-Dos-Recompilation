/* Amiga sound for the DOS port.
 *
 * The Amiga version's root code (gen/amigasnd_gen.c, recompiled from the
 * user's DT1/DuckTales by tools/amiga/sndgen.py) runs here in its own 16 MB
 * Amiga memory: its sound engine (request 0x57D2, 50 Hz player 0x58BC,
 * audio interrupt handlers 0x55D6-0x5726) and the routines that build its
 * instruments from dt1.as / dt2.as (startup 0x4AF0, instrument set
 * 0x4D04). This file provides what they use: the hunk loader, Paula (the 4
 * audio channels and their interrupts), and the few exec/dos library calls
 * (memory, files). Sound numbers are the same as the DOS game's.
 *
 * Without the generated code (no Amiga game when the game was built) the
 * Amiga sound option is unavailable.
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "amigasnd.h"

#if !__has_include("amigasnd_layout.h")
int amigasnd_init(const char *dir)
{
    (void)dir;
    fprintf(stderr, "Amiga sound: the game was built without the Amiga game (rebuild it with the Amiga game folder set)\n");
    return 0;
}
int amigasnd_active(void) { return 0; }
void amigasnd_request(int n) { (void)n; }
void amigasnd_scene(int n) { (void)n; }
void amigasnd_overlay(int n) { (void)n; }
int amigasnd_speaking(void) { return 0; }
void amigasnd_voices_only(int on) { (void)on; }
int amigasnd_music(void) { return 0; }
void amigasnd_set_speech(int on) { (void)on; }
void amigasnd_speech(const int *sets, int n) { (void)sets; (void)n; }
void amigasnd_text(const char *text) { (void)text; }
void amigasnd_picture(const char *file, int entry) { (void)file; (void)entry; }
void amigasnd_event(int event) { (void)event; }
void amigasnd_render(int32_t *buf, int n, int rate, int music_pct) { (void)buf; (void)n; (void)rate; (void)music_pct; }
#else

#include "amigasnd_rt.h"
#include "amigasnd_layout.h"

CPU R;
uint8_t *mem;
int poll_count = 100000;
uint32_t poll_pc;
uint32_t ftrace_ring[64];
unsigned ftrace_pos;
uint32_t ftrace_break = 0xFFFFFFFFu;
int logfn_count;
uint32_t watch_lo = 0xFFFFFFFFu, watch_len;

#define EXEC_BASE 0x00F00000u
#define DOS_BASE 0x00F10000u
#define OTHER_BASE 0x00F20000u
#define RET_ADDR 0x00FFFFF8u     /* return address of calls made from C */
#define STACK_TOP 0x000FFFF0u
#define HEAP_START 0x00100000u
#define HEAP_END 0x00E00000u
#define PAL_CLOCK 3546895.0

static int active;
static char amiga_dir[1024];
static uint32_t heap = HEAP_START;
static uint32_t heap_sets;          /* start of the memory for instrument sets */
static int music_open;
static jmp_buf fail;                /* an error in the Amiga code turns the sound off */
static int depth;

/* ---- runtime services for the generated code ----------------------------------------- */
void rt_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "Amiga sound: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, " (sound off)\n");
    va_end(ap);
    active = 0;
    if (depth) longjmp(fail, 1);
    exit(1);
}

void watch_hit(uint32_t addr) { (void)addr; }
void hle_poll(void) { poll_count = 100000; }
void logfn_check(uint32_t addr) { (void)addr; }
void ftrace_dump(void) {}
void rt_program_exit(void) { rt_fatal("program exit"); }
int copy_protection_off(void) { return 0; }
int music_gate(void) { return music_open; }
void music_tick(void) {}
void amiga_overlay_call(uint32_t node_off) { rt_fatal("overlay call %x", node_off); }

static void (*find_function(uint32_t addr))(void)
{
    int lo = 0, hi = am_func_table_len - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (am_func_table[mid].addr == addr) return am_func_table[mid].fn;
        if (am_func_table[mid].addr < addr) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

void call_indirect_ret(uint32_t addr, uint32_t ret)
{
    void (*fn)(void) = find_function(addr);
    if (!fn) rt_fatal("call to unknown address %06x", addr);
    push32(ret);
    fn();
}

void call_indirect_tail(uint32_t addr)
{
    void (*fn)(void) = find_function(addr);
    if (!fn) rt_fatal("jump to unknown address %06x", addr);
    fn();
}

void call_indirect(uint32_t addr) { call_indirect_ret(addr, RET_ADDR); }

/* calls the function at hunk 0 + off with word arguments (pushed right to
 * left), keeping the registers; returns d0 */
static uint32_t call0(uint32_t off, int nargs, const uint32_t *args, int long_args)
{
    CPU saved = R;
    if (!depth++ && setjmp(fail)) {
        depth = 0;
        R = saved;
        return 0;
    }
    R.a[4] = AM_A4_VALUE;
    for (int i = nargs - 1; i >= 0; i--) {
        if (long_args) push32(args[i]);
        else { R.a[7] -= 2; ww(R.a[7], args[i]); }
    }
    call_indirect_ret(am_hunk_base[0] + off, RET_ADDR);
    uint32_t d0 = R.d[0];
    R = saved;
    depth--;
    return d0;
}

/* ---- Paula ------------------------------------------------------------------------- */
typedef struct {
    uint32_t pt;            /* current pointer */
    uint32_t left;          /* words left in the block */
    double pos;             /* position within the current word (0..2) */
    int on;
} Channel;

static Channel ch[4];
static uint16_t regs[0x100];
static uint16_t dmacon, intena, intreq;
static int in_interrupt, start_pending;
/* audio interrupt handlers (Paula channel 0-3), as SetIntVector installs */
static const uint32_t irq_handler[4] = {0x55D6, 0x5646, 0x56B6, 0x5726};

static uint32_t aud_lc(int i) { return ((uint32_t)regs[(0xA0 + 16 * i) / 2] << 16 | regs[(0xA2 + 16 * i) / 2]) & ADDR_MASK; }
static uint32_t aud_len(int i) { uint32_t n = regs[(0xA4 + 16 * i) / 2]; return n ? n : 0x10000; }

static void raise_irq(int bit)
{
    intreq |= (uint16_t)(1 << bit);
    if (in_interrupt || !(intena & 0x4000) || !(intena & (1 << bit))) return;
    in_interrupt = 1;
    call0(irq_handler[bit - 7], 0, NULL, 0);
    in_interrupt = 0;
}

/* Paula raises "block started" when DMA has fetched the first word, after
 * the program has gone on: delivered at the next safe point. */
static void service(void)
{
    int p = start_pending;
    start_pending = 0;
    for (int i = 0; i < 4; i++) if (p & (1 << i)) raise_irq(7 + i);
}

static void channel_start(int i)
{
    ch[i].pt = aud_lc(i);
    ch[i].left = aud_len(i);
    ch[i].pos = 0;
    ch[i].on = 1;
    start_pending |= 1 << i;
}

uint32_t hw_read(uint32_t addr, int size)
{
    uint32_t off = addr - 0xDFF000;
    uint16_t v = 0;
    if (addr >= 0xDFF000 && addr < 0xDFF200) {
        if (size == 4) return (hw_read(addr, 2) << 16) | hw_read(addr + 2, 2);
        switch (off & ~1u) {
        case 0x002: v = dmacon & 0x07FF; break;       /* DMACONR */
        case 0x01C: v = intena; break;                /* INTENAR */
        case 0x01E: v = intreq; break;                /* INTREQR */
        default: v = regs[(off & 0x1FE) / 2]; break;
        }
        if (size == 1) return (off & 1) ? (v & 0xFF) : (v >> 8);
        return v;
    }
    return 0;    /* CIA and others: not used by the sound code */
}

void hw_write(uint32_t addr, uint32_t v, int size)
{
    if (addr < 0xDFF000 || addr >= 0xDFF200) return;
    uint32_t off = addr - 0xDFF000;
    if (size == 4) { hw_write(addr, v >> 16, 2); hw_write(addr + 2, v & 0xFFFF, 2); return; }
    if (size == 1) v = (off & 1) ? v & 0xFF : (v & 0xFF) << 8;
    off &= ~1u;
    regs[off / 2] = (uint16_t)v;
    switch (off) {
    case 0x096: {                                       /* DMACON */
        uint16_t old = dmacon;
        if (v & 0x8000) dmacon |= v & 0x7FFF;
        else dmacon &= ~v;
        for (int i = 0; i < 4; i++) {
            int was = (old & 0x200) && (old & (1 << i)), now = (dmacon & 0x200) && (dmacon & (1 << i));
            if (now && !was) channel_start(i);
            if (!now) ch[i].on = 0;
        }
        break;
    }
    case 0x09A:                                         /* INTENA */
        if (v & 0x8000) intena |= v & 0x7FFF;
        else intena &= ~v;
        break;
    case 0x09C:                                         /* INTREQ */
        if (v & 0x8000) intreq |= v & 0x7FFF;
        else intreq &= ~v;
        break;
    }
}

/* ---- exec and dos ------------------------------------------------------------------- */
#define MAX_FILES 16
static FILE *files[MAX_FILES];

static uint32_t alloc(uint32_t size)
{
    size = (size + 7) & ~7u;
    if (size > HEAP_END - heap_sets) rt_fatal("AllocMem(%u)", size);
    if (heap + size > HEAP_END) heap = heap_sets;       /* never freed: wrap (sets are reloaded) */
    uint32_t a = heap;
    heap += size;
    memset(mem + a, 0, size);
    return a;
}

static void read_cstring(uint32_t a, char *out, size_t n)
{
    size_t i = 0;
    for (; i + 1 < n && mem[(a + i) & ADDR_MASK]; i++) out[i] = (char)mem[(a + i) & ADDR_MASK];
    out[i] = 0;
}

/* "DT1:dt1.as" -> <amiga_dir>/DT1/dt1.as */
static FILE *open_amiga(const char *name)
{
    char path[1600], vol[16] = "", file[256];
    const char *colon = strchr(name, ':');
    if (colon && colon - name < (long)sizeof vol) {
        memcpy(vol, name, (size_t)(colon - name));
        vol[colon - name] = 0;
        for (char *p = vol; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
        snprintf(file, sizeof file, "%s", colon + 1);
    } else snprintf(file, sizeof file, "%s", name);
    const char *disks[2] = {vol[0] ? vol : "DT1", "DT2"};
    for (int d = 0; d < 2; d++) {
        snprintf(path, sizeof path, "%s/%s/%s", amiga_dir, disks[d], file);
        FILE *f = fopen(path, "rb");
        if (f) return f;
    }
    return NULL;
}

void lib_call(uint32_t base, int32_t offset)
{
    service();
    if (base == EXEC_BASE) {
        switch (offset) {
        case -198: R.d[0] = alloc(R.d[0]); return;          /* AllocMem */
        case -186: R.d[0] = alloc(R.d[0]); return;          /* Allocate (from a pool) */
        case -210: case -192: return;                       /* FreeMem, Deallocate */
        case -120: case -126: case -132: case -138: return; /* Disable/Enable/Forbid/Permit */
        case -162: R.d[0] = 0; return;                      /* SetIntVector: handlers fixed */
        case -168: case -174: return;                       /* Add/RemIntServer */
        case -306: R.d[0] = 0; return;                      /* SetSignal */
        case -552: case -408: R.d[0] = OTHER_BASE; return;  /* OpenLibrary */
        case -414: return;                                  /* CloseLibrary */
        }
    } else if (base == DOS_BASE) {
        switch (offset) {
        case -30: {                                         /* Open(d1 name, d2 mode) */
            char name[256];
            read_cstring(R.d[1], name, sizeof name);
            int slot = 1;
            while (slot < MAX_FILES && files[slot]) slot++;
            FILE *f = slot < MAX_FILES ? open_amiga(name) : NULL;
            if (f) files[slot] = f;
            R.d[0] = f ? (uint32_t)slot : 0;
            return;
        }
        case -36:                                           /* Close */
            if (R.d[1] < MAX_FILES && files[R.d[1]]) { fclose(files[R.d[1]]); files[R.d[1]] = NULL; }
            R.d[0] = 1;
            return;
        case -42: {                                         /* Read(d1, d2 buf, d3 len) */
            FILE *f = R.d[1] < MAX_FILES ? files[R.d[1]] : NULL;
            uint32_t at = R.d[2] & ADDR_MASK, n = R.d[3];
            if (n > MEM_SIZE - at) n = MEM_SIZE - at;           /* stays in the Amiga memory */
            R.d[0] = f ? (uint32_t)fread(mem + at, 1, n, f) : (uint32_t)-1;
            return;
        }
        case -66: {                                         /* Seek(d1, d2 pos, d3 mode) */
            FILE *f = R.d[1] < MAX_FILES ? files[R.d[1]] : NULL;
            if (!f) { R.d[0] = (uint32_t)-1; return; }
            long old = ftell(f);
            int whence = (int32_t)R.d[3] == -1 ? SEEK_SET : (int32_t)R.d[3] == 1 ? SEEK_END : SEEK_CUR;
            R.d[0] = fseek(f, (int32_t)R.d[2], whence) == 0 ? (uint32_t)old : (uint32_t)-1;
            return;
        }
        case -132: R.d[0] = 0; return;                      /* IoErr */
        }
    }
    rt_fatal("library call %06x %d", base, offset);
}

/* ---- hunk loader ------------------------------------------------------------------- */
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static int load_program(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)len);
    if (!buf || fread(buf, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(buf); return 0; }
    fclose(f);
    long pos = 0;
    int hunk = 0, cur = -1;
    while (pos + 4 <= len) {
        uint32_t type = be32(buf + pos) & 0x3FFFFFFF;
        pos += 4;
        switch (type) {
        case 0x3F3: {
            while (be32(buf + pos)) pos += 4 + be32(buf + pos) * 4;
            pos += 4;
            uint32_t first = be32(buf + pos + 4), last = be32(buf + pos + 8);
            pos += 12 + (last - first + 1) * 4;
            hunk = (int)first;
            break;
        }
        case 0x3E9: case 0x3EA: {
            uint32_t n = be32(buf + pos) * 4;
            if (hunk >= AM_NUM_HUNKS) { free(buf); return 0; }
            memcpy(mem + am_hunk_base[hunk], buf + pos + 4, n);
            cur = hunk++;
            pos += 4 + n;
            break;
        }
        case 0x3EB: cur = hunk++; pos += 4; break;
        case 0x3EC:
            while (be32(buf + pos)) {
                uint32_t n = be32(buf + pos), target = be32(buf + pos + 4);
                for (uint32_t i = 0; i < n; i++) {
                    uint32_t at = am_hunk_base[cur] + be32(buf + pos + 8 + 4 * i);
                    wl(at, rl(at) + am_hunk_base[target]);
                }
                pos += 8 + n * 4;
            }
            pos += 4;
            break;
        case 0x3F0:
            while (be32(buf + pos)) pos += 4 + be32(buf + pos) * 4 + 4;
            pos += 4;
            break;
        case 0x3F1: pos += 4 + be32(buf + pos) * 4; break;
        case 0x3F2: case 0x3F6: break;
        case 0x3F5: pos += 4 + (be32(buf + pos) + 1) * 4; break;
        default: free(buf); return 0;
        }
    }
    free(buf);
    return 1;
}

/* ---- interface ------------------------------------------------------------------- */
#define DATA(off) (am_hunk_base[1] + (off))

/* The Amiga game loads each scene's instruments and voices (0x4D04(set),
 * lists at data 0x1F9E) when the scene starts and before each voice; the
 * DOS game has no such step. The scene is known from the DOS overlay that
 * runs it; the set is the one the Amiga version's code for that scene loads
 * at its start. The title, map and office (overlays 1, 2, 10) share one;
 * there the set follows the sound (the Amiga code loads set 0 before the
 * title music, set 1 before the office sounds 3-5 and 31). */
static const signed char overlay_set[16] = {-1, -1, -1, -1, 0x05, 0x04, 0x15, 0x0A, 0x08, 0x0C, -1, -1, -1, -1, -1, -1};
static const struct { int sound, set; } sound_sets[] = {{1, 0}, {3, 1}, {4, 1}, {5, 1}, {31, 1}};
/* the cave loads a voice before these sounds (Amiga node 8) */
static const struct { int sound, set; } cave_sets[] = {{0x47, 0x28}, {0x48, 0x16}, {0x49, 0x18}, {0x4B, 0x07}};
static int cur_set = -1;
static int speech_on = 1;
static int voices_only;             /* with Tandy / PC speaker: the Amiga voices alone */
static int cur_overlay;
static uint64_t treasure_shown;     /* when the treasure picture was loaded (+1) */
static int own_request;             /* amigasnd_request called by speak() */
static uint64_t played;             /* samples rendered: the clock of repeated triggers */

int amigasnd_active(void) { return active; }

int amigasnd_init(const char *dir)
{
    snprintf(amiga_dir, sizeof amiga_dir, "%s", dir);
    mem = calloc(1, MEM_SIZE);
    char path[1200];
    snprintf(path, sizeof path, "%s/DT1/DuckTales", dir);
    if (!mem || !load_program(path)) {
        fprintf(stderr, "Amiga sound: Amiga game not found in %s\n", dir);
        return 0;
    }
    R.a[7] = STACK_TOP;
    dmacon = 0x0200;                         /* DMA and interrupts master on, as the OS leaves them */
    intena = 0x4000;
    wl(4, EXEC_BASE);
    wl(AM_A4_VALUE + 0x33A4, EXEC_BASE);     /* SysBase as the program keeps it */
    wl(AM_A4_VALUE + 0x2990, DOS_BASE);      /* DOSBase */
    /* what the game's start-up sets up for the sound code */
    wl(DATA(0xB39A), am_hunk_base[0] + 0x6200);  /* sound table */
    wl(DATA(0x2B4A), AM_A4_VALUE);
    wl(DATA(0x5422), alloc(0x3E80));
    wl(DATA(0x5426), alloc(0x3E80));
    static const char *const as_files[2] = {"DT1:dt1.as", "DT2:dt2.as"};
    for (int i = 0; i < 2; i++) {
        uint32_t name = alloc(16);
        memcpy(mem + name, as_files[i], strlen(as_files[i]) + 1);
        R.d[1] = name;
        R.d[2] = 1005;
        lib_call(DOS_BASE, -30);
        if (!R.d[0]) { fprintf(stderr, "Amiga sound: %s not found\n", as_files[i]); return 0; }
        wl(DATA(0xA7CE + 4 * i), R.d[0]);
    }
    wl(DATA(0x46DA), rl(DATA(0xA7CE)));
    active = 1;
    call0(0x5868, 0, NULL, 0);             /* stop all, reset the channels */
    call0(0x4AF0, 0, NULL, 0);             /* start-up instruments */
    call0(0x5594, 0, NULL, 0);             /* audio interrupts on */
    heap_sets = heap;
    amigasnd_scene(0);
    return 1;
}

/* The Amiga game waits until a voice has been spoken (channel 0's sound
 * program, $2A16, ends) before it goes on; the DOS game goes on at once,
 * and its next sounds or scene would cut the voice off (a stop, or a new
 * set loaded over the streamed sample). While a voice plays, set loads and
 * requests for channel 0 wait and follow when it has ended. */
static int speaking, speak_ch;
static uint64_t speak_start;
static int pending_set = -1;
static int pending_req[8], npending;
static const int *pending_voice;
static int pending_voice_n, pending_voice_req;

static int speech_busy(void)
{
    if (!speaking) return 0;
    /* the voice's channel still runs its program ($2A12 + 0x4C per channel, +4) */
    if (rl(DATA(0x2A16 + 0x4C * (uint32_t)speak_ch)) && played - speak_start < 20 * 44100) return 1;
    speaking = 0;
    return 0;
}

static int sound_channel(int n)
{
    uint32_t desc = rl(rl(DATA(0xB39A)) + 4u * (uint32_t)(n & 0xFF));
    return n == 8 ? -1 : rb(desc);
}

static void load_set(int n)
{
    if (n == cur_set) return;
    uint32_t a = (uint32_t)n;
    call0(0x4D04, 1, &a, 0);
    cur_set = n;
    service();
}

int amigasnd_speaking(void) { return active && speech_busy(); }

void amigasnd_scene(int n)
{
    if (!active) return;
    if (speech_busy()) { pending_set = n; return; }
    load_set(n);
}

void amigasnd_overlay(int n)
{
    if (!active || n == cur_overlay) return;
    cur_overlay = n;
    if (n > 0 && n < 16 && overlay_set[n] >= 0) amigasnd_scene(overlay_set[n]);
}

/* A voice: one of the sets (each holding one voice sample), as the Amiga
 * game picks at random, then sound 0x77 plays it. A trigger seen again
 * within 3 s (a redrawn text) is ignored. */
void amigasnd_set_speech(int on) { speech_on = on; }
void amigasnd_voices_only(int on) { voices_only = on; }
int amigasnd_music(void) { return active && !voices_only; }

static void speak(const int *sets, int n, int req)
{
    static const int *last;
    static uint64_t last_time;
    if (!active || n <= 0 || !speech_on) return;
    if (sets == last && (speech_busy() || played - last_time < 3 * 44100)) return;
    if (speech_busy()) {
        pending_voice = sets;
        pending_voice_n = n;
        pending_voice_req = req;
        return;
    }
    last = sets;
    last_time = played;
    load_set(sets[rand() % n]);
    own_request = 1;
    amigasnd_request(req);
    own_request = 0;
}

void amigasnd_speech(const int *sets, int n) { speak(sets, n, 0x77); }

/* the voice has ended: what waited for it */
static void resume(void)
{
    int set = pending_set, q[8], nq = npending;
    const int *voice = pending_voice;
    pending_set = -1;
    pending_voice = NULL;
    memcpy(q, pending_req, sizeof q);
    npending = 0;
    if (set >= 0) load_set(set);
    if (voice) speak(voice, pending_voice_n, pending_voice_req);
    /* a stop that waited is dropped when sounds follow it (the voice has
     * ended already; the stop would hold the channel against the next) */
    for (int k = 0; k < nq; k++)
        if (q[k] != 0 || k == nq - 1) amigasnd_request(q[k]);
}

/* Voices tied to what the DOS game shows: the office's Launchpad (texts,
 * Amiga node 1 0x832) and the map's pictures (MAIN.ARC 36-39, node 2
 * 0x1A3A-0x1B86). the rival's taunts have a jingle (sound 0x6E). */
static const int launchpad1[] = {0x03}, launchpad2[] = {0x0D}, launchpad3[] = {0x0E};
static const int map_a[] = {0x1E}, map_b[] = {0x09, 0x11, 0x12}, map_c[] = {0x0F, 0x10};

/* The texts are known by the FNV-1a hash of the line (leading and
 * trailing spaces left out), so that the game's words are not copied here:
 * Launchpad's three greetings, and the two lines the rival's taunts start
 * with (played with his jingle, sound 0x6E). */
static uint32_t text_hash(const char *t)
{
    while (*t == ' ') t++;
    size_t n = strlen(t);
    while (n && t[n - 1] == ' ') n--;
    uint32_t h = 2166136261u;
    for (size_t k = 0; k < n; k++) h = (h ^ (uint8_t)t[k]) * 16777619u;
    return h;
}

void amigasnd_text(const char *text)
{
    static const struct { uint32_t hash; const int *sets; int n; } texts[] = {
        {0x01D06754u, launchpad1, 1}, {0x197AA05Cu, launchpad2, 1}, {0xC9C65BC9u, launchpad3, 1}};
    static const uint32_t taunts[] = {0x1C95D0F2u, 0x0B7C42B4u};
    static uint64_t jingle_time;
    if (!active) return;
    uint32_t h = text_hash(text);
    for (size_t k = 0; k < sizeof texts / sizeof texts[0]; k++)
        if (h == texts[k].hash) amigasnd_speech(texts[k].sets, texts[k].n);
    for (size_t k = 0; k < sizeof taunts / sizeof taunts[0]; k++)
        if (h == taunts[k] && played - jingle_time > 3 * 44100) {
            jingle_time = played;
            amigasnd_request(0x6E);
        }
}

void amigasnd_picture(const char *file, int entry)
{
    if (!active) return;
    for (const char *p = file; *p; p++) if (*p == '\\' || *p == '/' || *p == ':') file = p + 1;
    const char *a = "MAIN.ARC", *b = file;
    while (*a && (*b == *a || *b == *a + 32)) { a++; b++; }
    if (*a || *b) return;
    static const int campfire[] = {0x19};    /* node 0 0x33E: the Junior Woodchucks' camp */
    if (entry == 31) speak(campfire, 1, 0x71);
    else if (entry == 35) {                     /* the treasure (node 1 0xD62) */
        static const int t1[] = {0x0B}, t2[] = {0x18};
        /* one comment per treasure: the game loads the picture more than
         * once, and each draw would pick another one */
        static uint64_t treasure_time;
        static int treasure_seen;
        if (treasure_seen && played - treasure_time < 20 * 44100) return;
        treasure_seen = 1;
        treasure_time = played;
        treasure_shown = played + 1;
        int k = rand() % 3;
        if (k == 2) speak(t2, 1, 0x49);
        else speak(t1, 1, k ? 0x27 : 0x77);
    }
    else if (entry == 36 || entry == 37) amigasnd_speech(map_a, 1);
    else if (entry == 39) amigasnd_speech(map_b, 3);
    else if (entry == 38) amigasnd_speech(map_c, 2);
}

/* The plane's landing (Amiga node 3 0x1250) and the scales' result (node 4
 * 0xC02: the player's money above the rival's, or not). */
void amigasnd_event(int event)
{
    static const int landed[] = {0x06, 0x13, 0x14};
    static const int won[] = {0x1A, 0x1F, 0x20}, lost[] = {0x1D, 0x21, 0x23, 0x24, 0x25, 0x26};
    if (event == AMIGASND_LANDED) amigasnd_speech(landed, 3);
    else if (event == AMIGASND_SCALES_WON) amigasnd_speech(won, 3);
    else if (event == AMIGASND_SCALES_LOST) amigasnd_speech(lost, 6);
}

void amigasnd_request(int n)
{
    if (!active) return;
    /* voices: the sample of a set loaded just before (the cave's ending:
     * 0x47, 0x48, 0x49, 0x4B; the treasure: 0x27, 0x49) */
    /* the DOS treasure screen's own sound 0x49 (root 0C3D, after the
     * picture) is its fanfare; in the Amiga engine 0x49 is the cave's
     * "Wow, look at that", which played over the treasure's comment */
    if (n == 0x49 && !own_request && treasure_shown && played + 1 - treasure_shown < 15 * 44100) return;
    int voice = n == 0x77 || n == 0x71 || n == 0x27 || n == 0x47 || n == 0x48 || n == 0x49 || n == 0x4B;
    if (voice && !speech_on) return;      /* also the game's own requests */
    /* voices only (DOS music and effects): the game's other sounds are not
     * played here; a voice that the game itself asks for is */
    if (voices_only && !voice) {
        if (n == 0 && !speech_busy()) { uint32_t z = 0; call0(0x57D2, 1, &z, 0); service(); }
        return;
    }
    /* during a voice: sounds on its channel, stops and the theme wait */
    if (!voice && speech_busy() && (sound_channel(n) == speak_ch || n == 0 || n == 1)) {
        if (npending < 8) pending_req[npending++] = n;
        else pending_req[7] = n;
        return;
    }
    int home = cur_overlay <= 0 || cur_overlay == 1 || cur_overlay == 2 || cur_overlay == 10;
    for (size_t k = 0; k < sizeof sound_sets / sizeof sound_sets[0]; k++)
        if (sound_sets[k].sound == n && home) amigasnd_scene(sound_sets[k].set);
    for (size_t k = 0; k < sizeof cave_sets / sizeof cave_sets[0]; k++)
        if (cave_sets[k].sound == n && cur_overlay == 6) amigasnd_scene(cave_sets[k].set);
    uint32_t a = (uint32_t)n;
    call0(0x57D2, 1, &a, 0);
    service();
    if (voice) { speaking = 1; speak_start = played; speak_ch = sound_channel(n) & 3; }
}

void amigasnd_render(int32_t *buf, int n, int rate, int music_pct)
{
    static double tick_acc;
    if (!active) return;
    double tick_len = (double)rate / (50.0 * music_pct / 100.0);
    for (int s = 0; s < n; s++) {
        tick_acc += 1;
        if (tick_acc >= tick_len) {
            tick_acc -= tick_len;
            music_open = 1;
            call0(0x58BC, 0, NULL, 0);
            music_open = 0;
            service();
            if (!speech_busy() && (pending_set >= 0 || npending || pending_voice)) resume();
        }
        int mix = 0;
        for (int i = 0; i < 4; i++) {
            Channel *c = &ch[i];
            if (!c->on || !(dmacon & 0x200) || !(dmacon & (1 << i))) continue;
            uint32_t a = c->pt + ((uint32_t)c->pos & ~1u) + ((uint32_t)c->pos & 1);
            int vol = regs[(0xA8 + 16 * i) / 2] & 0x7F;
            if (vol > 64) vol = 64;
            mix += (int8_t)mem[a & ADDR_MASK] * vol;
            int per = regs[(0xA6 + 16 * i) / 2];
            if (per < 113) per = 113;
            c->pos += PAL_CLOCK / per / rate;
            while (c->pos >= 2.0) {
                c->pos -= 2.0;
                c->pt += 2;
                if (--c->left == 0) {
                    c->pt = aud_lc(i);
                    c->left = aud_len(i);
                    raise_irq(7 + i);
                }
            }
        }
        buf[s] += mix;
    }
    played += (uint64_t)n;
}

#endif
