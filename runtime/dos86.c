/* DOS (INT 21h) and BIOS (INT 10h/11h/16h/1Ah/33h, default IRQ
 * handlers) emulation. */
#include <ctype.h>
#include <string.h>
#include <time.h>

#include "cpu86.h"
#include "dos86.h"
#include "settings86.h"

int hle_trace;

/* ---- memory arena (paragraphs) --------------------------------------------------- */
/* End of DOS memory (paragraphs). A Tandy 1000 keeps its video memory in
 * the top of RAM; the BIOS reserves 16 KB, so DOS ends at 9C00h (the game
 * draws into the 64 KB below its memory end, 9000h-9FFFh). A Hercules card
 * has no memory at A000h, which is given to DOS for the larger buffers the
 * Hercules driver needs (see INT 21h AH=48h). */
static uint32_t mem_top(void)
{
    return settings.graphics == GFX_TANDY ? 0x9C00u
         : settings.graphics == GFX_HERCULES ? 0xB000u : 0xA000u;
}
#define MEM_TOP mem_top()
typedef struct { uint32_t seg, paras; int used; } MBlock;
#define MAX_BLOCKS 1024
static MBlock blocks[MAX_BLOCKS];
static int nblocks;

static void mem_init(uint32_t psp, uint32_t prog_end)
{
    (void)prog_end;
    blocks[0] = (MBlock){psp, MEM_TOP - psp, 1};   /* DOS gives the program everything */
    nblocks = 1;
}

static void mem_compact(void)
{
    /* merge adjacent free blocks */
    for (int i = 0; i + 1 < nblocks; ) {
        if (!blocks[i].used && !blocks[i + 1].used) {
            blocks[i].paras += blocks[i + 1].paras;
            memmove(&blocks[i + 1], &blocks[i + 2], sizeof(MBlock) * (nblocks - i - 2));
            nblocks--;
        } else {
            i++;
        }
    }
}

static int find_block(uint32_t seg)
{
    for (int i = 0; i < nblocks; i++) if (blocks[i].seg == seg && blocks[i].used) return i;
    return -1;
}

static void insert_block(int at, MBlock b)
{
    if (nblocks >= MAX_BLOCKS) rt_fatal("DOS memory: too many blocks");
    memmove(&blocks[at + 1], &blocks[at], sizeof(MBlock) * (nblocks - at));
    blocks[at] = b;
    nblocks++;
}

static int mem_alloc(uint32_t paras, uint32_t *seg, uint32_t *largest)
{
    *largest = 0;
    for (int i = 0; i < nblocks; i++) {
        if (blocks[i].used) continue;
        if (blocks[i].paras > *largest) *largest = blocks[i].paras;
        if (blocks[i].paras >= paras) {
            if (blocks[i].paras > paras)
                insert_block(i + 1, (MBlock){blocks[i].seg + paras, blocks[i].paras - paras, 0});
            blocks[i].paras = paras;
            blocks[i].used = 1;
            *seg = blocks[i].seg;
            return 1;
        }
    }
    return 0;
}

static int mem_resize(uint32_t seg, uint32_t paras, uint32_t *largest)
{
    int i = find_block(seg);
    if (i < 0) return 0;
    if (paras <= blocks[i].paras) {
        if (paras < blocks[i].paras) {
            insert_block(i + 1, (MBlock){seg + paras, blocks[i].paras - paras, 0});
            blocks[i].paras = paras;
            mem_compact();
        }
        return 1;
    }
    uint32_t avail = blocks[i].paras;
    if (i + 1 < nblocks && !blocks[i + 1].used) avail += blocks[i + 1].paras;
    *largest = avail;
    if (avail < paras) return 0;
    uint32_t extra = paras - blocks[i].paras;
    blocks[i + 1].seg += extra;
    blocks[i + 1].paras -= extra;
    blocks[i].paras = paras;
    if (!blocks[i + 1].paras) memmove(&blocks[i + 1], &blocks[i + 2], sizeof(MBlock) * (--nblocks - i - 1));
    return 1;
}

/* ---- files -------------------------------------------------------------------------- */
#define MAX_FILES 20
static FILE *files[MAX_FILES];
static char file_names[MAX_FILES][64];

const char *dos_file_name(int handle)
{
    return handle >= 0 && handle < MAX_FILES && files[handle] ? file_names[handle] : "";
}
static uint32_t psp_seg, env_seg;

static void read_str(uint32_t seg, uint32_t off, char *out, size_t max)
{
    size_t i = 0;
    for (; i + 1 < max; i++) {
        char c = (char)rb(seg, off + i);
        if (!c) break;
        out[i] = c;
    }
    out[i] = 0;
}

/* Game file names are DOS 8.3, opened relative to the current directory
 * (= the game folder); the host file system may be case sensitive. */
static FILE *open_game_file(const char *dosname, const char *mode)
{
    const char *base = dosname;
    for (const char *p = dosname; *p; p++) if (*p == '\\' || *p == ':' || *p == '/') base = p + 1;
    char name[260], path[1024];
    snprintf(name, sizeof name, "%s", base);
    for (int pass = 0; pass < 3; pass++) {
        for (char *p = name; *p; p++) *p = (char)(pass == 1 ? toupper(*p) : pass == 2 ? tolower(*p) : *p);
        snprintf(path, sizeof path, "%s/%s", game_dir(), name);
        FILE *f = fopen(path, mode);
        if (f) return f;
    }
    return NULL;
}

static void set_cf(int fail) { R.cf = fail != 0; }

static void dos_int21(void)
{
    uint32_t ah = HI(R.ax);
    char name[256];
    if (hle_trace) fprintf(stderr, "[int21 %02X] ax=%04X bx=%04X cx=%04X dx=%04X\n", ah, R.ax, R.bx, R.cx, R.dx);
    switch (ah) {
    case 0x02: video_text_out(LO(R.dx)); return;
    case 0x06:
        if (LO(R.dx) != 0xFF) { video_text_out(LO(R.dx)); return; }
        { int k = key_get(0); R.zf = k < 0; SET_LO(R.ax, k < 0 ? 0 : k & 0xFF); }
        return;
    case 0x01: case 0x07: case 0x08: {
        int k = key_get(1);
        SET_LO(R.ax, k & 0xFF);
        if (ah == 0x01) video_text_out(k & 0xFF);
        return;
    }
    case 0x09:
        for (uint32_t i = 0; i < 4000; i++) {
            uint32_t c = rb(R.ds, R.dx + i);
            if (c == '$') break;
            video_text_out((int)c);
        }
        return;
    case 0x0B: SET_LO(R.ax, key_get(-1) >= 0 ? 0xFF : 0); return;
    case 0x0C: {
        while (key_get(0) >= 0) {}
        uint32_t al = LO(R.ax);
        if (al == 1 || al == 6 || al == 7 || al == 8) { R.ax = (uint16_t)(al << 8 | (R.dx & 0xFF)); dos_int21(); }
        return;
    }
    case 0x0E: SET_LO(R.ax, 26); return;
    case 0x19: SET_LO(R.ax, 2); return;                     /* drive C: */
    case 0x1A: return;                                      /* set DTA */
    case 0x25: ww(0, LO(R.ax) * 4, R.dx); ww(0, LO(R.ax) * 4 + 2, R.ds); return;
    case 0x2A: { time_t t = time(NULL); struct tm *m = localtime(&t);
        R.cx = (uint16_t)(m->tm_year + 1900); R.dx = (uint16_t)((m->tm_mon + 1) << 8 | m->tm_mday);
        SET_LO(R.ax, m->tm_wday); return; }
    case 0x2C: { time_t t = time(NULL); struct tm *m = localtime(&t);
        uint32_t cs = (bios_ticks() * 100 / 18) % 100;
        R.cx = (uint16_t)(m->tm_hour << 8 | m->tm_min); R.dx = (uint16_t)(m->tm_sec << 8 | cs); return; }
    case 0x30: R.ax = 0x0003; R.bx = 0; R.cx = 0; return;   /* DOS 3.0 */
    case 0x33: SET_LO(R.dx, 0); return;
    case 0x35: R.bx = (uint16_t)rw(0, LO(R.ax) * 4); R.es = (uint16_t)rw(0, LO(R.ax) * 4 + 2); return;
    case 0x3D: {
        read_str(R.ds, R.dx, name, sizeof name);
        int slot = 5;
        while (slot < MAX_FILES && files[slot]) slot++;
        FILE *f = slot < MAX_FILES ? open_game_file(name, (LO(R.ax) & 3) ? "r+b" : "rb") : NULL;
        if (hle_trace || !f) fprintf(stderr, "  open %s: %s\n", name, f ? "ok" : "FAIL");
        if (!f) { R.ax = 2; set_cf(1); return; }
        files[slot] = f;
        snprintf(file_names[slot], sizeof file_names[slot], "%s", name);
        R.ax = (uint16_t)slot;
        set_cf(0);
        return;
    }
    case 0x3E:
        if (R.bx < MAX_FILES && files[R.bx]) { fclose(files[R.bx]); files[R.bx] = NULL; }
        set_cf(0);
        return;
    case 0x3F: {
        if (R.bx == 0) {                /* stdin: one key */
            int k = key_get(1);
            wb(R.ds, R.dx, k & 0xFF);
            R.ax = 1;
            set_cf(0);
            return;
        }
        FILE *f = R.bx < MAX_FILES ? files[R.bx] : NULL;
        if (!f) { R.ax = 6; set_cf(1); return; }
        uint32_t n = 0;
        for (; n < R.cx; n++) {
            int c = fgetc(f);
            if (c == EOF) break;
            wb(R.ds, R.dx + n, c);
        }
        R.ax = (uint16_t)n;
        set_cf(0);
        return;
    }
    case 0x40:
        if (R.bx == 1 || R.bx == 2) {
            for (uint32_t i = 0; i < R.cx; i++) video_text_out((int)rb(R.ds, R.dx + i));
            R.ax = R.cx;
            set_cf(0);
            return;
        }
        { FILE *f = R.bx < MAX_FILES ? files[R.bx] : NULL;
          if (!f) { R.ax = 6; set_cf(1); return; }
          for (uint32_t i = 0; i < R.cx; i++) fputc((int)rb(R.ds, R.dx + i), f);
          R.ax = R.cx; set_cf(0); }
        return;
    case 0x42: {
        FILE *f = R.bx < MAX_FILES ? files[R.bx] : NULL;
        if (!f) { R.ax = 6; set_cf(1); return; }
        long off = (long)(int32_t)(((uint32_t)R.cx << 16) | R.dx);
        int wh = LO(R.ax) == 0 ? SEEK_SET : LO(R.ax) == 1 ? SEEK_CUR : SEEK_END;
        fseek(f, off, wh);
        long p = ftell(f);
        R.ax = (uint16_t)p;
        R.dx = (uint16_t)(p >> 16);
        set_cf(0);
        return;
    }
    case 0x44:
        if (LO(R.ax) == 0) {           /* device info: handles 0-2 are the console */
            R.dx = R.bx <= 2 ? 0x80D3 : 0x0002;
            set_cf(0);
            return;
        }
        set_cf(0);
        R.ax = 0;
        return;
    case 0x47: wb(R.ds, R.si, 0); set_cf(0); return;      /* current dir = root */
    case 0x48: {
        uint32_t seg, largest, paras = R.bx;
        /* Hercules: the driver addresses screen-sized buffers as a 32 KB
         * page of 4 interleaved 8 KB banks (up to offset 7E9Fh), but the
         * game sizes them as 28000 bytes; they get the full page (the
         * memory for it comes from the A000h area, see mem_top). */
        if (settings.graphics == GFX_HERCULES && paras >= 0x600 && paras < 0x800) paras = 0x800;
        if (mem_alloc(paras, &seg, &largest)) { R.ax = (uint16_t)seg; set_cf(0); }
        else { R.ax = 8; R.bx = (uint16_t)largest; set_cf(1); }
        return;
    }
    case 0x49: {
        int i = find_block(R.es);
        if (i >= 0) { blocks[i].used = 0; mem_compact(); set_cf(0); } else { R.ax = 9; set_cf(1); }
        return;
    }
    case 0x4A: {
        uint32_t largest = 0;
        if (mem_resize(R.es, R.bx, &largest)) set_cf(0);
        else { R.ax = 8; R.bx = (uint16_t)largest; set_cf(1); }
        return;
    }
    case 0x4C:
        if (hle_trace) fprintf(stderr, "exit %d\n", LO(R.ax));
        exit(LO(R.ax));
    case 0x62: R.bx = (uint16_t)psp_seg; return;
    }
    rt_fatal("unimplemented INT 21h AH=%02X", ah);
}

static void bios_int10(void)
{
    uint32_t ah = HI(R.ax);
    switch (ah) {
    case 0x00: video_set_mode(LO(R.ax) & 0x7F); return;
    case 0x05:
        if (LO(R.ax) & 0x80) {      /* Tandy/PCjr page registers */
            int bh = HI(R.bx), bl = LO(R.bx);
            if (video_tandy_pages(LO(R.ax), &bh, &bl)) R.bx = (uint16_t)(bh << 8 | bl);
            return;
        }
        video_set_page(LO(R.ax));
        return;
    case 0x01: case 0x02: case 0x06: case 0x07: return;
    case 0x03: R.dx = 0; R.cx = 0x0607; return;
    case 0x08: R.ax = 0x0720; return;
    case 0x09: case 0x0A:
        for (uint32_t i = 0; i < R.cx; i++) video_text_out(LO(R.ax));
        return;
    case 0x0B: video_cga_palette(HI(R.bx), LO(R.bx)); return;
    case 0x0E: video_text_out(LO(R.ax)); return;
    case 0x0F: R.ax = (uint16_t)(0x2800 | video_get_mode()); R.bx = (uint16_t)((video_get_page() << 8) | LO(R.bx)); return;
    case 0x10:
        if (LO(R.ax) == 0x00) { video_ega_palette(LO(R.bx), HI(R.bx)); return; }
        if (LO(R.ax) == 0x02) {
            for (int i = 0; i < 16; i++) video_ega_palette(i, rb(R.es, R.dx + i));
            return;
        }
        return;
    case 0x11: return;
    case 0x12:                       /* EGA information: colour, 256 KB */
        if (LO(R.bx) == 0x10 && GFX_IS_EGA(settings.graphics)) { R.bx = 0x0003; R.cx = 0x0009; }
        return;
    case 0x1A: return;               /* no VGA: AL stays as is */
    }
    rt_fatal("unimplemented INT 10h AH=%02X", ah);
}

void bios_vector(uint32_t n)
{
    switch (n) {
    case 0x08:                         /* timer: tick count, then INT 1Ch */
        ww(0x40, 0x6C, rw(0x40, 0x6C) + 1);
        do_int(0x1C, 0xF000, 0xFFF0);
        return;
    case 0x09: { extern void bios_keyboard_irq(void); bios_keyboard_irq(); return; }
    case 0x10: bios_int10(); return;
    case 0x11: R.ax = 0x0063; return;  /* equipment: no joystick bit needed */
    case 0x12: R.ax = (uint16_t)(MEM_TOP / 64); return;
    case 0x16: {
        uint32_t ah = HI(R.ax) & 0xEF;
        if (hle_trace) { static uint32_t n16; if (++n16 % 500 == 1) fprintf(stderr, "[int16 %02X] #%u\n", ah, n16); }
        if (ah == 0x00) { int k = key_get(1); R.ax = (uint16_t)k; return; }
        if (ah == 0x01) { int k = key_get(-1); R.zf = k < 0; if (k >= 0) R.ax = (uint16_t)k; return; }
        if (ah == 0x02) { SET_LO(R.ax, 0); return; }
        return;
    }
    case 0x1A:
        if (HI(R.ax) == 0) { uint32_t t = bios_ticks(); R.cx = (uint16_t)(t >> 16); R.dx = (uint16_t)t; SET_LO(R.ax, 0); }
        return;
    case 0x1C: case 0x23: case 0x24: case 0x1B: return;
    case 0x20: exit(0);
    case 0x21: dos_int21(); return;
    case 0x33: R.ax = 0; return;       /* no mouse */
    }
    rt_fatal("unimplemented interrupt %02X", n);
}

void dos_init(uint32_t psp, uint32_t prog_end)
{
    psp_seg = psp;
    for (uint32_t n = 0; n < 256; n++) {
        ww(0, n * 4, n * 4);
        ww(0, n * 4 + 2, BIOS_SEG);
    }
    /* BIOS data area */
    ww(0x40, 0x10, 0x0063);
    wb(0x40, 0x49, 3);
    if (settings.graphics == GFX_TANDY) {
        mem[0xFFFFE] = 0xFF;            /* model byte: PC / Tandy 1000 */
        mem[0xFC000] = 0x21;            /* Tandy BIOS signature */
    } else {
        mem[0xFFFFE] = 0xFC;            /* AT */
    }
    if (settings.graphics == GFX_HERCULES) {
        ww(0x40, 0x10, 0x0073);         /* equipment: monochrome display */
        ww(0x40, 0x63, 0x3B4);
    }
    ww(0x40, 0x13, MEM_TOP / 64);
    ww(0x40, 0x1A, 0x1E);              /* keyboard buffer head/tail */
    ww(0x40, 0x1C, 0x1E);
    /* environment: one variable, then the program path */
    env_seg = psp - 0x20;
    const char envtext[] = "PATH=C:\\\0\0\x01\0C:\\DUCKTALE.EXE";
    for (size_t i = 0; i < sizeof envtext; i++) wb(env_seg, i, (uint8_t)envtext[i]);
    /* PSP */
    wb(psp, 0, 0xCD); wb(psp, 1, 0x20);
    ww(psp, 2, MEM_TOP);
    ww(psp, 0x2C, env_seg);
    wb(psp, 0x80, 0);
    wb(psp, 0x81, 0x0D);
    mem_init(psp, prog_end);
    video_init();
}
