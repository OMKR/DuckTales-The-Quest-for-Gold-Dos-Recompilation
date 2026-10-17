/* DuckTales: The Quest for Gold (DOS) - settings program.
 *
 * A small stand-alone window that edits ducktales_dos.ini (see
 * runtime/settings86.h) and starts the game. Keyboard, mouse and game
 * controller all work: up/down choose, left/right change, Enter selects.
 * A game folder dropped on the window becomes the game folder.
 */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "settings86.h"
#include "diskimage.h"
#include "font8x8_basic.h"
#include "font8x8_ext_latin.h"

#define W 720
#define H 660          /* menu rows end ~576; edit box and help line below */
#define CHAR 16        /* 8x8 font drawn at 2x */
#define LINE 20
#define TOP 60
#define LEFT 40

#ifdef _WIN32
#define GAME_EXE "ducktales_dos.exe"
#else
#define GAME_EXE "ducktales_dos"
#endif

static SDL_Renderer *ren;
static Settings cfg;
static char status[128];
static int editing_dir;
static char dir_edit[512];
static int status_ok;           /* status line shown in green */

enum {
    I_SPEED, I_FPS, I_MUSIC, I_GFX, I_SOUND, I_SPEECH, I_VOLUME, I_FULL, I_SCALE, I_JOY, I_DEAD, I_PAD, I_ORDER, I_PROT, I_DIR, I_ADIR, I_IMPORT,
    I_BUILD, I_ABUILD, I_DEFAULTS, I_START, I_SAVE, I_QUIT, I_COUNT
};

static const char *const labels[I_COUNT] = {
    "Game speed", "Frame rate limit (fps)", "Music speed", "Graphics", "Sound", "Amiga speech", "Volume", "Fullscreen", "Window size", "Controls",
    "Controller dead zone", "Keyboard mode", "Jungle nephew order", "Copy protection",
    "Game folder", "Amiga game folder", "Import Amiga disk image (.adf/.ipf)",
    "Build game from game files", "Add Amiga sound",
    "Restore defaults", "Save and start game", "Save and quit", "Quit without saving",
};

static const char *const sound_names[] = {"off", "PC speaker", "Tandy", "Amiga"};
static const char *const gfx_names[] = {"EGA 16 colours", "CGA 4 colours", "Tandy 16 colours",
                                        "Hercules (test)", "Amiga 32 colours"};

/* ---- text ----------------------------------------------------------------------- */

static const char *glyph(unsigned cp)
{
    if (cp < 128) return font8x8_basic[cp];
    if (cp >= 0xA0 && cp <= 0xFF) return font8x8_ext_latin[cp - 0xA0];
    return font8x8_basic['?'];
}

static unsigned next_cp(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    unsigned cp = *p++;
    if (cp >= 0xC0 && cp < 0xE0 && (*p & 0xC0) == 0x80) cp = ((cp & 0x1F) << 6) | (*p++ & 0x3F);
    *s = (const char *)p;
    return cp;
}

static int text_width(const char *s)
{
    int n = 0;
    while (*s) { next_cp(&s); n++; }
    return n * CHAR;
}

/* px = size of one font pixel (2 = normal text, 1 = small help line) */
static void draw_text_px(int x, int y, const char *s, SDL_Color c, int px)
{
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, 255);
    while (*s) {
        const char *g = glyph(next_cp(&s));
        for (int row = 0; row < 8; row++)
            for (int col = 0; col < 8; col++)
                if ((g[row] >> col) & 1) {
                    SDL_Rect r = {x + col * px, y + row * px, px, px};
                    SDL_RenderFillRect(ren, &r);
                }
        x += 8 * px;
    }
}

static void draw_text(int x, int y, const char *s, SDL_Color c)
{
    draw_text_px(x, y, s, c, 2);
}

/* ---- settings ---------------------------------------------------------------------- */

static char base_dir[1024];

static int file_in(const char *dir, const char *name)
{
    char path[1600];
    struct stat st;
    int absolute = dir[0] == '/' || dir[0] == '\\' || (dir[0] && dir[1] == ':');
    snprintf(path, sizeof path, "%s%s/%s", absolute ? "" : base_dir, dir, name);
    if (stat(path, &st) == 0) return 1;
    /* case-insensitive file systems aside, try the lower case name too */
    char lower[64];
    size_t i = 0;
    for (; name[i] && i < sizeof lower - 1; i++)
        lower[i] = (char)(name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 32 : name[i]);
    lower[i] = 0;
    snprintf(path, sizeof path, "%s%s/%s", absolute ? "" : base_dir, dir, lower);
    return stat(path, &st) == 0;
}

/* The game folder holds the original DOS files (resolved like the game
 * does: relative to the program folder). */
static int gamedir_ok(const char *dir)
{
    return file_in(dir, "DUCKTALE.EXE") && file_in(dir, "IT.DRV");
}

/* folder of the Amiga version (DT1/DuckTales, as the Amiga port uses) */
static int amigadir_ok(const char *dir)
{
    return file_in(dir, "DT1/DuckTales");
}

/* the Amiga sound code was built into the game (the installer leaves its
 * generated layout header) */
static int amiga_sound_built(void)
{
    char path[1200];
    struct stat st;
    snprintf(path, sizeof path, "%sbuild_tmp/gen/amigasnd_layout.h", base_dir);
    return stat(path, &st) == 0;
}

static int game_built(void)
{
    char path[1200];
    struct stat st;
    snprintf(path, sizeof path, "%s%s", base_dir, GAME_EXE);
    return stat(path, &st) == 0;
}

static void value_text(int item, char *out, size_t n)
{
    out[0] = 0;
    switch (item) {
    case I_SPEED: snprintf(out, n, "< %d %% >", cfg.speed); break;
    case I_FPS: snprintf(out, n, "< %d >", cfg.fps); break;
    case I_MUSIC: snprintf(out, n, "< %d %% >", cfg.music); break;
    case I_GFX: snprintf(out, n, "< %s >", gfx_names[cfg.graphics]); break;
    case I_SOUND: snprintf(out, n, "< %s >", sound_names[cfg.sound]); break;
    case I_SPEECH: snprintf(out, n, "< %s >", cfg.speech ? "on" : "off"); break;
    case I_VOLUME: snprintf(out, n, "< %d %% >", cfg.volume); break;
    case I_FULL: snprintf(out, n, "< %s >", cfg.fullscreen ? "on" : "off"); break;
    case I_SCALE: snprintf(out, n, "< %dx %dx%d >", cfg.scale, 320 * cfg.scale, 200 * cfg.scale); break;
    case I_PROT: snprintf(out, n, "< %s >", cfg.copy_protection ? "on" : "off"); break;
    case I_JOY: snprintf(out, n, "< %s >", cfg.joystick ? "joystick" : "keyboard"); break;
    case I_DEAD: snprintf(out, n, "< %d %% >", cfg.deadzone); break;
    case I_PAD: snprintf(out, n, "< %s >", cfg.keyboard_mode ? "joystick" : "keyboard"); break;
    case I_ORDER: snprintf(out, n, "< %s >", cfg.jungle_order ? "Amiga" : "DOS"); break;
    default: break;
    }
}

static const char *help_text(int item)
{
    switch (item) {
    case I_SPEED: return "100 % = original game speed; Shift + arrow: step 1";
    case I_FPS: return "screen updates per second (60 = original EGA); Shift + arrow: step 1";
    case I_MUSIC: return "100 % = original tempo, independent of game speed";
    case I_GFX: return cfg.graphics == GFX_AMIGA ? "the Amiga version's pictures (set the Amiga game folder)"
                                                : "the graphics card the game sees; Amiga = best picture";
    case I_SOUND: return "Tandy = 3 voices + noise (best), PC speaker = 1 voice";
    case I_SPEECH: return "the Amiga version's voices (any sound; needs the Amiga game + sound)";
    case I_VOLUME: return "Shift + arrow: step 1";
    case I_SCALE: return "window size in pixels (the game screen is 320x200)";
    case I_PROT: return "off = skip the code wheel check (security system)";
    case I_ADIR: return editing_dir ? "type the path, Enter = done, Esc = cancel"
                                    : "folder with DT1 and DT2 of the Amiga version (Amiga port's files)";
    case I_JOY: return "joystick = game controller as the PC joystick (no question at start)";
    case I_DEAD: return "how far the stick must tilt before it moves";
    case I_PAD: return "joystick = arrows move the joystick, Space/Ctrl = fire";
    case I_ORDER: return "who plays first in the jungle: DOS = red, blue, green; Amiga = green, blue, red";
    case I_IMPORT: return editing_dir == 3 ? "type the image path, Enter = extract, Esc = cancel"
                                           : "into the Amiga game folder: Enter = type a path, or drop .adf/.ipf files";
    case I_DIR: return editing_dir ? "type the path, Enter = done, Esc = cancel"
                                   : "Enter = edit, or drop the game folder on this window";
    case I_BUILD: return game_built() ? "built - rebuild after changing the game folder"
                                      : "needed once: converts the game into " GAME_EXE;
    case I_ABUILD: return amiga_sound_built() ? "built in - rebuild after changing the Amiga game folder"
                                              : "adds the Amiga option of Sound (needs the Amiga game folder)";
    default: return "";
    }
}

static void change(int item, int dir, int big)
{
    int step = big ? 1 : 5;
    switch (item) {
    case I_SPEED: cfg.speed += dir * step; break;
    case I_FPS: cfg.fps += dir * step; break;
    case I_MUSIC: cfg.music += dir * step; break;
    case I_GFX: {   /* EGA, CGA, Tandy, Amiga; Hercules (experimental) only via the ini */
        static const int order[] = {GFX_EGA, GFX_CGA, GFX_TANDY, GFX_AMIGA};
        int k = 0;
        while (k < 4 && order[k] != cfg.graphics) k++;
        cfg.graphics = order[((k < 4 ? k : 0) + 4 + dir) % 4];
        break;
    }
    case I_SOUND: cfg.sound = (cfg.sound + 4 + dir) % 4; break;
    case I_SPEECH: cfg.speech = !cfg.speech; break;
    case I_VOLUME: cfg.volume += dir * step; break;
    case I_FULL: cfg.fullscreen = !cfg.fullscreen; break;
    case I_PROT: cfg.copy_protection = !cfg.copy_protection; break;
    case I_SCALE: cfg.scale += dir; break;
    case I_JOY: cfg.joystick = !cfg.joystick; break;
    case I_DEAD: cfg.deadzone += dir * step; break;
    case I_PAD: cfg.keyboard_mode = !cfg.keyboard_mode; break;
    case I_ORDER: cfg.jungle_order = !cfg.jungle_order; break;
    default: return;
    }
    settings_clamp(&cfg);
    status[0] = 0;
}

/* Run the installer (install.bat in its own console window, install.sh in
 * the background): it downloads the build tools on first use, recompiles
 * the user's copy of the game and compiles the game program. With
 * amiga_only, a game already converted only gets the Amiga sound code
 * (from the Amiga game folder) and is compiled again. */
static void build_game(int amiga_only)
{
    char bat[1200], dir[1200], cmd[2600];
    struct stat st;
    status_ok = 0;
#ifdef _WIN32
    snprintf(bat, sizeof bat, "%sinstall.bat", base_dir);
#else
    snprintf(bat, sizeof bat, "%sinstall.sh", base_dir);
#endif
    if (stat(bat, &st) != 0) {
        snprintf(status, sizeof status, "Installer not found (use build.sh)");
        return;
    }
    if (!gamedir_ok(cfg.gamedir)) {
        snprintf(status, sizeof status, "Game folder needs DUCKTALE.EXE and IT.DRV");
        return;
    }
    if (amiga_only && !amigadir_ok(cfg.amiga_dir)) {
        snprintf(status, sizeof status, "Amiga game folder needs DT1/DuckTales");
        return;
    }
    const char *d = cfg.gamedir;
    int absolute = d[0] == '/' || d[0] == '\\' || (d[0] && d[1] == ':');
    snprintf(dir, sizeof dir, "%s%s", absolute ? "" : base_dir, d);
#ifdef _WIN32
    snprintf(cmd, sizeof cmd, "cmd.exe /c \"\"%s\" \"%s\"%s\"", bat, dir, amiga_only ? " --amiga" : "");
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL,
                        base_dir[0] ? base_dir : NULL, &si, &pi)) {
        snprintf(status, sizeof status, "Could not start install.bat");
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    snprintf(status, sizeof status, "Building in the console window...");
    status_ok = 1;
#else
    (void)cmd;
    pid_t pid = fork();
    if (pid == 0) {
        if (amiga_only) execl("/bin/sh", "sh", bat, dir, "--amiga", (char *)NULL);
        else execl("/bin/sh", "sh", bat, dir, (char *)NULL);
        _exit(127);
    }
    if (pid < 0) {
        snprintf(status, sizeof status, "Could not start install.sh");
        return;
    }
    snprintf(status, sizeof status, "Building - see the terminal; " GAME_EXE " appears when done");
    status_ok = 1;
#endif
}

static int start_game(void)
{
    char exe[1200];
    snprintf(exe, sizeof exe, "%s%s", base_dir, GAME_EXE);
#ifdef _WIN32
    char cmd[1300];
    snprintf(cmd, sizeof cmd, "\"%s\"", exe);
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(exe, cmd, NULL, NULL, FALSE, 0, NULL, base_dir[0] ? base_dir : NULL, &si, &pi))
        return 0;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
#else
    pid_t pid = fork();
    if (pid == 0) {
        if (base_dir[0] && chdir(base_dir) != 0) _exit(1);
        execl(exe, exe, (char *)NULL);
        _exit(1);
    }
    return pid > 0;
#endif
}

/* Folder dropped on the window or typed: a folder holding the game. Paths
 * inside the program folder are stored relative to it. */
static void set_gamedir(const char *dir)
{
    char d[512];
    snprintf(d, sizeof d, "%s", dir);
    size_t len = strlen(d);
    while (len > 1 && (d[len - 1] == '/' || d[len - 1] == '\\')) d[--len] = 0;
    size_t bl = strlen(base_dir);
    if (bl && !strncmp(d, base_dir, bl) && d[bl]) memmove(d, d + bl, strlen(d + bl) + 1);
    snprintf(cfg.gamedir, sizeof cfg.gamedir, "%s", d);
    status_ok = gamedir_ok(cfg.gamedir);
    snprintf(status, sizeof status, status_ok ? "Game folder set" : "No DUCKTALE.EXE + IT.DRV in that folder");
}

/* The Amiga version's disk images (.adf, or .ipf with CAPSImg) are
 * extracted into the Amiga game folder: DT1/ and DT2/ (the volume names). */
static void import_image(const char *image)
{
    static unsigned char adf[ADF_SIZE];
    char err[160], vol[64], out[1200];
    const char *dir = cfg.amiga_dir[0] ? cfg.amiga_dir : "amiga";
    int absolute = dir[0] == '/' || dir[0] == '\\' || (dir[0] && dir[1] == ':');
    snprintf(out, sizeof out, "%s%s", absolute ? "" : base_dir, dir);
    status_ok = 0;
    if (!disk_read_image(image, adf, err, sizeof err)) {
        snprintf(status, sizeof status, "Import failed: %s", err);
        return;
    }
    int n = disk_extract(adf, out, vol, sizeof vol, err, sizeof err);
    if (n < 0) {
        snprintf(status, sizeof status, "Import failed: %s", err);
        return;
    }
    if (!cfg.amiga_dir[0]) snprintf(cfg.amiga_dir, sizeof cfg.amiga_dir, "%s", dir);
    snprintf(status, sizeof status, "Disk %s: %d files extracted%s", vol, n,
             amigadir_ok(cfg.amiga_dir) ? "" : " - now the other disk");
    status_ok = 1;
}

static int is_image(const char *path)
{
    size_t n = strlen(path);
    if (n < 4) return 0;
    const char *e = path + n - 4;
    return (e[0] == '.') && ((e[1] | 32) == 'a' || (e[1] | 32) == 'i') &&
           ((e[2] | 32) == 'd' || (e[2] | 32) == 'p') && (e[3] | 32) == 'f';
}

/* returns 0 when the program should end */
static int activate(int item)
{
    switch (item) {
    case I_GFX: case I_SOUND: case I_SPEECH: case I_FULL: case I_PROT: case I_JOY: case I_PAD: case I_ORDER:
        change(item, 1, 0);
        return 1;
    case I_DIR:
        editing_dir = 1;
        snprintf(dir_edit, sizeof dir_edit, "%s", cfg.gamedir);
        SDL_StartTextInput();
        return 1;
    case I_IMPORT:
        editing_dir = 3;
        dir_edit[0] = 0;
        status[0] = 0;
        SDL_StartTextInput();
        return 1;
    case I_ADIR:
        editing_dir = 2;
        snprintf(dir_edit, sizeof dir_edit, "%s", cfg.amiga_dir);
        SDL_StartTextInput();
        return 1;
    case I_DEFAULTS:
        settings_defaults(&cfg);
        status[0] = 0;
        return 1;
    case I_BUILD: case I_ABUILD:
        if (!settings_save(&cfg, settings_path())) {
            snprintf(status, sizeof status, "Saving failed!");
            return 1;
        }
        build_game(item == I_ABUILD);
        return 1;
    case I_START:
        status_ok = 0;
        if (!game_built()) {
            snprintf(status, sizeof status, "Build the game first (Build game from game files)");
            return 1;
        }
        if (!gamedir_ok(cfg.gamedir)) {
            snprintf(status, sizeof status, "Game folder not found!");
            return 1;
        }
        if (!settings_save(&cfg, settings_path())) {
            snprintf(status, sizeof status, "Saving failed!");
            return 1;
        }
        if (!start_game()) {
            snprintf(status, sizeof status, "Could not start the game!");
            return 1;
        }
        return 0;
    case I_SAVE:
        if (!settings_save(&cfg, settings_path())) {
            status_ok = 0;
            snprintf(status, sizeof status, "Saving failed!");
            return 1;
        }
        return 0;
    case I_QUIT:
        return 0;
    }
    return 1;
}

static int row_y(int i)
{
    return TOP + i * LINE + (i >= I_BUILD ? 8 : 0) + (i >= I_DEFAULTS ? 8 : 0);
}

static void draw(int sel)
{
    const SDL_Color white = {255, 255, 255, 255}, grey = {150, 150, 170, 255};
    const SDL_Color gold = {255, 204, 51, 255}, red = {255, 110, 90, 255}, green = {120, 220, 120, 255};
    SDL_SetRenderDrawColor(ren, 16, 24, 64, 255);
    SDL_RenderClear(ren);
    const char *title = "DuckTales (DOS) - Settings";
    draw_text((W - text_width(title)) / 2, 20, title, gold);

    for (int i = 0; i < I_COUNT; i++) {
        int y = row_y(i);
        if (i == sel) {
            SDL_SetRenderDrawColor(ren, 60, 80, 160, 255);
            SDL_Rect r = {LEFT - 10, y - 3, W - 2 * LEFT + 20, LINE - 1};
            SDL_RenderFillRect(ren, &r);
        }
        draw_text(LEFT, y, labels[i], i == sel ? white : grey);
        char v[64];
        value_text(i, v, sizeof v);
        if (v[0]) draw_text(W - LEFT - text_width(v), y, v, gold);
        if (i == I_DIR || i == I_ADIR) {
            int ed = 0;
            const char *d = i == I_DIR ? cfg.gamedir : cfg.amiga_dir;
            char shown[24];
            size_t len = strlen(d);
            snprintf(shown, sizeof shown, "%s%s", len > 14 ? "..." : "", len > 14 ? d + len - 14 : d);
            if (ed) strcat(shown, "_");
            int ok = i == I_DIR ? gamedir_ok(d) : amigadir_ok(d);
            draw_text(W - LEFT - text_width(shown), y, shown, ed ? gold : ok ? green : red);
        }
    }
    /* the path being typed: a box of its own below the menu */
    if (editing_dir) {
        static const char *const what[4] = {"", "Game folder:", "Amiga game folder:", "Disk image file:"};
        int by = H - 74, bw = W - 2 * LEFT + 20;
        SDL_Rect box = {LEFT - 10, by, bw, 26};
        SDL_SetRenderDrawColor(ren, 8, 12, 32, 255);
        SDL_RenderFillRect(ren, &box);
        SDL_SetRenderDrawColor(ren, 255, 204, 51, 255);
        SDL_RenderDrawRect(ren, &box);
        draw_text_px(LEFT - 10, by - 12, what[editing_dir], gold, 1);
        int fit = (bw - 16) / CHAR - 1;
        char shown[512];
        size_t len = strlen(dir_edit);
        snprintf(shown, sizeof shown, "%s%s_", (int)len > fit ? "..." : "",
                 (int)len > fit ? dir_edit + len - (fit - 3) : dir_edit);
        draw_text(LEFT - 2, by + 5, shown, white);
    }
    const char *help = status[0] ? status : help_text(sel);
    draw_text_px(LEFT - 10, H - 30, help, status[0] ? (status_ok ? green : red) : grey, 1);
    SDL_RenderPresent(ren);
}

int main(int argc, char **argv)
{
    /* --extract <image> <folder>: command line import (also used for tests) */
    if (argc == 4 && !strcmp(argv[1], "--extract")) {
        static unsigned char adf[ADF_SIZE];
        char err[160], vol[64];
        if (!disk_read_image(argv[2], adf, err, sizeof err)) { printf("error: %s\n", err); return 1; }
        int n = disk_extract(adf, argv[3], vol, sizeof vol, err, sizeof err);
        if (n < 0) { printf("error: %s\n", err); return 1; }
        printf("%s: %d files\n", vol, n);
        return 0;
    }
    (void)argc;
    (void)argv;
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    char *b = SDL_GetBasePath();
    if (b) snprintf(base_dir, sizeof base_dir, "%s", b);
    SDL_free(b);
    settings_defaults(&cfg);
    settings_load(&cfg, settings_path());

    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    SDL_Window *win = SDL_CreateWindow("DuckTales (DOS) Settings", SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED, W * 3 / 2, H * 3 / 2,
                                       SDL_WINDOW_RESIZABLE);
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC);
    SDL_RenderSetLogicalSize(ren, W, H);
    SDL_GameController *pad = NULL;

    int sel = I_START, running = 1;
    if (getenv("DT_SEL")) sel = atoi(getenv("DT_SEL"));   /* testing */
    if (getenv("DT_EDIT")) {                              /* testing: the edit box */
        editing_dir = 3;
        snprintf(dir_edit, sizeof dir_edit, "%s", getenv("DT_EDIT"));
    }
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = 0;
            if (e.type == SDL_DROPFILE) {
                /* a disk image is extracted, a folder of the Amiga version
                 * goes to the Amiga folder */
                if (is_image(e.drop.file)) {
                    import_image(e.drop.file);
                    sel = I_IMPORT;
                } else if (amigadir_ok(e.drop.file)) {
                    snprintf(cfg.amiga_dir, sizeof cfg.amiga_dir, "%s", e.drop.file);
                    sel = I_ADIR;
                } else {
                    set_gamedir(e.drop.file);
                    sel = I_DIR;
                }
                SDL_free(e.drop.file);
            }
            if (e.type == SDL_CONTROLLERDEVICEADDED && !pad) pad = SDL_GameControllerOpen(e.cdevice.which);
            if (editing_dir && e.type == SDL_TEXTINPUT) {
                size_t len = strlen(dir_edit);
                snprintf(dir_edit + len, sizeof dir_edit - len, "%s", e.text.text);
            } else if (editing_dir && e.type == SDL_KEYDOWN) {
                SDL_Keycode k = e.key.keysym.sym;
                if (k == SDLK_BACKSPACE && dir_edit[0]) {
                    size_t len = strlen(dir_edit);
                    do len--; while (len > 0 && (dir_edit[len] & 0xC0) == 0x80);
                    dir_edit[len] = 0;
                } else if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_ESCAPE) {
                    if (k != SDLK_ESCAPE && dir_edit[0]) {
                        if (editing_dir == 1) set_gamedir(dir_edit);
                        else if (editing_dir == 3) import_image(dir_edit);
                        else snprintf(cfg.amiga_dir, sizeof cfg.amiga_dir, "%s", dir_edit);
                    }
                    editing_dir = 0;
                    SDL_StopTextInput();
                }
            } else if (e.type == SDL_KEYDOWN || e.type == SDL_CONTROLLERBUTTONDOWN) {
                int big = (SDL_GetModState() & KMOD_SHIFT) != 0;
                int key = e.type == SDL_KEYDOWN ? e.key.keysym.sym : -1;
                int btn = e.type == SDL_CONTROLLERBUTTONDOWN ? e.cbutton.button : -1;
                if (key == SDLK_UP || btn == SDL_CONTROLLER_BUTTON_DPAD_UP)
                    sel = (sel + I_COUNT - 1) % I_COUNT;
                else if (key == SDLK_DOWN || btn == SDL_CONTROLLER_BUTTON_DPAD_DOWN)
                    sel = (sel + 1) % I_COUNT;
                else if (key == SDLK_LEFT || btn == SDL_CONTROLLER_BUTTON_DPAD_LEFT) change(sel, -1, big);
                else if (key == SDLK_RIGHT || btn == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) change(sel, 1, big);
                else if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE ||
                         btn == SDL_CONTROLLER_BUTTON_A || btn == SDL_CONTROLLER_BUTTON_START)
                    running = activate(sel);
                else if (key == SDLK_ESCAPE || btn == SDL_CONTROLLER_BUTTON_B) running = 0;
            } else if (e.type == SDL_MOUSEMOTION && !editing_dir) {
                for (int i = 0; i < I_COUNT; i++) {
                    int y = row_y(i);
                    if (e.motion.y >= y - 3 && e.motion.y < y - 3 + LINE) sel = i;
                }
            } else if (e.type == SDL_MOUSEBUTTONDOWN && !editing_dir) {
                /* on a value: left half decreases, right half increases */
                char v[64];
                value_text(sel, v, sizeof v);
                if (v[0] && e.button.x > W / 2)
                    change(sel, e.button.x < W - LEFT - text_width(v) / 2 ? -1 : 1, 0);
                else
                    running = activate(sel);
            } else if (e.type == SDL_MOUSEWHEEL && !editing_dir) {
                change(sel, e.wheel.y > 0 ? 1 : -1, 0);
            }
        }
        draw(sel);
        /* DT_SHOT=file.bmp: save a frame and exit (for testing) */
        if (getenv("DT_SHOT")) {
            SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, W * 3 / 2, H * 3 / 2, 32,
                                                            SDL_PIXELFORMAT_ARGB8888);
            SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
            SDL_SaveBMP(s, getenv("DT_SHOT"));
            SDL_FreeSurface(s);
            running = 0;
        }
    }
    SDL_Quit();
    return 0;
}
