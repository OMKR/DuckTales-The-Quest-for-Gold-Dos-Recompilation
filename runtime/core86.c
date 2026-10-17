/* Core: memory, program loading (EXEPACK + overlays), PSP, interrupt
 * vectors, dynamic dispatch of far/near calls, overlay switching. */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <stdarg.h>
#include <string.h>

#include "cpu86.h"
#include "settings86.h"
#include "dispatch86.h"
#include "functions86.h"
#include "layout86.h"
#include "dos86.h"
#include "amigasnd.h"

CPU86 R;
uint8_t *mem;
uint32_t ftrace_ring[64];
unsigned ftrace_pos;

#define OVL_SEG 0x0C17
#define DGROUP 0x120E
#define MAX_OVL 16
#define EMU87_SEG 0x09C4          /* "MSEM87" segment of the 8087 emulator */

static struct { uint8_t *data; uint32_t size; uint32_t *relocs; uint32_t nrel; } ovl[MAX_OVL];
static int cur_ovl;
static uint32_t image_paras;        /* paragraphs of the root image */

void ftrace_dump(void)
{
    fprintf(stderr, "recent functions:");
    for (unsigned i = ftrace_pos - 24; i != ftrace_pos; i++) {
        uint32_t a = ftrace_ring[i & 63];
        fprintf(stderr, " %u:%04X:%04X", a >> 28, (a >> 16) & 0xFFF, a & 0xFFFF);
    }
    fprintf(stderr, "\nreturn stack (innermost first):");
    for (int i = 1; i <= 12 && i <= rs_top; i++)
        fprintf(stderr, " %04X:%04X", rs_stack[(rs_top - i) & 1023] >> 16, rs_stack[(rs_top - i) & 1023] & 0xFFFF);
    fprintf(stderr, "\n");
}

void rt_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    ftrace_dump();
    fprintf(stderr, "FATAL: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n  ax=%04X bx=%04X cx=%04X dx=%04X si=%04X di=%04X bp=%04X sp=%04X\n"
            "  es=%04X cs=%04X ss=%04X ds=%04X ovl=%d\n", R.ax, R.bx, R.cx, R.dx, R.si, R.di,
            R.bp, R.sp, R.es, R.cs, R.ss, R.ds, cur_ovl);
    fprintf(stderr, "  stack:");
    for (int i = 0; i < 12; i++) fprintf(stderr, " %04X", rw(R.ss, R.sp + 2 * i));
    fprintf(stderr, "\n  return stack:");
    for (int i = 1; i <= 8 && i <= rs_top; i++)
        fprintf(stderr, " %04X:%04X", rs_stack[(rs_top - i) & 1023] >> 16, rs_stack[(rs_top - i) & 1023] & 0xFFFF);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

/* ---- function lookup --------------------------------------------------------------- */

static void (*find_fn(int img, uint32_t cs, uint32_t ip))(void)
{
    int lo = 0, hi = func86_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const Func86 *f = &func86_table[mid];
        int c = f->img != img ? (f->img < img ? -1 : 1)
              : f->cs != cs ? (f->cs < cs ? -1 : 1)
              : f->ip != ip ? (f->ip < ip ? -1 : 1) : 0;
        if (c == 0) return f->fn;
        if (c < 0) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

/* Which driver (if any) the game loaded at `seg`, by content fingerprint. */
static int driver_at(uint32_t seg)
{
    static uint32_t cache_seg[8];
    static int cache_img[8], ncache;
    for (int i = 0; i < ncache; i++)
        if (cache_seg[i] == seg) return cache_img[i];
    uint32_t v = 0x811C9DC5u;
    for (int i = 0; i < 16; i++) v = (v ^ mem[LIN(seg, i)]) * 0x01000193u;
    for (size_t k = 0; k < sizeof driver_fp / sizeof driver_fp[0]; k++)
        if (driver_fp[k].hash == v) {
            if (ncache < 8) { cache_seg[ncache] = seg; cache_img[ncache++] = driver_fp[k].img; }
            return driver_fp[k].img;
        }
    return -1;
}

static void call_resolved(uint32_t seg, uint32_t off, int near_call)
{
    if (seg == BIOS_SEG) {               /* a chained or called BIOS/DOS vector */
        bios_vector(off / 4);
        pop16(); pop16(); set_flags(pop16());   /* its iret */
        return;
    }
    uint32_t raw = (seg - LOAD_SEG) & 0xFFFF;
    if (raw == EMU87_SEG && !near_call) {
        /* MSC's 8087 emulator (initialised through a far pointer): the
         * x87 instructions are executed natively, so its services are
         * no-ops; return as its retf would. */
        pop16(); pop16();
        R.ax = 0;
        return;
    }
    if (seg >= LOAD_SEG && raw < image_paras + 0x1000) {
        void (*fn)(void) = NULL;
        if (raw >= OVL_SEG && raw < DGROUP && cur_ovl) fn = find_fn(cur_ovl, raw, off);
        if (!fn) fn = find_fn(0, raw, off);
        if (fn) { fn(); return; }
    }
    int d = driver_at(seg);
    if (d >= 0) {
        void (*fn)(void) = find_fn(d, 0, off);
        if (fn) {
            uint16_t saved = R.cs;
            if (d == DRIVER_BASE + 1 && amiga_driver(seg, off)) return;
            /* the Tandy / speaker driver's sound request (far, number at SP+4) */
            if (((d == DRIVER_BASE + 4 && off == 0x415) || (d == DRIVER_BASE + 5 && off == 0x408)) && amigasnd_active())
                amigasnd_request(rw(R.ss, R.sp + 4));
            /* the EGA driver's texts (string at SP+10): voices */
            if (d == DRIVER_BASE + 1 && (off == 0x81F || off == 0x909) && amigasnd_active()) {
                char text[96];
                uint32_t str = rw(R.ss, R.sp + 10);
                size_t k = 0;
                for (; k + 1 < sizeof text && rb(R.ds, str + k); k++) text[k] = (char)rb(R.ds, str + k);
                text[k] = 0;
                amigasnd_text(text);
            }
            R.cs = (uint16_t)seg;
            fn();
            R.cs = saved;
            if (d == DRIVER_BASE + 1) amiga_driver_after(off);
            return;
        }
    }
    if (getenv("DT_DUMPSEG")) {
        FILE *f = fopen("seg_dump.bin", "wb");
        if (f) { fwrite(mem + LIN(seg, 0), 1, 0x4000, f); fclose(f); }
    }
    if (raw < image_paras) {   /* record it so the analyzer can add it (tools/extra_entries.txt) */
        int img = (raw >= OVL_SEG && raw < DGROUP) ? cur_ovl : 0;
        FILE *f = fopen("missing_entries.txt", "a");
        if (f) { fprintf(f, "%x %04x %04x\n", img, raw, off); fclose(f); }
        fprintf(stderr, "MISSING ENTRY %x %04x %04x\n", img, raw, off);
    }
    rt_fatal("call to unknown code %04X:%04X (load %04X)", seg, off, LOAD_SEG);
}

uint32_t rs_stack[1024];
int rs_top;

void dispatch_far(uint32_t seg, uint32_t off) { call_resolved(seg & 0xFFFF, off & 0xFFFF, 0); }

static int is_return_to(uint32_t seg, uint32_t off)
{
    return rs_top > 0 && rs_stack[(rs_top - 1) & 1023] == (((seg & 0xFFFF) << 16) | (off & 0xFFFF));
}

void jump_far(uint32_t seg, uint32_t off)
{
    if (is_return_to(seg, off)) return;
    call_resolved(seg & 0xFFFF, off & 0xFFFF, 0);
}

void jump_near(uint32_t cs, uint32_t off)
{
    if (is_return_to(cs, off)) return;
    call_resolved(cs & 0xFFFF, off & 0xFFFF, 1);
}
void dispatch_near(uint32_t cs, uint32_t off) { call_resolved(cs & 0xFFFF, off & 0xFFFF, 1); }

/* Software interrupt from the program. */
void do_int(uint32_t n, uint32_t ret_cs, uint32_t ret_ip)
{
    uint32_t off = rw(0, n * 4), seg = rw(0, n * 4 + 2);
    if (seg == BIOS_SEG && off == n * 4) {
        bios_vector(n);
        return;
    }
    push16(get_flags());
    push16(ret_cs);
    push16(ret_ip);
    R.ifl = 0;
    dispatch_far(seg, off);
}

/* Hardware interrupt delivered between instructions. */
void raise_irq(uint32_t vector)
{
    if (!R.ifl) return;
    CPU86 saved = R;          /* handlers restore registers themselves; keep C-side state safe */
    do_int(vector, 0xF000, 0xFFF0);
    saved.sp = R.sp;
    R = saved;
}

/* ---- overlays ------------------------------------------------------------------------- */

int ovl_load(int n)
{
    int prev = cur_ovl;
    if (n == cur_ovl) return prev;
    if (n <= 0 || n >= MAX_OVL || !ovl[n].data) rt_fatal("bad overlay %d", n);
    /* the next scene starts when a voice has been spoken, as on the Amiga
     * (else the plane's engine and other scene sounds would wait behind it) */
    for (int k = 0; k < 20 * 60 && amigasnd_speaking(); k++) wait_frames(1);
    uint32_t base = LIN(LOAD_SEG + OVL_SEG, 0);
    memcpy(mem + base, ovl[n].data, ovl[n].size);
    for (uint32_t i = 0; i < ovl[n].nrel; i++) {
        uint32_t a = LOAD_SEG * 16 + ovl[n].relocs[i];
        uint32_t v = mem[a] | (mem[a + 1] << 8);
        v += LOAD_SEG;
        mem[a] = (uint8_t)v;
        mem[a + 1] = (uint8_t)(v >> 8);
    }
    cur_ovl = n;
    amigasnd_overlay(n);
    return prev;
}

/* Vertical retraces per frame, %: the climbing, cave and other minigames
 * draw a full screen per step and wait for the next retrace; on the PCs of
 * the time a step took two retraces (the plane's, a lighter screen, one). */
int retrace_pct(void)
{
    switch (cur_ovl) {
    case 1: case 2: case 4: case 10: return 100;   /* menus, office, map, plane */
    default: return 50;
    }
}

/* Game ticks (DS:2225), %: the scales (overlay 3) pace their steps by
 * ticks, and on the PCs of the time each full-screen step took longer
 * than its wait. */
/* menus (title, office, map, investments): not a minigame (overlays 3-9) */
int menu_mode(void) { return cur_ovl < 3 || cur_ovl > 9; }

int tick_pct(void) { return cur_ovl == 3 ? 40 : 100; }

void ovl_restore(int n)
{
    if (n && n != cur_ovl) ovl_load(n);
}

/* ---- loading ---------------------------------------------------------------------------- */

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*len);
    if (fread(b, 1, *len, f) != *len) { fclose(f); free(b); return NULL; }
    fclose(f);
    return b;
}

static uint32_t u16le(const uint8_t *p) { return p[0] | (p[1] << 8); }

/* Unpack the EXEPACK'd load image into mem at LOAD_SEG and relocate it. */
static void load_root(const uint8_t *d, size_t len, size_t *image_end_out)
{
    uint32_t last = u16le(d + 2), pages = u16le(d + 4), hdr = u16le(d + 8) * 16;
    uint32_t cs = u16le(d + 0x16);
    size_t image_end = (pages - 1) * 512 + (last ? last : 512);
    if (image_end > len) rt_fatal("short EXE");
    const uint8_t *image = d + hdr;
    size_t image_len = image_end - hdr;
    uint32_t eh = cs * 16;
    uint32_t dest_len = u16le(image + eh + 0x0C) * 16;
    uint32_t skip = 1, hlen = 0x10;
    if (memcmp(image + eh + 0x0E, "RB", 2) != 0) {
        skip = u16le(image + eh + 0x0E);
        hlen = 0x12;
    }
    (void)hlen;
    uint32_t packed_end = eh - (skip - 1) * 16;
    uint8_t *buf = calloc(1, dest_len > image_len ? dest_len : image_len);
    memcpy(buf, image, packed_end);
    int32_t src = (int32_t)packed_end - 1, dst = (int32_t)dest_len - 1;
    while (src >= 0 && buf[src] == 0xFF) src--;
    for (;;) {
        uint8_t cmd = buf[src];
        uint32_t count = (buf[src - 1] << 8) | buf[src - 2];
        src -= 3;
        if ((cmd & 0xFE) == 0xB0) {
            uint8_t v = buf[src--];
            while (count--) buf[dst--] = v;
        } else if ((cmd & 0xFE) == 0xB2) {
            while (count--) buf[dst--] = buf[src--];
        } else {
            rt_fatal("EXEPACK: bad command %02X", cmd);
        }
        if (cmd & 1) break;
    }
    memcpy(mem + LOAD_SEG * 16, buf, dest_len);
    image_paras = (dest_len + 15) / 16;
    /* relocations: 16 segment groups after "Packed file is corrupt" */
    const uint8_t *stub = image + eh;
    uint32_t exepack_size = u16le(stub + 6), pos = 0;
    for (uint32_t i = 0; i + 22 <= exepack_size; i++)
        if (!memcmp(stub + i, "Packed file is corrupt", 22)) { pos = i + 22; break; }
    if (!pos) rt_fatal("EXEPACK: relocation table not found");
    for (uint32_t g = 0; g < 16; g++) {
        uint32_t n = u16le(stub + pos);
        pos += 2;
        for (uint32_t k = 0; k < n; k++) {
            uint32_t a = LOAD_SEG * 16 + g * 0x10000 + u16le(stub + pos);
            pos += 2;
            uint32_t v = (mem[a] | (mem[a + 1] << 8)) + LOAD_SEG;
            mem[a] = (uint8_t)v;
            mem[a + 1] = (uint8_t)(v >> 8);
        }
    }
    free(buf);
    *image_end_out = image_end;
}

static void load_overlays(const uint8_t *d, size_t len, size_t start)
{
    for (size_t pos = start; pos + 0x20 < len; ) {
        if (d[pos] != 'M' || d[pos + 1] != 'Z') { pos++; continue; }
        const uint8_t *h = d + pos;
        uint32_t last = u16le(h + 2), pages = u16le(h + 4), nrel = u16le(h + 6);
        uint32_t hdr = u16le(h + 8) * 16, relo = u16le(h + 0x18), n = u16le(h + 0x1A);
        uint32_t size = (pages - 1) * 512 + (last ? last : 512);
        if (n > 0 && n < MAX_OVL) {
            ovl[n].size = size - hdr;
            ovl[n].data = malloc(ovl[n].size);
            memcpy(ovl[n].data, h + hdr, ovl[n].size);
            ovl[n].nrel = nrel;
            ovl[n].relocs = malloc(sizeof(uint32_t) * (nrel + 1));
            for (uint32_t k = 0; k < nrel; k++)
                ovl[n].relocs[k] = u16le(h + relo + 4 * k + 2) * 16 + u16le(h + relo + 4 * k);
        }
        pos += ((size + 511) / 512) * 512;
    }
}

/* ---- startup ------------------------------------------------------------------------------ */

static const char *gamedir;

const char *game_dir(void) { return gamedir; }

int main(int argc, char **argv)
{
    settings_defaults(&settings);
    settings_load(&settings, settings_path());
    gamedir = argc > 1 ? argv[1] : settings.gamedir;
    /* a relative game folder in the settings is relative to the program */
    static char gamedir_buf[1600];
    const char *g = settings.gamedir;
    if (argc <= 1 && !(g[0] == '/' || g[0] == '\\' || (g[0] && g[1] == ':'))) {
        char *base = SDL_GetBasePath();
        if (base) {
            snprintf(gamedir_buf, sizeof gamedir_buf, "%s%s", base, g);
            gamedir = gamedir_buf;
            SDL_free(base);
        }
    }
    mem = calloc(1, MEM86_SIZE);
    char path[1024];
    snprintf(path, sizeof path, "%s/DUCKTALE.EXE", gamedir);
    size_t len, image_end;
    uint8_t *exe = read_file(path, &len);
    if (!exe) rt_fatal("cannot read %s", path);
    load_root(exe, len, &image_end);
    load_overlays(exe, len, image_end);
    free(exe);

    dos_init(LOAD_SEG - 0x10, LOAD_SEG + image_paras + 0x0D14);  /* + minalloc */
    chips_init();
    amiga_init();

    R.ds = R.es = LOAD_SEG - 0x10;      /* PSP */
    R.ss = STACK_SS;
    R.sp = STACK_SP;
    R.cs = ENTRY_CS;
    R.ifl = 1;
    push16(0);                          /* return to PSP:0000 (INT 20h) */
    void (*entry)(void) = find_fn(0, ENTRY_CS - LOAD_SEG, ENTRY_IP);
    if (!entry) rt_fatal("entry not found");
    entry();
    fprintf(stderr, "program returned\n");
    return 0;
}
