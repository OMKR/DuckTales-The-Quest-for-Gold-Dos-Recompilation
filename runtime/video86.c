/* EGA (planar, modes 0Dh/0Eh/10h), CGA (04h), Tandy (09h), Hercules
 * graphics and text mode emulation, shown through SDL. The adapter chosen in
 * the settings decides how the game's detection probes are answered. */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <string.h>

#include "cpu86.h"
#include "dos86.h"
#include "settings86.h"
#include "font8x8_basic.h"

#define W 640
#define H 400

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static uint32_t pixels[W * H];
static int headless, mode = 3;

/* EGA state */
static uint8_t plane[4][0x10000];
static uint8_t latch[4];
static uint8_t seq_idx, seq[8] = {0, 0, 0x0F};
static uint8_t gc_idx, gc[9] = {0, 0, 0, 0, 0, 0, 0, 0x0F, 0xFF};
static uint8_t crtc_idx, crtc[32];
static uint8_t attr[21], attr_idx;
static int attr_flip;
static uint32_t start_addr;
static int active_page;
static int text_x, text_y;

/* CGA: colour select register (INT 10h AH=0Bh / port 3D9h) */
static uint8_t cga_color = 0x30;    /* palette 1, intense */
/* Tandy: page register 3DFh (CRT page bits 0-2, CPU page bits 3-5, 16 KB
 * pages in the top 128 KB of RAM) and palette registers via 3DAh/3DEh */
static uint8_t tandy_page = 0x36, tandy_idx, tandy_pal[16];
#define TANDY_RAM 0x80000u
/* Hercules: mode control 3B8h (bit 1 graphics, bit 7 page 1), CRTC 3B4h */
static uint8_t herc_ctrl, herc_crtc_idx, herc_crtc[32];

static const uint32_t cga16[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

static uint32_t ega64(int v)
{
    /* rgbRGB: primary bits 0-2 (B G R), secondary bits 3-5 */
    int b = ((v & 1) ? 2 : 0) + ((v & 8) ? 1 : 0);
    int g = ((v & 2) ? 2 : 0) + ((v & 16) ? 1 : 0);
    int r = ((v & 4) ? 2 : 0) + ((v & 32) ? 1 : 0);
    return (uint32_t)(r * 0x55) << 16 | (uint32_t)(g * 0x55) << 8 | (uint32_t)(b * 0x55);
}

static uint32_t palette_rgb(int i)
{
    int v = attr[i & 15];
    if (mode == 0x0D || mode == 0x0E) {
        /* 200-line modes: IRGB with intensity from bit 4 */
        int c = (v & 7) | ((v & 0x10) ? 8 : 0);
        return cga16[c];
    }
    return ega64(v);
}

uint32_t video_ega_rgb(int colour) { return palette_rgb(colour); }

uint8_t video_plane_byte(int p, uint32_t off) { return plane[p & 3][off & 0xFFFF]; }

void video_plane_copy(uint32_t dst, uint32_t src, uint32_t n)
{
    for (int p = 0; p < 4; p++) memmove(&plane[p][dst & 0xFFFF], &plane[p][src & 0xFFFF], n);
}

void video_plane_fill(uint32_t dst, uint32_t n, uint8_t v)
{
    for (int p = 0; p < 4; p++) memset(&plane[p][dst & 0xFFFF], v, n);
}

void video_ega_palette(int index, int value)
{
    if (index < 16) attr[index] = (uint8_t)value;
}

void video_init(void)
{
    headless = getenv("DT_HEADLESS") != NULL;
    for (int i = 0; i < 16; i++) attr[i] = (uint8_t)(i < 8 ? i : i + 8);
    if (headless) return;
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0)
        rt_fatal("SDL_Init: %s", SDL_GetError());
    window = SDL_CreateWindow("DuckTales: The Quest for Gold (DOS)", SDL_WINDOWPOS_CENTERED,
                              SDL_WINDOWPOS_CENTERED, 320 * settings.scale, 200 * settings.scale,
                              SDL_WINDOW_RESIZABLE |
                              (settings.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    /* no vsync: the emulated CPU runs in this thread, and waiting for the
     * monitor would leave it almost no time; frames are paced by the
     * frame rate limit (chips86.c) */
    renderer = SDL_CreateRenderer(window, -1, 0);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_RenderSetLogicalSize(renderer, W, H);
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, W, H);
}

void video_set_mode(int m)
{
    mode = m;
    memset(plane, 0, sizeof plane);
    text_x = text_y = 0;
    start_addr = 0;
    active_page = 0;
    for (int i = 0; i < 16; i++) tandy_pal[i] = (uint8_t)i;
    if (m == 9) tandy_page = 0x36;     /* BIOS: 32 KB at the top (pages 6-7) */
    if (m == 4 || m == 5) cga_color = 0x30;
    for (int i = 0; i < 16; i++) attr[i] = (uint8_t)(i < 8 ? i : i + 8);
    if (m == 3) for (int i = 0; i < 4000; i += 2) { mem[0xB8000 + i] = ' '; mem[0xB8001 + i] = 7; }
    if (hle_trace || getenv("DT_VTRACE")) fprintf(stderr, "[video] mode %02X\n", m);
}

int video_get_mode(void) { return mode; }

void video_toggle_fullscreen(void)
{
    if (!window) return;
    int full = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
    SDL_SetWindowFullscreen(window, full ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
}

void video_set_title(const char *title)
{
    if (window) SDL_SetWindowTitle(window, title);
}


/* INT 10h AH=0Bh: CGA background (BH=0) / palette (BH=1) */
void video_cga_palette(int bh, int bl)
{
    if (bh == 0) cga_color = (uint8_t)((cga_color & 0xE0) | (bl & 0x1F));
    else cga_color = (uint8_t)((cga_color & ~0x20) | ((bl & 1) << 5));
}

/* INT 10h AH=05h AL=80h-83h (Tandy/PCjr): read/set the CRT and CPU pages */
int video_tandy_pages(int al, int *bh, int *bl)
{
    if (settings.graphics != GFX_TANDY) return 0;
    if (al & 1) tandy_page = (uint8_t)((tandy_page & 0xC7) | ((*bl & 7) << 3));
    if (al & 2) tandy_page = (uint8_t)((tandy_page & 0xF8) | (*bh & 7));
    *bh = tandy_page & 7;
    *bl = (tandy_page >> 3) & 7;
    return 1;
}

/* INT 10h AH=05: the BIOS page size is 2000h bytes in mode 0Dh, 4000h in
 * 0Eh and 8000h in 10h. */
void video_set_page(int page)
{
    uint32_t size = mode == 0x0D ? 0x2000 : mode == 0x0E ? 0x4000 : 0x8000;
    active_page = page;
    start_addr = (uint32_t)page * size;
}
int video_get_page(void) { return active_page; }
uint32_t video_start_addr(void) { return start_addr; }

/* reads of 3DAh that still see the current vertical retrace */
static int retrace_reads;
void video_vblank(void) { retrace_reads = 4; }

static int is_ega(void)
{
    return GFX_IS_EGA(settings.graphics) && (mode == 0x0D || mode == 0x0E || mode == 0x10);
}

/* Tandy: B800-BFFF is a 32 KB window on the CPU page */
static uint32_t tandy_cpu_addr(uint32_t lin)
{
    return TANDY_RAM + ((tandy_page >> 3) & 6) * 0x4000u + (lin - 0xB8000) % 0x8000u;
}

/* DT_VSTAT: which function writes video memory, with what EGA state */
static int vstat_on = -1;
static struct { uint32_t fn, key, n, lo, hi; } vstat[256];
static void vstat_note(uint32_t off)
{
    if (vstat_on < 0) { vstat_on = getenv("DT_VSTAT") != NULL; if (!vstat_on) return; }
    uint32_t fn = ftrace_ring[(ftrace_pos - 1) & 63];
    uint32_t key = (uint32_t)(gc[5] & 3) << 24 | (uint32_t)seq[2] << 16 | (uint32_t)gc[8] << 8 | gc[3];
    for (int i = 0; i < 256; i++) {
        if (vstat[i].n && (vstat[i].fn != fn || vstat[i].key != key)) continue;
        if (!vstat[i].n) { vstat[i].fn = fn; vstat[i].key = key; vstat[i].lo = off; vstat[i].hi = off; }
        vstat[i].n++;
        if (off < vstat[i].lo) vstat[i].lo = off;
        if (off > vstat[i].hi) vstat[i].hi = off;
        return;
    }
}
void vstat_dump(void)
{
    if (vstat_on <= 0) return;
    for (int i = 0; i < 256 && vstat[i].n; i++)
        fprintf(stderr, "vstat %u:%04X:%04X wm%u map%02X mask%02X rot%02X n=%u off %04X-%04X\n",
                vstat[i].fn >> 28, (vstat[i].fn >> 16) & 0xFFF, vstat[i].fn & 0xFFFF,
                vstat[i].key >> 24, (vstat[i].key >> 16) & 0xFF, (vstat[i].key >> 8) & 0xFF,
                vstat[i].key & 0xFF, vstat[i].n, vstat[i].lo, vstat[i].hi);
}

/* ---- EGA memory access ---------------------------------------------------------------- */
uint32_t vram_read(uint32_t lin)
{
    if (lin >= 0xA0000 && lin < 0xB0000 && is_ega()) {
        uint32_t off = lin - 0xA0000;
        for (int p = 0; p < 4; p++) latch[p] = plane[p][off];
        if (gc[5] & 8) {                /* read mode 1: colour compare */
            uint8_t r = 0;
            for (int bit = 0; bit < 8; bit++) {
                int match = 1;
                for (int p = 0; p < 4; p++) {
                    if (!(gc[7] & (1 << p))) continue;
                    int b = (latch[p] >> bit) & 1, c = (gc[2] >> p) & 1;
                    if (b != c) match = 0;
                }
                if (match) r |= (uint8_t)(1 << bit);
            }
            return r;
        }
        return latch[gc[4] & 3];
    }
    if (settings.graphics == GFX_TANDY && lin >= 0xB8000) return mem[tandy_cpu_addr(lin)];
    return mem[lin];
}

void vram_write(uint32_t lin, uint32_t v)
{
    if (!(lin >= 0xA0000 && lin < 0xB0000 && is_ega())) {
        if (settings.graphics == GFX_TANDY && lin >= 0xB8000) lin = tandy_cpu_addr(lin);
        mem[lin] = (uint8_t)v;
        return;
    }
    uint32_t off = lin - 0xA0000;
    if (vstat_on) vstat_note(off);
    int wmode = gc[5] & 3, rot = gc[3] & 7, func = (gc[3] >> 3) & 3;
    uint8_t mask = gc[8];
    v = ((v >> rot) | (v << (8 - rot))) & 0xFF;
    for (int p = 0; p < 4; p++) {
        if (!(seq[2] & (1 << p))) continue;
        uint8_t d;
        if (wmode == 1) { plane[p][off] = latch[p]; continue; }
        if (wmode == 2) d = (v & (1 << p)) ? 0xFF : 0;
        else d = (gc[1] & (1 << p)) ? ((gc[0] & (1 << p)) ? 0xFF : 0) : (uint8_t)v;
        switch (func) {
        case 1: d &= latch[p]; break;
        case 2: d |= latch[p]; break;
        case 3: d ^= latch[p]; break;
        }
        plane[p][off] = (uint8_t)((d & mask) | (latch[p] & ~mask));
    }
}

uint32_t vga_port_in(uint32_t port)
{
    int g = settings.graphics;
    /* only the chosen adapter's ports answer the game's probes */
    if (port >= 0x3B0 && port < 0x3BC && g != GFX_HERCULES) return 0xFF;
    if (port >= 0x3C0 && port < 0x3D0 && !GFX_IS_EGA(g)) return 0xFF;
    if (port >= 0x3D0 && port < 0x3E0 && g == GFX_HERCULES) return 0xFF;
    switch (port) {
    case 0x3B5: return herc_crtc[herc_crtc_idx & 31];
    case 0x3BA: {
        /* bit 7: vertical retrace (the detection waits for it to change) */
        static uint32_t n;
        return (++n & 0x40) ? 0x80 : 0x00;
    }
    case 0x3DA: {
        attr_flip = 0;
        /* Vertical retrace: one per presented frame (video_vblank, called
         * at the frame rate limit), seen by the next 4 reads. The game waits
         * for it before drawing or flipping pages; while it waits, the
         * frame clock is checked often (poll_count) so the next frame and
         * retrace come on time. Display enable toggles. */
        int retrace = retrace_reads > 0;
        if (retrace) retrace_reads--;
        else if (poll_count > 20) poll_count = 20;
        static int de;
        de ^= 1;
        return (retrace ? 0x08 : 0) | de;
    }
    case 0x3C5: return seq[seq_idx & 7];
    case 0x3CF: return gc[gc_idx % 9];
    case 0x3D5: return crtc[crtc_idx & 31];
    }
    return 0xFF;
}

void vga_port_out(uint32_t port, uint32_t v)
{
    switch (port) {
    case 0x3B4: herc_crtc_idx = (uint8_t)v; return;
    case 0x3B5: herc_crtc[herc_crtc_idx & 31] = (uint8_t)v; return;
    case 0x3B8: herc_ctrl = (uint8_t)v; return;
    case 0x3D9: cga_color = (uint8_t)v; return;
    case 0x3DA: if (settings.graphics == GFX_TANDY) { tandy_idx = (uint8_t)v; return; } break;
    case 0x3DE:
        if (tandy_idx >= 0x10 && tandy_idx < 0x20) tandy_pal[tandy_idx - 0x10] = (uint8_t)(v & 15);
        return;
    case 0x3DF: tandy_page = (uint8_t)v; return;
    case 0x3C4: seq_idx = (uint8_t)v; return;
    case 0x3C5: seq[seq_idx & 7] = (uint8_t)v; return;
    case 0x3CE: gc_idx = (uint8_t)v; return;
    case 0x3CF: gc[gc_idx % 9] = (uint8_t)v; return;
    case 0x3D4: crtc_idx = (uint8_t)v; return;
    case 0x3D5:
        if (getenv("DT_VTRACE")) fprintf(stderr, "[crtc %02X=%02X]", crtc_idx, (unsigned)v & 0xFF);
        crtc[crtc_idx & 31] = (uint8_t)v;
        start_addr = (uint32_t)crtc[0x0C] << 8 | crtc[0x0D];
        return;
    case 0x3C0:
        if (!attr_flip) attr_idx = (uint8_t)(v & 0x1F);
        else if (attr_idx < 16) attr[attr_idx] = (uint8_t)v;
        attr_flip ^= 1;
        return;
    }
}

/* ---- text output (DOS/BIOS teletype) ------------------------------------------------------- */
void video_text_out(int ch)
{
    if (hle_trace || headless) fputc(ch, stderr);
    if (mode != 3) return;
    if (ch == '\r') { text_x = 0; return; }
    if (ch == '\n') { text_y++; }
    else if (ch == 8) { if (text_x) text_x--; return; }
    else if (ch == 7) return;
    else {
        mem[0xB8000 + (text_y * 80 + text_x) * 2] = (uint8_t)ch;
        mem[0xB8000 + (text_y * 80 + text_x) * 2 + 1] = 7;
        if (++text_x >= 80) { text_x = 0; text_y++; }
    }
    if (text_y >= 25) {
        memmove(mem + 0xB8000, mem + 0xB8000 + 160, 160 * 24);
        for (int i = 0; i < 160; i += 2) { mem[0xB8000 + 24 * 160 + i] = ' '; mem[0xB8000 + 24 * 160 + i + 1] = 7; }
        text_y = 24;
    }
}

/* ---- rendering ------------------------------------------------------------------------------- */
static void render(void)
{
    if (is_ega() && mode == 0x0D && amiga_active()) {
        int dark = 1;                    /* palette all black: faded out */
        for (int i = 0; i < 16; i++) if (attr[i]) dark = 0;
        amiga_render(start_addr, pixels, W, H, dark);
        return;
    }
    if (is_ega()) {
        int w = mode == 0x10 ? 640 : (mode == 0x0E ? 640 : 320);
        int h = mode == 0x10 ? 350 : 200;
        int bpr = w / 8;
        uint32_t pal[16];
        for (int i = 0; i < 16; i++) pal[i] = 0xFF000000u | palette_rgb(i);
        for (int y = 0; y < H; y++) {
            int sy = y * h / H;
            for (int x = 0; x < W; x++) {
                int sx = x * w / W;
                uint32_t off = (start_addr + sy * bpr + (sx >> 3)) & 0xFFFF;
                int bit = 7 - (sx & 7), c = 0;
                for (int p = 0; p < 4; p++) c |= ((plane[p][off] >> bit) & 1) << p;
                pixels[y * W + x] = pal[c];
            }
        }
        return;
    }
    if (settings.graphics == GFX_HERCULES && (herc_ctrl & 2)) {
        /* 720x348, 4 interleaved banks of 90-byte rows */
        uint32_t base = (herc_ctrl & 0x80) ? 0xB8000 : 0xB0000;
        for (int y = 0; y < H; y++) {
            int sy = y * 348 / H;
            for (int x = 0; x < W; x++) {
                int sx = x * 720 / W;
                uint8_t b = mem[base + (sy & 3) * 0x2000 + (sy >> 2) * 90 + (sx >> 3)];
                pixels[y * W + x] = ((b >> (7 - (sx & 7))) & 1) ? 0xFFE0E0E0u : 0xFF000000u;
            }
        }
        return;
    }
    if (mode == 9) {
        /* Tandy 320x200x16: 4 interleaved banks of 160-byte rows, CRT page */
        uint32_t base = TANDY_RAM + (tandy_page & 6) * 0x4000u;
        for (int y = 0; y < H; y++) {
            int sy = y / 2;
            for (int x = 0; x < W; x++) {
                int sx = x / 2;
                uint8_t b = mem[base + (sy & 3) * 0x2000 + (sy >> 2) * 160 + (sx >> 1)];
                int c = (sx & 1) ? (b & 15) : (b >> 4);
                pixels[y * W + x] = 0xFF000000u | cga16[tandy_pal[c] & 15];
            }
        }
        return;
    }
    if (mode == 4 || mode == 5) {
        /* palette 0: green/red/brown, 1: cyan/magenta/white; bit 4 intensity */
        int hi = (cga_color & 0x10) ? 8 : 0;
        uint32_t cgapal[4] = {cga16[cga_color & 15], 0, 0, 0};
        static const int pal0[3] = {2, 4, 6}, pal1[3] = {3, 5, 7};
        for (int i = 0; i < 3; i++) cgapal[i + 1] = cga16[((cga_color & 0x20) ? pal1 : pal0)[i] | hi];
        for (int y = 0; y < H; y++) {
            int sy = y / 2;
            for (int x = 0; x < W; x++) {
                int sx = x / 2;
                uint8_t b = mem[0xB8000 + (sy & 1) * 0x2000 + (sy >> 1) * 80 + (sx >> 2)];
                pixels[y * W + x] = 0xFF000000u | cgapal[(b >> (6 - 2 * (sx & 3))) & 3];
            }
        }
        return;
    }
    /* text 80x25 with an 8x8 font scaled to 8x16 */
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int cx = x / 8, cy = y / 16, row = (y % 16) / 2;
            uint8_t ch = mem[0xB8000 + (cy * 80 + cx) * 2], at = mem[0xB8000 + (cy * 80 + cx) * 2 + 1];
            int on = ch < 128 && (font8x8_basic[ch][row] >> (x % 8)) & 1;
            pixels[y * W + x] = 0xFF000000u | cga16[on ? (at & 15) : (at >> 4) & 7];
        }
    }
}

void video_frame(void)
{
    if (headless) return;
    render();
    SDL_UpdateTexture(texture, NULL, pixels, W * 4);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
}

int video_save_bmp(const char *path)
{
    render();
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    uint32_t size = 54 + W * H * 4;
    uint8_t hdr[54] = {'B', 'M'};
    memcpy(hdr + 2, &size, 4);
    hdr[10] = 54; hdr[14] = 40;
    int32_t w = W, h = -H;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 32;
    fwrite(hdr, 1, 54, f);
    fwrite(pixels, 4, W * H, f);
    fclose(f);
    return 1;
}
