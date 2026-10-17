/* "Amiga" graphics mode (settings: graphics = GFX_AMIGA, amiga_dir).
 *
 * The game sees an EGA card and keeps its EGA driver (IT.DRV module 1).
 * Every drawing call to that driver is also carried out here on 8-bit
 * surfaces (one byte per pixel, an index into a 32-colour palette), and the
 * screen is shown from these surfaces only:
 *  - the two screen pages (A000h, A200h);
 *  - picture buffers: when the game loads a picture (040E:175C, hook at its
 *    exit) the same entry of the Amiga version's .arc file is decoded into
 *    the buffer's surface, with the Amiga palette that best reproduces the
 *    DOS picture;
 *  - sprite strips: loaded the same way, drawn by the sprite call.
 * Pictures or sprites without an Amiga counterpart, and text, lines and
 * fills, are drawn from the EGA data / colours with the nearest colour of
 * the target's palette. Palette 0 is the standard EGA palette.
 *
 * EGA driver entries (offset in module 1, arguments from SP+4):
 *   00E5 flip pages            0101 copy drawn page to shown (5 interlaced passes)
 *   0155 wipe shown page       01B0 clear drawn page (colour)
 *   01EF copy rectangle (h, w, dst y, dst x, dst desc, src y, src x, src desc)
 *   035D sprite (object)       0515 pixel (y, x)       069B/073F lines
 *   081F text (colour, y, x, string, font far)
 *   0909 text with background (fg<<8|bg, y, x, string, font far)
 * Descriptors (DS): {row bytes, height, far data, plane size}; the screen's
 * at driver cs:001E (its segment is the page being drawn), the colour of
 * pixels/lines at cs:0024, the "draw text on the shown page" flag at
 * DS:[cs:0022]. Sprite objects: {x, y, width bytes, height, stride, ?, far
 * data, plane size, frame}.
 *
 * Amiga .arc entries: same container and headers as the DOS ones, RLE
 * (bit 7 clear: repeat the next byte n times, set: copy n bytes), 5
 * bitplanes stored by 8-pixel column (column, plane 0..4, `height` bytes
 * each); sprite strips hold `frames` such images side by side. Palettes: 32
 * 12-bit RGB words in the Amiga program. See tools/amigagfx.py.
 */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu86.h"
#include "dos86.h"
#include "settings86.h"
#include "amigasnd.h"

#define MAX_SURF 64
#define MAX_SPRITES 160
#define NPALS 64

typedef struct {
    uint32_t lin;          /* plane 0 of the DOS buffer (pages: A0000h + offset) */
    int w, h;
    int pal;               /* palette: of the picture / the page's scene */
    int scene;             /* a scene picture: copying it sets the page palette */
    int arc;               /* archive of the scene (arcs[]), -1 = none */
    int cycle;             /* colour cycling of the scene (cycles[]), 0 = none */
    uint8_t *px;           /* 0-31 Amiga colour index, EGA(c) = drawn by the game
                            * in EGA colour c (fills, text, EGA pictures) */
} Surface;

/* EGA colours are shown as the Amiga colour learned for them in the
 * screen's palette. */
#define EGA(c) (uint8_t)(0x20 | ((c) & 15))
/* single pixels the game sets (markers such as the cave map's rooms): the
 * nearest colour to the EGA colour itself, not the learned one (in the cave
 * the learned green is a brown; its map marks slime rooms in green) */
#define EGA_PIXEL(c) (uint8_t)(0x30 | ((c) & 15))

typedef struct {
    uint32_t lin;          /* DOS sprite data */
    int fw, h, frames;
    uint8_t *px;           /* frames * fw x h, 0 = transparent */
    int arc;               /* archive it was loaded from */
    int hw, hh;            /* Amiga entry header width/height */
    int sub_arc;           /* scene archive of `sub`, -1 = not looked up */
    int entry;             /* its entry in the Amiga archive */
    uint8_t *sub;          /* the scene archive's own version, or NULL */
    uint8_t sub_frame[64]; /* its frame for each frame (matched by colour) */
} Sprite;

static Surface pages[2];
static Surface surf[MAX_SURF];
static int nsurf;
static Sprite sprites[MAX_SPRITES];
static int nsprites;
static uint32_t pals[NPALS][32];       /* ARGB; 0 = standard EGA */
/* EGA colour -> palette index, learned from DOS/Amiga picture pairs:
 * votes[pal][ega][index] */
static uint32_t votes[NPALS][16][32];
static int16_t learned[NPALS][16];     /* -1 = none */
static int npals;
static int enabled;
static uint32_t drv;                   /* driver segment */

/* ---- files ------------------------------------------------------------------------------- */
static uint8_t *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = n > 0 ? malloc((size_t)n) : NULL;
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); d = NULL; }
    fclose(f);
    if (d) *len = (size_t)n;
    return d;
}

static uint8_t *amiga_file(const char *name, size_t *len)
{
    char lower[64], path[1200];
    size_t i = 0;
    for (; name[i] && i < sizeof lower - 1; i++)
        lower[i] = (char)(name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 32 : name[i]);
    lower[i] = 0;
    static const char *const disks[] = {"DT1", "DT2"};
    const char *names[2] = {name, lower};
    for (int k = 0; k < 2; k++)
        for (int n = 0; n < 2; n++) {
            snprintf(path, sizeof path, "%s/%s/%s", settings.amiga_dir, disks[k], names[n]);
            uint8_t *d = read_all(path, len);
            if (d) return d;
        }
    return NULL;
}

/* ---- archives ---------------------------------------------------------------------------- */
typedef struct {
    char name[16];
    uint8_t *amiga, *dos;
    size_t alen, dlen;
    int an, dn;
    int map[256];          /* DOS entry -> Amiga entry or -1 */
    int pal[256];          /* palette chosen for an Amiga entry, -1 = not yet */
    int small_pal;         /* one palette for the small pictures (tiles, icons) */
    int scene;             /* fixed palette of the archive's scene, or 0 */
} Archive;

static Archive arcs[24];
static int narcs;

static int arc_count(const uint8_t *d, size_t len) { return len >= 2 ? d[0] | d[1] << 8 : 0; }

static const uint8_t *arc_entry(const uint8_t *d, size_t len, int i, size_t *elen)
{
    int n = arc_count(d, len);
    if (i < 0 || i >= n || 6 + 4u * (unsigned)i + 4 > len) return NULL;
    const uint8_t *t = d + 2 + 4 * i;
    uint32_t a = t[0] | t[1] << 8 | t[2] << 16 | (uint32_t)t[3] << 24;
    uint32_t b = t[4] | t[5] << 8 | t[6] << 16 | (uint32_t)t[7] << 24;
    /* the last entry runs to the end of the file (the Amiga MAIN.ARC ends its
     * table with a wrong offset there: its entry 41, the inventor) */
    if (i == n - 1 && (b > len || b < a)) b = (uint32_t)len;
    if (b > len || a + 16 > b) return NULL;
    *elen = b - a;
    return d + a;
}

static int hdr16(const uint8_t *e, int k) { return e[2 * k] | e[2 * k + 1] << 8; }

static int kind(const uint8_t *d, size_t len, int i)
{
    size_t el;
    const uint8_t *e = arc_entry(d, len, i, &el);
    if (!e) return -1;
    return hdr16(e, 2) << 16 | hdr16(e, 4);
}

/* DOS and Amiga archives mostly have the same entries in the same order; a
 * few have one or more extra. Align them by entry type and height. */
static void align(Archive *a)
{
    int i = 0, j = 0;
    for (int k = 0; k < 256; k++) a->map[k] = -1;
    while (i < a->dn && j < a->an && i < 256) {
        if (kind(a->dos, a->dlen, i) == kind(a->amiga, a->alen, j)) { a->map[i++] = j++; continue; }
        /* the same picture in a different size (EXP.ARC 17: 136x112 / 144x102)
         * when the archives agree again after it */
        if (i + 1 < a->dn && j + 1 < a->an && kind(a->dos, a->dlen, i + 1) == kind(a->amiga, a->alen, j + 1) &&
            hdr16(arc_entry(a->dos, a->dlen, i, &(size_t){0}), 2) == hdr16(arc_entry(a->amiga, a->alen, j, &(size_t){0}), 2)) {
            a->map[i++] = j++;
            continue;
        }
        int skip_a = 0, skip_d = 0;
        for (int k = 1; k <= 4; k++) {
            if (!skip_a && j + k < a->an && kind(a->dos, a->dlen, i) == kind(a->amiga, a->alen, j + k)) skip_a = k;
            if (!skip_d && i + k < a->dn && kind(a->dos, a->dlen, i + k) == kind(a->amiga, a->alen, j)) skip_d = k;
        }
        if (skip_a && (!skip_d || skip_a <= skip_d)) j += skip_a;
        else if (skip_d) i += skip_d;
        else a->map[i++] = j++;        /* same position, different size */
    }
}

/* DOS pictures: LZW (9-12 bit codes LSB first, 256 = reset, 257 = end),
 * 4 bits per pixel */
static uint8_t *lzw(const uint8_t *e, size_t len, size_t limit)
{
    static uint16_t prefix[4096];
    static uint8_t suffix[4096], stack[4096];
    uint8_t *out = malloc(limit + 4096);
    size_t n = 0, bit = 16 * 8, end = len * 8;
    int width = 9, next = 258, prev = -1, first = 0;
    while (out && bit + (size_t)width <= end && n < limit) {
        size_t b = bit >> 3;
        uint32_t chunk = e[b] | (b + 1 < len ? e[b + 1] << 8 : 0) | (b + 2 < len ? e[b + 2] << 16 : 0);
        int code = (int)((chunk >> (bit & 7)) & ((1u << width) - 1));
        bit += (size_t)width;
        if (code == 257) break;
        if (code == 256) { width = 9; next = 258; prev = -1; continue; }
        int sp = 0, c = code;
        if (c >= next) { if (prev < 0) break; stack[sp++] = (uint8_t)first; c = prev; }
        while (c >= 258 && sp < 4000) { stack[sp++] = suffix[c]; c = prefix[c]; }
        if (c > 255) break;
        stack[sp++] = (uint8_t)c;
        first = c;
        while (sp) out[n++] = stack[--sp];
        if (prev >= 0 && next < 4096) {
            prefix[next] = (uint16_t)prev;
            suffix[next] = (uint8_t)first;
            if (++next >= (1 << width) && width < 12) width++;
        }
        prev = code;
    }
    return out;
}

static const uint32_t ega16[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF};

static uint8_t *decode(const uint8_t *e, size_t len, int *w, int *h);

static void vote(int pal, int ega, int idx)
{
    uint32_t *v = votes[pal][ega & 15], total = 0;
    v[idx & 31]++;
    int best = 0;
    for (int k = 0; k < 32; k++) { total += v[k]; if (v[k] > v[best]) best = k; }
    /* only a clear correspondence counts (the sky of the plane game: all
     * of one colour); mixed votes mean the EGA colour stands for many */
    learned[pal][ega & 15] = (int16_t)(total >= 100 && v[best] * 10 >= total * 7 ? best : -1);
}

/* the palette that best reproduces all small DOS pictures of an archive
 * (standard EGA colours) with their Amiga versions */
/* Votes from a DOS/Amiga picture pair, only where both are flat (3x3 of
 * one colour): fills and text in the game's EGA colours stand for areas,
 * and edges of shapes drawn differently would vote for wrong colours. */
static void vote_flat(int pal, const uint8_t *eg, const uint8_t *am, int w, int h)
{
    for (int y = 1; y + 1 < h; y++)
        for (int x = 1; x + 1 < w; x++) {
            uint8_t e = eg[y * w + x], c = am[y * w + x];
            int flat = 1;
            for (int dy = -1; dy <= 1 && flat; dy++)
                for (int dx = -1; dx <= 1; dx++)
                    if (eg[(y + dy) * w + x + dx] != e || am[(y + dy) * w + x + dx] != c) { flat = 0; break; }
            if (flat) vote(pal, e, c);
        }
}

static int archive_palette(Archive *a, int forced)
{
    static double err[NPALS];
    memset(err, 0, sizeof err);
    for (int i = 0; i < a->dn && i < 256; i++) {
        if (a->map[i] < 0) continue;
        size_t dl, al;
        const uint8_t *de = arc_entry(a->dos, a->dlen, i, &dl), *ae = arc_entry(a->amiga, a->alen, a->map[i], &al);
        if (!de || !ae || hdr16(de, 2) != 1 || hdr16(ae, 2) != 1 || hdr16(de, 4) >= 100) continue;
        int dw = hdr16(de, 3), dh = hdr16(de, 4), aw, ah;
        uint8_t *dp = lzw(de, dl, (size_t)dw * dh / 2), *ap = decode(ae, al, &aw, &ah);
        if (dp && ap)
            for (int y = 0; y < dh && y < ah; y += 2)
                for (int x = 0; x < dw && x < aw; x += 2) {
                    uint8_t b = dp[(y * dw + x) / 2];
                    uint32_t d = ega16[x % 2 ? b & 15 : b >> 4];
                    for (int k = 1; k < npals; k++) {
                        uint32_t c = pals[k][ap[y * aw + x]];
                        for (int sh = 0; sh < 24; sh += 8) {
                            int diff = (int)((c >> sh) & 255) - (int)((d >> sh) & 255);
                            err[k] += diff * diff;
                        }
                    }
                }
        free(dp);
        free(ap);
    }
    int best = 1;
    for (int k = 2; k < npals; k++) if (err[k] < err[best]) best = k;
    if (forced) best = forced;
    /* learn the colour correspondence for that palette */
    for (int i = 0; i < a->dn && i < 256; i++) {
        if (a->map[i] < 0) continue;
        size_t dl, al;
        const uint8_t *de = arc_entry(a->dos, a->dlen, i, &dl), *ae = arc_entry(a->amiga, a->alen, a->map[i], &al);
        if (!de || !ae || hdr16(de, 2) != 1 || hdr16(ae, 2) != 1 || hdr16(de, 4) >= 100) continue;
        int dw = hdr16(de, 3), dh = hdr16(de, 4), aw, ah;
        uint8_t *dp = lzw(de, dl, (size_t)dw * dh / 2), *ap = decode(ae, al, &aw, &ah);
        if (dp && ap) {
            int w = dw < aw ? dw : aw, h = dh < ah ? dh : ah;
            uint8_t *eg = malloc((size_t)w * (size_t)h);
            for (int y = 0; eg && y < h; y++)
                for (int x = 0; x < w; x++) {
                    uint8_t b = dp[(y * dw + x) / 2];
                    eg[y * w + x] = x % 2 ? b & 15 : b >> 4;
                }
            uint8_t *am = malloc((size_t)w * (size_t)h);
            for (int y = 0; am && y < h; y++) memcpy(am + y * w, ap + y * aw, (size_t)w);
            if (eg && am) vote_flat(best, eg, am, w, h);
            free(eg);
            free(am);
        }
        free(dp);
        free(ap);
    }
    return best;
}

/* Scenes whose palette the comparison with the DOS pictures gets wrong: the
 * entry of the Amiga program's palette table (0 = at 0xB066, 64 bytes
 * apart) the Amiga game uses there, seen by running it. */
static const struct { const char *arc; int table; } scene_palette[] = {
    {"JG.ARC", 11},      /* jungle, 0xB326 */
    {"MT.ARC", 7},       /* mountain, 0xB226 */
    {"PS.ARC", 8},       /* nature photography, 0xB266 */
    {"EXP.ARC", 10},     /* cave, 0xB2E6 */
    {"PLANE.ARC", 6},    /* plane, 0xB1E6 */
    {"MAC.ARC", 5},      /* the scales, 0xB1A6 */
    {"MBIN.ARC", 19},    /* money bin, 0xB526 */
};

/* Single pictures the comparison gets wrong: the map's frame (MAIN.ARC 20)
 * is shown with the map (entry 22) and the office, all in 0xB126; the
 * treasure chest (35) was matched to a grey-blue palette. */
static const struct { const char *arc; int entry, table; } picture_palette[] = {
    {"MAIN.ARC", 20, 3},
    {"MAIN.ARC", 35, 18},    /* the treasure chest: gold on brown (0xB4E6) */
    {"MAIN.ARC", 41, 15},    /* the inventor (plane broken, Bombasium) */
    {"MAIN.ARC", 36, 22},    /* the winner's magazine cover: the player */
    {"MAIN.ARC", 37, 22},    /* ... and the rival (green hat) */
};

static int same_name(const char *a, const char *b)
{
    while (*a && (*b == *a || *b == *a + 32)) { a++; b++; }
    return !*a && !*b;
}

static int picture_override(const char *name, int entry)
{
    for (size_t i = 0; i < sizeof picture_palette / sizeof picture_palette[0]; i++)
        if (picture_palette[i].entry == entry && same_name(picture_palette[i].arc, name) &&
            1 + picture_palette[i].table < npals) return 1 + picture_palette[i].table;
    return 0;
}

static int scene_override(const char *name)
{
    for (size_t i = 0; i < sizeof scene_palette / sizeof scene_palette[0]; i++) {
        const char *a = scene_palette[i].arc, *b = name;
        while (*a && (*b == *a || *b == *a + 32)) { a++; b++; }
        if (!*a && !*b && 1 + scene_palette[i].table < npals) return 1 + scene_palette[i].table;
    }
    return 0;
}

static Archive *archive(const char *name)
{
    if (!name[0]) return NULL;
    for (int i = 0; i < narcs; i++)
        if (!strcmp(arcs[i].name, name)) return arcs[i].amiga ? &arcs[i] : NULL;
    if (narcs >= (int)(sizeof arcs / sizeof arcs[0])) return NULL;
    Archive *a = &arcs[narcs++];
    memset(a, 0, sizeof *a);
    snprintf(a->name, sizeof a->name, "%s", name);
    a->amiga = amiga_file(name, &a->alen);
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", game_dir(), name);
    a->dos = read_all(path, &a->dlen);
    if (!a->amiga || !a->dos) { a->amiga = NULL; return NULL; }
    a->an = arc_count(a->amiga, a->alen);
    a->dn = arc_count(a->dos, a->dlen);
    for (int k = 0; k < 256; k++) a->pal[k] = -1;
    align(a);
    a->scene = scene_override(name);
    a->small_pal = archive_palette(a, a->scene);
    return a;
}

/* ---- decoding ---------------------------------------------------------------------------- */
static uint8_t *rle(const uint8_t *e, size_t len, size_t *out_len)
{
    size_t cap = 65536, n = 0, p = 16;
    uint8_t *out = malloc(cap);
    while (p < len && out) {
        int c = e[p++], k = c & 0x7F;
        if (n + (size_t)k > cap) {
            uint8_t *o = realloc(out, cap *= 2);
            if (!o) { free(out); return NULL; }
            out = o;
        }
        if (c & 0x80) { for (int i = 0; i < k && p < len; i++) out[n++] = e[p++]; }
        else if (p < len) { memset(out + n, e[p++], (size_t)k); n += (size_t)k; }
    }
    *out_len = n;
    return out;
}

/* colour indices of an Amiga picture or strip, w (all frames) x h */
static uint8_t *decode(const uint8_t *e, size_t len, int *w, int *h)
{
    size_t n;
    uint8_t *data = rle(e, len, &n);
    if (!data) return NULL;
    *h = hdr16(e, 4);
    int cols = *h ? (int)(n / (5u * (unsigned)*h)) : 0;
    *w = cols * 8;
    uint8_t *px = malloc((size_t)*w * (size_t)*h + 1);
    for (int y = 0; px && y < *h; y++)
        for (int xb = 0; xb < cols; xb++) {
            const uint8_t *b = data + (size_t)xb * *h * 5 + y;
            for (int bit = 0; bit < 8; bit++) {
                int c = 0;
                for (int p = 0; p < 5; p++) c |= ((b[p * *h] >> (7 - bit)) & 1) << p;
                px[y * *w + xb * 8 + bit] = (uint8_t)c;
            }
        }
    free(data);
    return px;
}

static void load_palettes(void)
{
    static const uint32_t ega[16] = {
        0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
        0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF};
    for (int k = 0; k < 32; k++) pals[0][k] = 0xFF000000u | ega[k & 15];
    npals = 1;
    memset(learned, 0xFF, sizeof learned);
    size_t len;
    uint8_t *exe = amiga_file("DuckTales", &len);
    if (!exe) return;
    /* The palette table: the longest run of 32-word palettes (colour 0 black,
     * 12-bit words) 64 bytes apart (0xB066-0xB5E6 in this program). */
    size_t best = 0, best_n = 0;
    for (size_t st = 0; st + 64 <= len; st += 2) {
        size_t n = 0;
        while (st + 64 * (n + 1) <= len) {
            const uint8_t *q = exe + st + 64 * n;
            int ok = q[0] == 0 && q[1] == 0, distinct = 0;
            uint16_t seen[32];
            for (int k = 0; ok && k < 32; k++) {
                uint16_t w = (uint16_t)(q[2 * k] << 8 | q[2 * k + 1]);
                if (w >= 0x1000) ok = 0;
                int dup = 0;
                for (int m = 0; m < distinct; m++) if (seen[m] == w) dup = 1;
                if (!dup) seen[distinct++] = w;
            }
            if (!ok || distinct < 4) break;
            n++;
        }
        if (n > best_n) { best_n = n; best = st; }
        if (n > 1) st += 64 * (n - 1);
    }
    for (size_t k = 0; k < best_n && npals < NPALS; k++, npals++)
        for (int c = 0; c < 32; c++) {
            const uint8_t *q = exe + best + 64 * k + 2 * c;
            uint16_t w = (uint16_t)(q[0] << 8 | q[1]);
            pals[npals][c] = 0xFF000000u | ((w >> 8) & 15) * 0x110000u | ((w >> 4) & 15) * 0x1100u | (w & 15) * 0x11u;
        }
    free(exe);
}

/* What an EGA colour of the game stands for in a scene with palette `pal`:
 * the Amiga colour learned from the DOS/Amiga picture pairs, else the EGA
 * colour itself (with the game's current EGA palette). */
static uint32_t ega_argb(int pal, int ega)
{
    if (!(ega & 15)) return pals[pal][0];        /* black: the Amiga's colour 0 */
    if (!(ega & 0x10) && learned[pal][ega & 15] >= 0) return pals[pal][learned[pal][ega & 15]];
    /* else the nearest colour of the palette */
    uint32_t rgb = video_ega_rgb(ega & 15), best = pals[pal][0];
    long best_d = 1L << 30;
    for (int k = 0; k < 32; k++) {
        uint32_t c = pals[pal][k];
        long d = 0;
        for (int sh = 0; sh < 24; sh += 8) {
            long diff = (long)((c >> sh) & 255) - (long)((rgb >> sh) & 255);
            d += diff * diff;
        }
        if (d < best_d) { best_d = d; best = c; }
    }
    return best;
}

/* ---- DOS buffers ------------------------------------------------------------------------- */
typedef struct {
    uint32_t lin, plane_size;
    int row, h;
    int page;              /* 0/1 = screen page, -1 = RAM */
} Desc;

static Desc desc(uint32_t d)
{
    Desc r;
    r.row = (int)rw(R.ds, d);
    r.h = (int)rw(R.ds, d + 2);
    r.lin = LIN(rw(R.ds, d + 6), rw(R.ds, d + 4));
    r.plane_size = rw(R.ds, d + 8);
    r.page = r.lin >= 0xA0000 && r.lin < 0xB0000 ? ((r.lin - 0xA0000) >= 0x2000) : -1;
    if (r.page >= 0) { r.row = 40; r.h = 200; }
    return r;
}

/* EGA colour of a pixel of a DOS buffer */
static int dos_pixel(const Desc *d, int x, int y)
{
    uint32_t i = (uint32_t)y * (uint32_t)d->row + (uint32_t)x / 8;
    int c = 0;
    for (int p = 0; p < 4; p++) {
        uint8_t b = d->page >= 0 ? video_plane_byte(p, d->lin - 0xA0000 + i)
                                 : mem[(d->lin + p * d->plane_size + i) & 0xFFFFF];
        c |= ((b >> (7 - x % 8)) & 1) << p;
    }
    return c;
}

static Surface *find_ram(uint32_t lin)
{
    for (int i = 0; i < nsurf; i++) if (surf[i].lin == lin) return &surf[i];
    return NULL;
}

static Surface *new_ram(uint32_t lin, int w, int h)
{
    Surface *s = find_ram(lin);
    if (s && (s->w != w || s->h != h)) { free(s->px); s->px = NULL; }
    if (!s) {
        if (nsurf == MAX_SURF) {
            free(surf[0].px);
            memmove(&surf[0], &surf[1], sizeof(Surface) * (MAX_SURF - 1));
            nsurf--;
        }
        s = &surf[nsurf++];
        s->px = NULL;
    }
    s->lin = lin;
    s->w = w;
    s->h = h;
    s->pal = 0;
    s->scene = 0;
    s->arc = -1;
    s->cycle = 0;
    if (!s->px) s->px = calloc((size_t)w * (size_t)h, 1);
    return s;
}

/* Surface of a descriptor. A RAM buffer that is a copy destination but has
 * no surface gets one made from its EGA contents (palette 0). */
static Surface *surface(const Desc *d, int create)
{
    if (d->page >= 0) return &pages[d->page];
    Surface *s = find_ram(d->lin);
    if (s && s->w == d->row * 8 && s->h == d->h) return s;
    if (!create || d->row <= 0 || d->row > 80 || d->h <= 0 || d->h > 400) return NULL;
    s = new_ram(d->lin, d->row * 8, d->h);
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++) s->px[y * s->w + x] = EGA(dos_pixel(d, x, y));
    return s;
}

static Desc screen(void) { return desc(rw(drv, 0x1E)); }
/* where the driver draws (cs:001E): a screen page or a RAM buffer */
static Surface *drawn_page(void)
{
    Desc d = screen();
    Surface *s = d.page >= 0 ? &pages[d.page] : surface(&d, 1);
    return s ? s : &pages[0];
}

/* ---- loading ----------------------------------------------------------------------------- */
static int choose_palette(const Desc *d, const uint8_t *px, int w, int h)
{
    int best = 0;
    double best_err = 1e30;
    for (int k = 1; k < npals; k++) {
        double err = 0;
        for (int y = 0; y < h && y < d->h; y += 4)
            for (int x = 0; x < w && x < d->row * 8; x += 4) {
                uint32_t a = pals[k][px[y * w + x]], e = video_ega_rgb(dos_pixel(d, x, y));
                for (int sh = 0; sh < 24; sh += 8) {
                    int diff = (int)((a >> sh) & 255) - (int)((e >> sh) & 255);
                    err += diff * diff;
                }
            }
        if (err < best_err) { best_err = err; best = k; }
    }
    return best;
}

static Sprite *find_sprite(uint32_t lin)
{
    for (int i = 0; i < nsprites; i++) if (sprites[i].lin == lin) return &sprites[i];
    return NULL;
}

/* A DOS sprite strip with more frames than the Amiga one (MT.ARC 7, the
 * goat: 8 / 4): the missing frames are made from the DOS frames. The frames both have teach how the
 * DOS pixels become Amiga pens: for each DOS pixel its 5x5, else 3x3,
 * else 1x1 neighbourhood of EGA colours (16 = transparent) and the Amiga
 * pen at the same place. A new frame takes for each pixel the pen of the
 * same detail in a frame both have (a nearly identical 7x7 window close
 * by, so the eye and the muzzle come along), else the most common pen of
 * the largest neighbourhood seen. */
#define SYN_SLOTS 32768
typedef struct { uint64_t key; uint16_t count[32]; } SynSlot;

static int dos_sprite_px(uint32_t lin, int wb, int h, int stride, uint32_t psize, int f, int x, int y)
{
    if (x < 0 || y < 0 || x >= wb * 8 || y >= h) return 16;
    uint32_t i = lin + (uint32_t)(y * stride + f * wb + x / 8);
    int bit = 7 - x % 8;
    if (!((mem[(i + 4 * psize) & 0xFFFFF] >> bit) & 1)) return 16;
    int c = 0;
    for (int p = 0; p < 4; p++) c |= ((mem[(i + p * psize) & 0xFFFFF] >> bit) & 1) << p;
    return c;
}

static uint64_t syn_key(uint32_t lin, int wb, int h, int stride, uint32_t psize, int f, int x, int y, int r)
{
    uint64_t k = 1469598103934665603ull ^ (uint64_t)r;
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            k = (k ^ (uint64_t)dos_sprite_px(lin, wb, h, stride, psize, f, x + dx, y + dy)) * 1099511628211ull;
    return k ? k : 1;
}

static SynSlot *syn_slot(SynSlot *t, uint64_t key, int add)
{
    for (uint32_t i = (uint32_t)(key >> 17) & (SYN_SLOTS - 1);; i = (i + 1) & (SYN_SLOTS - 1)) {
        if (t[i].key == key) return &t[i];
        if (!t[i].key) {
            if (!add) return NULL;
            t[i].key = key;
            return &t[i];
        }
    }
}

static uint8_t *synth_frames(const uint8_t *apx, int afw, int h, int aframes, uint32_t obj)
{
    int wb = rw(R.ds, obj + 4), dh = rw(R.ds, obj + 6), stride = rw(R.ds, obj + 8);
    uint32_t lin = LIN(rw(R.ds, obj + 0xE), rw(R.ds, obj + 0xC)), psize = rw(R.ds, obj + 0x10);
    int dframes = wb ? stride / wb : 0;
    if (dframes <= aframes || dframes > 64 || dh != h || wb * 8 > afw + 16) return NULL;
    SynSlot *t = calloc(SYN_SLOTS, sizeof *t);
    uint8_t *px = calloc((size_t)afw * (size_t)dframes * (size_t)h, 1);
    if (!t || !px) { free(t); free(px); return NULL; }
    int dw = wb * 8;
    for (int f = 0; f < aframes; f++)
        for (int y = 0; y < h; y++)
            for (int x = 0; x < dw && x < afw; x++) {
                uint8_t pen = apx[y * afw * aframes + f * afw + x];
                if (!pen || dos_sprite_px(lin, wb, h, stride, psize, f, x, y) == 16) continue;
                for (int r = 0; r <= 2; r++) {
                    SynSlot *s = syn_slot(t, syn_key(lin, wb, h, stride, psize, f, x, y, r), 1);
                    if (s->count[pen & 31] < 65535) s->count[pen & 31]++;
                }
            }
    for (int f = 0; f < dframes; f++)
        for (int y = 0; y < h; y++)
            for (int x = 0; x < afw; x++) {
                if (f < aframes) { px[y * afw * dframes + f * afw + x] = apx[y * afw * aframes + f * afw + x]; continue; }
                int here = x < dw ? dos_sprite_px(lin, wb, h, stride, psize, f, x, y) : 16;
                if (here == 16) continue;
                /* the same detail in a frame both have (the eye, the
                 * muzzle): the best matching 7x7 DOS window within 6
                 * pixels, nearly exact; its Amiga pen */
                int best_s = -1, bk = 0, bdy = 0, bdx = 0;
                for (int k = 0; k < aframes; k++)
                    for (int dy = -6; dy <= 6; dy++)
                        for (int dx = -6; dx <= 6; dx++) {
                            if (dos_sprite_px(lin, wb, h, stride, psize, k, x + dx, y + dy) != here) continue;
                            int sc = 0;
                            for (int a2 = -3; a2 <= 3; a2++)
                                for (int b2 = -3; b2 <= 3; b2++)
                                    sc += dos_sprite_px(lin, wb, h, stride, psize, f, x + b2, y + a2) ==
                                          dos_sprite_px(lin, wb, h, stride, psize, k, x + dx + b2, y + dy + a2);
                            if (sc > best_s) { best_s = sc; bk = k; bdy = dy; bdx = dx; }
                        }
                if (best_s >= 47 && x + bdx >= 0 && x + bdx < afw && y + bdy >= 0 && y + bdy < h) {
                    uint8_t pen = apx[(y + bdy) * afw * aframes + bk * afw + x + bdx];
                    if (pen) { px[y * afw * dframes + f * afw + x] = pen; continue; }
                }
                for (int r = 2; r >= 0; r--) {
                    SynSlot *s = syn_slot(t, syn_key(lin, wb, h, stride, psize, f, x, y, r), 0);
                    if (!s) continue;
                    int best = 0;
                    for (int c = 1; c < 32; c++) if (s->count[c] > s->count[best]) best = c;
                    if (s->count[best]) { px[y * afw * dframes + f * afw + x] = (uint8_t)best; break; }
                }
            }
    /* a lone pixel the table gave a pen none of its neighbours has (white
     * specks in a body): the most common neighbour pen */
    for (int f = aframes; f < dframes; f++)
        for (int y = 1; y + 1 < h; y++)
            for (int x = 1; x + 1 < afw; x++) {
                uint8_t *c = &px[y * afw * dframes + f * afw + x];
                if (!*c) continue;
                int cnt[32] = {0}, same = 0, n = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        if (!dy && !dx) continue;
                        uint8_t v = px[(y + dy) * afw * dframes + f * afw + x + dx];
                        if (!v) continue;
                        n++;
                        same += v == *c;
                        cnt[v & 31]++;
                    }
                if (n < 7 || same) continue;
                int best = 0;
                for (int k = 1; k < 32; k++) if (cnt[k] > cnt[best]) best = k;
                *c = (uint8_t)best;
            }
    free(t);
    return px;
}

/* The special monkey's head (PS.ARC 32): 6 DOS frames, 5 Amiga ones. The
 * DOS frame 5 is frame 0 with the head one row lower (the lever on top
 * stays) and a tuft of black dots on top. The new frame is made the same
 * way from the Amiga frame 0: its rows 0-3 (the lever) as they are, the
 * lever shaft in row 4, the head one row down; the tuft's black dots (DOS
 * frame 5 rows 3-4, colour 0) one row up, in the Amiga's outline pen. */
static uint8_t *monkey_frames(const uint8_t *apx, int afw, int h, int aframes, uint32_t obj)
{
    int wb = rw(R.ds, obj + 4), dh = rw(R.ds, obj + 6), stride = rw(R.ds, obj + 8);
    uint32_t lin = LIN(rw(R.ds, obj + 0xE), rw(R.ds, obj + 0xC)), psize = rw(R.ds, obj + 0x10);
    if (wb == 0 || stride / wb != 6 || aframes != 5 || dh != h || wb * 8 != afw || h < 6) return NULL;
    int sw = afw * 5, dw = afw * 6;
    uint8_t *px = calloc((size_t)dw * (size_t)h, 1);
    if (!px) return NULL;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < afw; x++) {
            for (int f = 0; f < 5; f++) px[y * dw + f * afw + x] = apx[y * sw + f * afw + x];
            uint8_t pen = 0, a0 = apx[y * sw + x];
            if (y < 4) pen = a0;
            else if (y == 4) pen = (a0 == 1 || a0 == 10) ? a0 : 0;     /* the lever's shaft */
            else pen = apx[(y - 1) * sw + x];
            px[y * dw + 5 * afw + x] = pen;
        }
    for (int y = 3; y <= 4 && y < h; y++)
        for (int x = 0; x < afw; x++)
            if (dos_sprite_px(lin, wb, h, stride, psize, 5, x, y) == 0 && !px[(y - 1) * dw + 5 * afw + x])
                px[(y - 1) * dw + 5 * afw + x] = 31;
    return px;
}

/* 040E:199F, exit of the picture/sprite loader 040E:175C(entry, buffer):
 * AX = descriptor/object, DS:26DA = the entry's header, DS:26EA = file. */
void hook_picture_loaded(void)
{
    if (R.ax && rw(R.ds, 0x26DA + 4) == 1) amigasnd_picture(dos_file_name(rw(R.ds, 0x26EA)), (int)rw(R.ss, R.bp + 6));
    if (!enabled || !R.ax) return;
    uint32_t type = rw(R.ds, 0x26DA + 4);
    if (type != 1 && type != 2) return;
    Archive *a = archive(dos_file_name(rw(R.ds, 0x26EA)));
    int entry = (int)rw(R.ss, R.bp + 6);
    int ae = a && entry >= 0 && entry < 256 ? a->map[entry] : -1;
    size_t el = 0;
    const uint8_t *e = ae >= 0 ? arc_entry(a->amiga, a->alen, ae, &el) : NULL;
    if (e && (uint32_t)hdr16(e, 2) != type) e = NULL;
    if (type == 1) {
        Desc d = desc(R.ax);
        if (d.page >= 0 || d.row <= 0 || d.row > 80) return;
        int w, h;
        uint8_t *px = e ? decode(e, el, &w, &h) : NULL;
        if (!px) {                                 /* no Amiga picture: drop a stale one */
            Surface *s = find_ram(d.lin);
            if (s) s->w = -1;
            return;
        }
        Surface *s = new_ram(d.lin, d.row * 8, d.h);
        if (a->pal[ae] < 0) {
            int forced = picture_override(a->name, ae);
            a->pal[ae] = forced ? forced : a->scene ? a->scene : h < 100 ? a->small_pal : choose_palette(&d, px, w, h);
            if (h >= 100) {
                int vw = w < d.row * 8 ? w : d.row * 8, vh = h < d.h ? h : d.h;
                uint8_t *eg = malloc((size_t)vw * (size_t)vh), *am = malloc((size_t)vw * (size_t)vh);
                for (int y = 0; eg && am && y < vh; y++)
                    for (int x = 0; x < vw; x++) {
                        eg[y * vw + x] = (uint8_t)dos_pixel(&d, x, y);
                        am[y * vw + x] = px[y * w + x];
                    }
                if (eg && am) vote_flat(a->pal[ae], eg, am, vw, vh);
                free(eg);
                free(am);
            }
        }
        s->pal = a->pal[ae];
        s->scene = h >= 100 || a->scene;
        s->arc = (int)(a - arcs);
        s->cycle = same_name("MAIN.ARC", a->name) && ae == 31 ? 1 : 0;
        for (int y = 0; y < s->h; y++)
            for (int x = 0; x < s->w; x++)
                s->px[y * s->w + x] = y < h && x < w ? px[y * w + x] : 0;
        /* Scene pictures end in a 24-row placeholder (one colour and a size
         * label) where the Amiga shows its status bar on black. */
        if (s->h == 200 && s->w == 320) {
            int count[32] = {0}, top = 0;
            for (int i = 176 * 320; i < 200 * 320; i++) count[s->px[i] & 31]++;
            for (int c = 1; c < 32; c++) if (count[c] > count[top]) top = c;
            if (count[top] * 10 >= 24 * 320 * 9) memset(s->px + 176 * 320, 0, 24 * 320);
        }
        free(px);
        return;
    }
    /* sprite strip */
    uint32_t lin = LIN(rw(R.ds, R.ax + 0xE), rw(R.ds, R.ax + 0xC));
    Sprite *sp = find_sprite(lin);
    if (sp) { free(sp->px); free(sp->sub); sp->px = sp->sub = NULL; }
    if (!e) { if (sp) sp->lin = 0; return; }
    int w, h;
    uint8_t *px = decode(e, el, &w, &h);
    int frames = hdr16(e, 5);
    /* A strip shorter than its frames (JG.ARC 16, the water ripples under
     * the hippos: 25 byte columns for 3 x 80 pixels): frames as the header
     * gives them, the missing part transparent. */
    int hw = (hdr16(e, 3) + 7) & ~7;
    if (px && frames > 0 && w % frames && hw * frames > w) {
        uint8_t *full = calloc((size_t)hw * (size_t)frames * (size_t)h + 1, 1);
        for (int y = 0; full && y < h; y++) memcpy(full + (size_t)y * hw * frames, px + (size_t)y * w, (size_t)w);
        free(px);
        px = full;
        w = hw * frames;
    }
    if (!px || frames <= 0 || w % frames) { free(px); if (sp) sp->lin = 0; return; }
    /* missing frames: each sprite its own way */
    uint8_t *more = NULL;
    if (ae == 32 && same_name("PS.ARC", a->name)) more = monkey_frames(px, w / frames, h, frames, R.ax);
    else if (ae == 7 && same_name("MT.ARC", a->name)) more = synth_frames(px, w / frames, h, frames, R.ax);
    if (more) {
        int dframes = rw(R.ds, R.ax + 8) / rw(R.ds, R.ax + 4);
        w = w / frames * dframes;
        frames = dframes;
        free(px);
        px = more;
    }
    if (!sp) {
        if (nsprites == MAX_SPRITES) {
            free(sprites[0].px);
            memmove(&sprites[0], &sprites[1], sizeof(Sprite) * (MAX_SPRITES - 1));
            nsprites--;
        }
        sp = &sprites[nsprites++];
    }
    sp->lin = lin;
    sp->fw = w / frames;
    sp->h = h;
    sp->frames = frames;
    sp->px = px;
    sp->arc = (int)(a - arcs);
    sp->entry = ae;
    sp->hw = hdr16(e, 3);
    sp->hh = hdr16(e, 4);
    sp->sub_arc = -1;
}

/* ---- drawing ----------------------------------------------------------------------------- */
#define ARG(k) rw(R.ss, R.sp + 4 + 2 * (k))

static void put(Surface *s, int x, int y, uint8_t c)
{
    if (x >= 0 && y >= 0 && x < s->w && y < s->h) s->px[y * s->w + x] = c;
}

static void put_ega(Surface *s, int x, int y, int ega) { put(s, x, y, EGA(ega)); }

static void copy_rect(void)
{
    int h = ARG(0), w = ARG(1) & ~7, dy = ARG(2), dx = ARG(3) & ~7, sy = ARG(5), sx = ARG(6) & ~7;
    Desc dd = desc(ARG(4)), sd = desc(ARG(7));
    if (dy + h > 200) h = 200 - dy;
    if (h <= 0) return;
    Surface *dst = surface(&dd, 1), *src = surface(&sd, 0);
    if (!dst) return;
    /* A sprite's buffer the game copies the screen back into (the cave
     * map: its frame sprite is drawn, the rooms are drawn over it and the
     * result is saved there): the sprite takes what was copied inside its
     * shape. Any other copy into a sprite's memory ends the sprite. */
    Sprite *old = dd.page < 0 ? find_sprite(dd.lin) : NULL;
    if (old && old->px && src && old->frames == 1 && dx == 0 && dy == 0 && w == old->fw) {
        for (int y = 0; y < h && y < old->h && sy + y < src->h; y++)
            for (int x = 0; x < w && sx + x < src->w; x++)
                if (old->px[y * old->fw + x]) old->px[y * old->fw + x] = src->px[(sy + y) * src->w + sx + x];
        free(old->sub);
        old->sub = NULL;
        old->sub_arc = -1;
    } else if (old) {
        free(old->px);
        free(old->sub);
        old->px = old->sub = NULL;
        old->lin = 0;
    }
    if (src) {
        if (src->scene) { dst->pal = src->pal; dst->scene = 1; dst->arc = src->arc; dst->cycle = src->cycle; }
        for (int y = 0; y < h; y++) {
            if (sy + y >= src->h || dy + y >= dst->h) break;
            for (int x = 0; x < w && sx + x < src->w && dx + x < dst->w; x++)
                dst->px[(dy + y) * dst->w + dx + x] = src->px[(sy + y) * src->w + sx + x];
        }
    } else {                                       /* EGA buffer without a surface */
        for (int y = 0; y < h && sy + y < sd.h && dy + y < dst->h; y++)
            for (int x = 0; x < w && sx + x < sd.row * 8 && dx + x < dst->w; x++)
                dst->px[(dy + y) * dst->w + dx + x] = EGA(dos_pixel(&sd, sx + x, sy + y));
    }
}

/* The status bar's pictures (money bag, hats) are loaded once from
 * MAIN.ARC; the Amiga minigame archives hold their own versions in the
 * scene's colours, as extra entries the DOS archives do not have. A sprite
 * drawn in such a scene uses the extra entry of the same size; its frames
 * are matched by colour (the jungle's hats are in the reverse order). */
static void frame_colour(const uint8_t *px, int fw, int frames, int h, int f, int pal, double rgb[3])
{
    double n = 0;
    rgb[0] = rgb[1] = rgb[2] = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < fw; x++) {
            uint8_t c = px[y * fw * frames + f * fw + x];
            if (!c) continue;
            for (int k = 0; k < 3; k++) rgb[k] += pals[pal][c] >> (16 - 8 * k) & 255;
            n++;
        }
    for (int k = 0; k < 3 && n; k++) rgb[k] /= n;
}

static int arc_pal(int arc) { return arcs[arc].scene ? arcs[arc].scene : arcs[arc].small_pal; }

static const uint8_t *scene_version(Sprite *sp, int arc)
{
    if (arc < 0 || arc == sp->arc) return NULL;
    if (sp->sub_arc != arc) {
        free(sp->sub);
        sp->sub = NULL;
        sp->sub_arc = arc;
        Archive *a = &arcs[arc];
        uint8_t used[256] = {0};
        for (int i = 0; i < 256; i++) if (a->map[i] >= 0 && a->map[i] < 256) used[a->map[i]] = 1;
        for (int j = 0; j < a->an && j < 256; j++) {
            size_t el;
            const uint8_t *e = arc_entry(a->amiga, a->alen, j, &el);
            if (used[j] || !e || hdr16(e, 2) != 2 || hdr16(e, 3) != sp->hw || hdr16(e, 4) != sp->hh ||
                hdr16(e, 5) != sp->frames) continue;
            int w, h;
            uint8_t *px = decode(e, el, &w, &h);
            if (px && w == sp->fw * sp->frames && h == sp->h) {
                sp->sub = px;
                /* Each frame of the DOS picture gets the Amiga frame of the
                 * same colour: the DOS game picks a nephew's hat by frame,
                 * and the jungle's Amiga strip has its frames in another
                 * order. Pairing of frames with the closest hues (colour
                 * minus its grey level), tried over all orders (up to 4). */
                for (int f = 0; f < 64; f++) sp->sub_frame[f] = (uint8_t)f;
                if (sp->frames > 1 && sp->frames <= 4) {
                    double want[4][3], got[4][3];
                    for (int f = 0; f < sp->frames; f++) {
                        frame_colour(sp->px, sp->fw, sp->frames, sp->h, f, arc_pal(sp->arc), want[f]);
                        frame_colour(px, sp->fw, sp->frames, sp->h, f, arc_pal(arc), got[f]);
                        double gw = (want[f][0] + want[f][1] + want[f][2]) / 3, gg = (got[f][0] + got[f][1] + got[f][2]) / 3;
                        for (int c = 0; c < 3; c++) { want[f][c] -= gw; got[f][c] -= gg; }
                    }
                    int perm[4] = {0, 1, 2, 3}, best[4] = {0, 1, 2, 3};
                    double best_d = 1e30;
                    for (int p0 = 0; p0 < 256; p0++) {
                        int used_k = 0, ok = 1;
                        for (int f = 0; f < sp->frames; f++) {
                            perm[f] = p0 >> (2 * f) & 3;
                            if (perm[f] >= sp->frames || (used_k >> perm[f] & 1)) ok = 0;
                            used_k |= 1 << perm[f];
                        }
                        if (!ok || (p0 >> (2 * sp->frames))) continue;
                        double d = 0;
                        for (int f = 0; f < sp->frames; f++)
                            for (int c = 0; c < 3; c++) d += (want[f][c] - got[perm[f]][c]) * (want[f][c] - got[perm[f]][c]);
                        if (d < best_d) { best_d = d; memcpy(best, perm, sizeof best); }
                    }
                    for (int f = 0; f < sp->frames; f++) sp->sub_frame[f] = (uint8_t)best[f];
                }
                break;
            }
            free(px);
        }
    }
    return sp->sub;
}

static void sprite(void)
{
    uint32_t o = ARG(0);
    int x = (int16_t)rw(R.ds, o), y = (int16_t)rw(R.ds, o + 2);
    int wb = rw(R.ds, o + 4), h = rw(R.ds, o + 6), stride = rw(R.ds, o + 8);
    uint32_t lin = LIN(rw(R.ds, o + 0xE), rw(R.ds, o + 0xC)), psize = rw(R.ds, o + 0x10);
    int frame = rb(R.ds, o + 0x12);
    Surface *dst = drawn_page();
    Sprite *sp = find_sprite(lin);
    /* The money bag (MAIN.ARC 23) in the mountain and the jungle: the Amiga
     * game shows it with its pens moved to scene colours (seen in its
     * screens; the coin pile is drawn over it by the game). */
    static const struct { const char *arc; uint8_t pen[32]; } bag_pens[] = {
        {"MT.ARC", {0, 1, 0, 0, 6, 0, 11, 12, 0, 26, 0, 24, 22, 21, 0, 0, 0, 17, 8, 24, 0, 10, 0, 21, 0, 0, 0, 0, 0, 27, 0, 7}},
        {"JG.ARC", {0, 1, 0, 0, 8, 0, 30, 29, 0, 26, 0, 24, 22, 21, 0, 0, 0, 14, 15, 24, 0, 13, 0, 21, 0, 0, 0, 0, 0, 15, 0, 9}},
    };
    const uint8_t *bag = NULL;
    if (sp && sp->px && sp->entry == 23 && dst->arc >= 0 && same_name("MAIN.ARC", arcs[sp->arc].name))
        for (size_t k = 0; k < sizeof bag_pens / sizeof bag_pens[0]; k++)
            if (same_name(bag_pens[k].arc, arcs[dst->arc].name)) bag = bag_pens[k].pen;
    if (bag && frame < sp->frames) {
        /* the money bar (pen 9) has a frame of its own colour on the Amiga
         * (jungle: blue around red, mountain: light blue around blue) */
        int jungle = same_name("JG.ARC", arcs[dst->arc].name);
        uint8_t edge = jungle ? 26 : 25, fill = jungle ? 21 : 26;
        const uint8_t *f0 = sp->px + frame * sp->fw;
        int stride = sp->fw * sp->frames;
        for (int yy = 0; yy < sp->h; yy++)
            for (int xx = 0; xx < sp->fw; xx++) {
                uint8_t c = f0[yy * stride + xx];
                if (!c) continue;
                uint8_t o = bag[c];
                if (c == 9) {
                    int border = 0;
                    for (int d = 0; d < 4; d++) {
                        int nx = xx + (d == 0) - (d == 1), ny = yy + (d == 2) - (d == 3);
                        if (nx < 0 || ny < 0 || nx >= sp->fw || ny >= sp->h || f0[ny * stride + nx] != 9) border = 1;
                    }
                    o = border ? edge : fill;
                }
                put(dst, x + xx, y + yy, o);
            }
        return;
    }
    /* another picture of another archive without a version for this
     * minigame: its DOS picture in the scene's colours */
    int foreign = sp && dst->arc >= 0 && sp->arc != dst->arc && arcs[dst->arc].scene &&
                  !scene_version(sp, dst->arc);
    /* frames the Amiga strip does not have (when they could not be made) */
    if (sp && sp->px && frame >= sp->frames && frame < 64 && !foreign) frame %= sp->frames;
    if (sp && sp->px && frame < sp->frames && !foreign) {
        const uint8_t *px = scene_version(sp, dst->arc);
        if (px && frame < 64) frame = sp->sub_frame[frame];
        if (!px) px = sp->px;
        for (int yy = 0; yy < sp->h; yy++)
            for (int xx = 0; xx < sp->fw; xx++) {
                uint8_t c = px[yy * sp->fw * sp->frames + frame * sp->fw + xx];
                int ry = y + yy;
                if (!c || ry < 0 || ry >= dst->h) continue;
                put(dst, x + xx, ry, c);
            }
        return;
    }
    /* the EGA sprite: 4 planes and a mask plane. A buffer the game saved
     * from the screen (the cave map) has its surface: the pixel markers
     * drawn into it keep their colours. */
    Surface *ram = find_ram(lin);
    if (ram && ram->w != stride * 8) ram = NULL;
    for (int yy = 0; yy < h; yy++)
        for (int xb = 0; xb < wb; xb++) {
            uint32_t i = lin + (uint32_t)(yy * stride + frame * wb + xb);
            uint8_t m = mem[(i + 4 * psize) & 0xFFFFF];
            for (int bit = 0; bit < 8; bit++) {
                if (!((m >> (7 - bit)) & 1)) continue;
                int c = 0;
                for (int p = 0; p < 4; p++) c |= ((mem[(i + p * psize) & 0xFFFFF] >> (7 - bit)) & 1) << p;
                int sx = (frame * wb + xb) * 8 + bit;
                uint8_t v = ram && yy < ram->h ? ram->px[yy * ram->w + sx] : 0;
                if ((v & 0xF0) == 0x30 && (v & 15) == c) put(dst, x + xb * 8 + bit, y + yy, v);
                else put_ega(dst, x + xb * 8 + bit, y + yy, c);
            }
        }
}

static void text(int with_bg)
{
    int colour = ARG(0), y = ARG(1), x = ARG(2);
    uint32_t str = ARG(3), font = LIN(ARG(5), ARG(4));
    Desc sd = screen();
    int pg = sd.page > 0;
    if (rb(R.ds, rw(drv, 0x22))) pg ^= 1;             /* on the shown page */
    Surface *dst = &pages[pg];
    int adv = mem[(font + 2) & 0xFFFFF], fh = mem[(font + 4) & 0xFFFFF];
    int fg = (with_bg ? colour >> 8 : colour) & 15, bg = colour & 15;
    /* the status bar's money (EGA yellow) in the minigames: white, as the
     * Amiga shows them (the learned EGA yellow is a beige in the jungle) */
    int white = !with_bg && fg == 14 && y >= 176 && dst->arc >= 0 && arcs[dst->arc].scene;
    for (int n = 0; n < 200; n++) {
        int ch = rb(R.ds, str + (uint32_t)n);
        if (!ch) break;
        for (int r = 0; r < fh; r++) {
            uint8_t bits = mem[(font + 8 + (uint32_t)(ch * fh + r)) & 0xFFFFF];
            for (int b = 0; b < (with_bg ? adv : 8); b++) {
                int on = b < 8 && ((bits >> (7 - b)) & 1);
                if (on && white) put(dst, x + b, y + r, 1);
                else if (on) put_ega(dst, x + b, y + r, fg);
                else if (with_bg) put_ega(dst, x + b, y + r, bg);
            }
        }
        x += adv;
    }
}

/* 069B (y2, y1, x) vertical, 073F (y, x2, x1) horizontal */
static void line(int vertical)
{
    Surface *dst = drawn_page();
    int c = rb(drv, 0x24) & 15;
    int a = (int16_t)ARG(vertical ? 0 : 1), b = (int16_t)ARG(vertical ? 1 : 2);
    int p = (int16_t)ARG(vertical ? 2 : 0);
    if (a > b) { int t = a; a = b; b = t; }
    for (int i = a; i <= b; i++) {
        if (vertical) put_ega(dst, p, i, c);
        else put_ega(dst, i, p, c);
    }
}

/* 0101: the drawn page to the shown one, 0155: clear the shown page; both
 * in 5 interlaced passes with 5 frames between them. Done here for the EGA
 * planes too, so the effect is seen. */
static void passes(int clear)
{
    Desc sd = screen();
    int from = sd.page > 0, to = from ^ 1;
    uint32_t fo = from * 0x2000u, to_off = to * 0x2000u;
    for (int pass = 0; pass < 5; pass++) {
        for (int y = pass; y < 200; y += 5) {
            if (clear) {
                video_plane_fill(to_off + y * 40u, 40, 0);
                memset(pages[to].px + y * 320, EGA(0), 320);
            } else {
                video_plane_copy(to_off + y * 40u, fo + y * 40u, 40);
                memcpy(pages[to].px + y * 320, pages[from].px + y * 320, 320);
                if (pass == 0 && y == 0) { pages[to].pal = pages[from].pal; pages[to].arc = pages[from].arc; pages[to].cycle = pages[from].cycle; }
            }
        }
        wait_frames(5);
    }
}

/* 0589: lines in any direction (the climbers' ropes), drawn by code that
 * rewrites itself; the EGA driver draws them and the pixels it changed are
 * copied (amiga_driver_after). */
static uint8_t *line_before;
static Desc line_desc;

static uint8_t plane_byte(const Desc *d, int p, uint32_t i)
{
    return d->page >= 0 ? video_plane_byte(p, d->lin - 0xA0000 + i) : mem[(d->lin + p * d->plane_size + i) & 0xFFFFF];
}

static void line_snapshot(void)
{
    line_desc = screen();
    uint32_t n = (uint32_t)line_desc.row * (uint32_t)line_desc.h;
    free(line_before);
    line_before = n && n <= 0x10000 ? malloc(4 * n) : NULL;
    for (int p = 0; line_before && p < 4; p++)
        for (uint32_t i = 0; i < n; i++) line_before[p * n + i] = plane_byte(&line_desc, p, i);
}

void amiga_driver_after(uint32_t off)
{
    if (!enabled || off != 0x589 || !line_before) return;
    Desc *d = &line_desc;
    Surface *s = d->page >= 0 ? &pages[d->page] : surface(d, 1);
    uint32_t n = (uint32_t)d->row * (uint32_t)d->h;
    for (uint32_t i = 0; s && i < n; i++) {
        int changed = 0;
        for (int p = 0; p < 4; p++) if (plane_byte(d, p, i) != line_before[p * n + i]) changed = 1;
        if (!changed) continue;
        int y = (int)(i / (uint32_t)d->row), x0 = (int)(i % (uint32_t)d->row) * 8;
        for (int b = 0; b < 8; b++) {
            int before = 0;
            for (int p = 0; p < 4; p++) before |= ((line_before[p * n + i] >> (7 - b)) & 1) << p;
            int now = dos_pixel(d, x0 + b, y);
            if (now != before) put_ega(s, x0 + b, y, now);
        }
    }
    free(line_before);
    line_before = NULL;
}

/* Before each call of the EGA driver (module 1) at drv_seg:off, with its
 * return address on the stack. Returns 1 when the call has been carried out
 * here completely (and returned from). */
int amiga_driver(uint32_t drv_seg, uint32_t off)
{
    if (!enabled) return 0;
    drv = drv_seg;
    switch (off) {
    case 0x101: case 0x155:
        passes(off == 0x155);
        pop16();
        pop16();                                   /* retf */
        return 1;
    case 0x1B0: {
        Surface *s = drawn_page();
        memset(s->px, EGA(ARG(0)), (size_t)s->w * (size_t)s->h);
        break;
    }
    case 0x1EF:
        copy_rect();
        break;
    case 0x35D: sprite(); break;
    case 0x515: {
        Surface *s = drawn_page();
        put(s, (int16_t)ARG(1), (int16_t)ARG(0), EGA_PIXEL(rb(drv, 0x24)));
        break;
    }
    case 0x589: line_snapshot(); break;
    case 0x69B: line(1); break;
    case 0x73F: line(0); break;
    case 0x81F: text(0); break;
    case 0x909: text(1); break;
    default: break;
    }
    return 0;                                      /* the EGA driver runs as well */
}

/* ---- display ----------------------------------------------------------------------------- */
void amiga_render(uint32_t start_addr, uint32_t *pixels, int W, int H, int dark)
{
    /* one palette for the whole screen, as the Amiga game shows it */
    Surface *s = &pages[start_addr >= 0x2000];
    uint32_t lut[256];
    for (int c = 0; c < 256; c++)
        lut[c] = dark ? 0xFF000000u : c < 32 ? pals[s->pal][c] : ega_argb(s->pal, c & 31);
    /* The climbing and jungle games show the nephew playing in his colour:
     * the DOS game sets EGA colour 5 (and 13) to it, the Amiga game colours
     * 17-20 (mountain node 0x23E, jungle node 0x14E4). */
    static const uint16_t nephew[3][4] = {
        {0x062, 0x082, 0x0B3, 0x4D8}, {0x005, 0x008, 0x11C, 0x44D}, {0x500, 0x800, 0xC00, 0xD44}};
    const char *arc = s->arc >= 0 ? arcs[s->arc].name : "";
    uint32_t c5 = video_ega_rgb(5);
    int r = c5 >> 16 & 255, g = c5 >> 8 & 255, b = c5 & 255;
    int who = g > r && g > b ? 0 : b > r && b > g ? 1 : r > g && r > b ? 2 : -1;
    if (!dark && who >= 0 && (same_name("MT.ARC", arc) || same_name("JG.ARC", arc)))
        for (int k = 0; k < 4; k++) {
            uint16_t w = nephew[who][k];
            lut[17 + k] = 0xFF000000u | ((w >> 8) & 15) * 0x110000u | ((w >> 4) & 15) * 0x1100u | (w & 15) * 0x11u;
        }
    /* the status bar's frame (EGA brown, 6): the Amiga's jungle draws it in
     * pen 17 (dark green), seen in its screen */
    if (!dark && same_name("JG.ARC", arc)) lut[EGA(6)] = pals[s->pal][17];
    /* The Amiga game's colour cycling (its 50 Hz interrupt, 0x61A0: table
     * at data 0x2BFE): the campfire's flames (mode 4) turn colours 28-31
     * through a list, one step per 127/15 frames. */
    static const struct { int first, count; uint16_t list[8]; } cycles[] = {
        {0, 0, {0}}, {28, 4, {0xF00, 0xF70, 0xFF7, 0x000, 0xF00, 0xF70, 0xFF7}}};
    if (!dark && s->cycle) {
        const int c = s->cycle;
        int step = (int)(SDL_GetTicks() * 15u / (127u * 20u)) % cycles[c].count;
        int i = cycles[c].count - 1 - step;              /* the index counts down */
        for (int k = 0; k < cycles[c].count; k++) {
            uint16_t w = cycles[c].list[i + k];
            lut[cycles[c].first + k] = 0xFF000000u | ((w >> 8) & 15) * 0x110000u | ((w >> 4) & 15) * 0x1100u | (w & 15) * 0x11u;
        }
    }
    for (int y = 0; y < H; y++) {
        int sy = y * 200 / H;
        const uint8_t *row = s->px + sy * 320;
        for (int x = 0; x < W; x++) pixels[y * W + x] = lut[row[x * 320 / W]];
    }
}

int amiga_active(void) { return enabled; }

void amiga_init(void)
{
    if (settings.graphics != GFX_AMIGA) return;
    load_palettes();
    if (npals < 2) fprintf(stderr, "Amiga graphics: Amiga game not found in %s\n", settings.amiga_dir);
    for (int i = 0; i < 2; i++) {
        pages[i].lin = 0xA0000 + i * 0x2000u;
        pages[i].w = 320;
        pages[i].h = 200;
        pages[i].px = calloc(320 * 200, 1);
    }
    enabled = 1;
}
