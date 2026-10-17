/* Amiga disk image extraction.
 *
 * ADF: a plain sector dump, read as is.
 * IPF: decoded with the SPS decoder library (CAPSImg.dll / libcapsimage),
 * loaded at run time. It yields the raw MFM bit stream of every track,
 * from which the standard AmigaDOS sectors are decoded here.
 *
 * The AmigaDOS file system (OFS or FFS) is then walked and every file is
 * written below outdir/<volume name>/.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define MKDIR(p) _mkdir(p)
#else
#include <dlfcn.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

#include "diskimage.h"

#define BSIZE 512
#define TRACKS 160
#define SECTORS 11

static uint32_t be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* ---- IPF via CAPSImg ------------------------------------------------------------- */

/* From capsimage.h (CAPSImg 4.2/5.1). */
#define DI_LOCK_INDEX   (1u << 0)
#define DI_LOCK_DENVAR  (1u << 2)
#define DI_LOCK_UPDATEFD (1u << 8)
#define DI_LOCK_TYPE    (1u << 9)
#define DI_LOCK_TRKBIT  (1u << 12)

#pragma pack(push, 1)   /* CapsAPI.h packs its structures */
typedef struct {        /* CapsTrackInfoT1 */
    uint32_t type, cylinder, head, sectorcnt, sectorsize;
    unsigned char *trackbuf;
    uint32_t tracklen;  /* bits with DI_LOCK_TRKBIT */
    uint32_t timelen;
    uint32_t *timebuf;
    int32_t overlap;
} CapsTrackInfoT1;
#pragma pack(pop)

typedef int32_t (*fn_void)(void);
typedef int32_t (*fn_id)(int32_t);
typedef int32_t (*fn_lockimage)(int32_t, const char *);
typedef int32_t (*fn_loadimage)(int32_t, uint32_t);
typedef int32_t (*fn_locktrack)(void *, int32_t, uint32_t, uint32_t, uint32_t);
typedef int32_t (*fn_unlocktrack)(int32_t, uint32_t, uint32_t);

static void *lib_open(void)
{
#ifdef _WIN32
    /* next to the program first, then the normal DLL search path */
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, sizeof path);
    while (n > 0 && path[n - 1] != '\\' && path[n - 1] != '/') n--;
    snprintf(path + n, sizeof path - n, "CAPSImg.dll");
    HMODULE h = LoadLibraryA(path);
    if (!h) h = LoadLibraryA("CAPSImg.dll");
    return (void *)h;
#else
    void *h = dlopen("libcapsimage.so.5", RTLD_NOW);
    if (!h) h = dlopen("libcapsimage.so", RTLD_NOW);
    if (!h) h = dlopen("libcapsimage.dylib", RTLD_NOW);
    return h;
#endif
}

static void *lib_sym(void *lib, const char *name)
{
#ifdef _WIN32
    return (void *)GetProcAddress((HMODULE)lib, name);
#else
    return dlsym(lib, name);
#endif
}

/* Bit i of a circular MFM bit stream. */
static unsigned mfm_bit(const unsigned char *bits, uint32_t nbits, uint32_t i)
{
    i %= nbits;
    return (bits[i >> 3] >> (7 - (i & 7))) & 1;
}

/* 32 MFM bits starting at bit position pos. */
static uint32_t mfm_long(const unsigned char *bits, uint32_t nbits, uint32_t pos)
{
    uint32_t v = 0;
    for (int b = 0; b < 32; b++) v = (v << 1) | mfm_bit(bits, nbits, pos + (uint32_t)b);
    return v;
}

/* Amiga odd/even MFM pair -> data long. */
static uint32_t mfm_decode(uint32_t odd, uint32_t even)
{
    return ((odd & 0x55555555u) << 1) | (even & 0x55555555u);
}

/* Decode the AmigaDOS sectors of one raw MFM track into adf. Returns the
 * number of good sectors found. Sector layout after the 0x4489 0x4489 sync:
 * info (2 longs), label (8), header checksum (2), data checksum (2), data
 * odd bits (128), data even bits (128). */
static int decode_track(const unsigned char *bits, uint32_t nbits, int track, unsigned char *adf)
{
    int found = 0;
    unsigned seen = 0;
    uint32_t shift = 0;
    /* scan the track twice so sectors across the index are found too */
    for (uint32_t i = 0; i < nbits * 2 && found < SECTORS; i++) {
        shift = (shift << 1) | mfm_bit(bits, nbits, i);
        if (shift != 0x44894489u) continue;
        uint32_t p = i + 1;
        uint32_t L[14];
        for (int k = 0; k < 14; k++) L[k] = mfm_long(bits, nbits, p + 32u * (uint32_t)k);
        uint32_t info = mfm_decode(L[0], L[1]);
        int fmt = (int)(info >> 24), trk = (int)((info >> 16) & 0xFF), sec = (int)((info >> 8) & 0xFF);
        if (fmt != 0xFF || trk != track || sec >= SECTORS || (seen & (1u << sec))) continue;
        uint32_t hsum = 0;
        for (int k = 0; k < 10; k++) hsum ^= L[k];
        if ((hsum & 0x55555555u) != mfm_decode(L[10], L[11])) continue;
        uint32_t dsum = 0;
        unsigned char *out = adf + ((size_t)track * SECTORS + (size_t)sec) * BSIZE;
        for (int k = 0; k < 128; k++) {
            uint32_t odd = mfm_long(bits, nbits, p + 32u * (14u + (uint32_t)k));
            uint32_t even = mfm_long(bits, nbits, p + 32u * (142u + (uint32_t)k));
            dsum ^= odd ^ even;
            uint32_t v = mfm_decode(odd, even);
            out[4 * k] = (unsigned char)(v >> 24);
            out[4 * k + 1] = (unsigned char)(v >> 16);
            out[4 * k + 2] = (unsigned char)(v >> 8);
            out[4 * k + 3] = (unsigned char)v;
        }
        if ((dsum & 0x55555555u) != mfm_decode(L[12], L[13])) continue;
        seen |= 1u << sec;
        found++;
    }
    return found;
}

static int read_ipf(const char *path, unsigned char *adf, char *err, size_t errlen)
{
    void *lib = lib_open();
    if (!lib) {
        snprintf(err, errlen, "IPF needs CAPSImg.dll next to the program");
        return 0;
    }
    fn_void init = (fn_void)lib_sym(lib, "CAPSInit"), add = (fn_void)lib_sym(lib, "CAPSAddImage");
    fn_lockimage lockimg = (fn_lockimage)lib_sym(lib, "CAPSLockImage");
    fn_loadimage loadimg = (fn_loadimage)lib_sym(lib, "CAPSLoadImage");
    fn_locktrack locktrk = (fn_locktrack)lib_sym(lib, "CAPSLockTrack");
    fn_unlocktrack unlocktrk = (fn_unlocktrack)lib_sym(lib, "CAPSUnlockTrack");
    fn_id unlockimg = (fn_id)lib_sym(lib, "CAPSUnlockImage"), rem = (fn_id)lib_sym(lib, "CAPSRemImage");
    if (!init || !add || !lockimg || !loadimg || !locktrk || !unlocktrk || !unlockimg || !rem) {
        snprintf(err, errlen, "CAPSImg.dll is missing functions");
        return 0;
    }
    init();
    int32_t id = add();
    if (id < 0 || lockimg(id, path) != 0) {
        snprintf(err, errlen, "CAPSImg cannot open the image");
        if (id >= 0) rem(id);
        return 0;
    }
    loadimg(id, DI_LOCK_INDEX | DI_LOCK_DENVAR);
    int bad_track = -1;
    for (int t = 0; t < TRACKS; t++) {
        CapsTrackInfoT1 ti;
        memset(&ti, 0, sizeof ti);
        ti.type = 1;
        if (locktrk(&ti, id, (uint32_t)t / 2, (uint32_t)t % 2,
                    DI_LOCK_INDEX | DI_LOCK_DENVAR | DI_LOCK_UPDATEFD | DI_LOCK_TYPE | DI_LOCK_TRKBIT) != 0
            || !ti.trackbuf || !ti.tracklen
            || decode_track(ti.trackbuf, ti.tracklen, t, adf) != SECTORS) {
            if (bad_track < 0) bad_track = t;
        }
        unlocktrk(id, (uint32_t)t / 2, (uint32_t)t % 2);
    }
    unlockimg(id);
    rem(id);
    if (bad_track >= 0) {
        snprintf(err, errlen, "IPF track %d could not be decoded", bad_track);
        return 0;
    }
    return 1;
}

int disk_read_image(const char *path, unsigned char *adf, char *err, size_t errlen)
{
    size_t len = strlen(path);
    const char *ext = len >= 4 ? path + len - 4 : "";
    memset(adf, 0, ADF_SIZE);
    if (!strcmp(ext, ".ipf") || !strcmp(ext, ".IPF") || !strcmp(ext, ".Ipf"))
        return read_ipf(path, adf, err, errlen);
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "cannot open the file");
        return 0;
    }
    size_t n = fread(adf, 1, ADF_SIZE, f);
    fclose(f);
    if (n != ADF_SIZE) {
        snprintf(err, errlen, "not an ADF (size %u)", (unsigned)n);
        return 0;
    }
    return 1;
}

/* ---- AmigaDOS file system ----------------------------------------------------------- */

typedef struct {
    const unsigned char *adf;
    int ffs;
    int files;
    char *err;
    size_t errlen;
} Fs;

static const unsigned char *block(const Fs *fs, uint32_t n)
{
    return n < ADF_SIZE / BSIZE ? fs->adf + (size_t)n * BSIZE : NULL;
}

/* BSTR name at offset 432 of header blocks, made safe for the host. */
static void entry_name(const unsigned char *b, char *out, size_t max)
{
    int len = b[432];
    if (len > 30) len = 30;
    size_t o = 0;
    for (int i = 0; i < len && o + 1 < max; i++) {
        char c = (char)b[433 + i];
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|' || (unsigned char)c < 32)
            c = '_';
        out[o++] = c;
    }
    out[o] = 0;
    if (!strcmp(out, ".") || !strcmp(out, "..") || !out[0]) snprintf(out, max, "_");
}

static void make_dirs(const char *path)
{
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/' || *p == '\\') {
            char c = *p;
            *p = 0;
            MKDIR(tmp);
            *p = c;
        }
    MKDIR(tmp);
}

static int write_file(Fs *fs, const unsigned char *hdr, const char *path)
{
    uint32_t size = be32(hdr + 324), done = 0;
    FILE *f = fopen(path, "wb");
    if (!f) {
        snprintf(fs->err, fs->errlen, "cannot write %s", path);
        return 0;
    }
    /* data block pointers: header and extension blocks list them from
     * offset 308 downwards (72 per block), extension at offset 504 */
    const unsigned char *list = hdr;
    int guard = 0;
    while (list && done < size && guard++ < 2000) {
        uint32_t count = be32(list + 8);
        for (uint32_t i = 0; i < count && i < 72 && done < size; i++) {
            const unsigned char *d = block(fs, be32(list + 308 - 4 * i));
            if (!d) break;
            const unsigned char *data = fs->ffs ? d : d + 24;
            uint32_t n = fs->ffs ? BSIZE : be32(d + 12);
            if (n > 488 && !fs->ffs) n = 488;
            if (n > size - done) n = size - done;
            fwrite(data, 1, n, f);
            done += n;
        }
        uint32_t ext = be32(list + 504);
        list = ext ? block(fs, ext) : NULL;
    }
    fclose(f);
    fs->files++;
    return 1;
}

/* Walk a directory's hash table (root or user directory block). */
static int walk_dir(Fs *fs, const unsigned char *dir, const char *path, int depth)
{
    if (depth > 20) return 1;
    for (int h = 0; h < 72; h++) {
        uint32_t n = be32(dir + 24 + 4 * h);
        int guard = 0;
        while (n && guard++ < 1000) {
            const unsigned char *b = block(fs, n);
            if (!b || be32(b) != 2) break;               /* T_HEADER */
            char name[64], sub[1200];
            entry_name(b, name, sizeof name);
            snprintf(sub, sizeof sub, "%s/%s", path, name);
            int32_t sec = (int32_t)be32(b + 508);
            if (sec == 2) {                               /* ST_USERDIR */
                make_dirs(sub);
                if (!walk_dir(fs, b, sub, depth + 1)) return 0;
            } else if (sec == -3) {                       /* ST_FILE */
                if (!write_file(fs, b, sub)) return 0;
            }
            n = be32(b + 496);                            /* hash chain */
        }
    }
    return 1;
}

int disk_extract(const unsigned char *adf, const char *outdir, char *volume, size_t vollen,
                 char *err, size_t errlen)
{
    if (memcmp(adf, "DOS", 3) != 0) {
        snprintf(err, errlen, "not an AmigaDOS disk");
        return -1;
    }
    Fs fs = {adf, adf[3] & 1, 0, err, errlen};
    const unsigned char *root = block(&fs, 880);
    if (be32(root) != 2 || be32(root + 508) != 1) {
        snprintf(err, errlen, "AmigaDOS root block not found");
        return -1;
    }
    entry_name(root, volume, vollen);
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", outdir, volume);
    make_dirs(path);
    if (!walk_dir(&fs, root, path, 0)) return -1;
    return fs.files;
}
