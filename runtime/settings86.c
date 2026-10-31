/* ducktales_dos.ini reading and writing (simple key = value lines). */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "settings86.h"

Settings settings;

void settings_defaults(Settings *s)
{
    memset(s, 0, sizeof *s);
    s->speed = 100;
    s->fps = 60;
    s->music = 100;
    s->graphics = GFX_EGA;
    s->sound = SOUND_TANDY;
    s->volume = 80;
    s->fullscreen = 0;
    s->scale = 3;
    s->copy_protection = 1;
    s->joystick = 0;
    s->deadzone = 40;
    s->keyboard_mode = 0;
    s->jungle_order = 0;
    s->fixed_random = 1;
    s->speaker_filter = 0;
    s->launchpad_stop = 1;
    s->rival_collisions = 1;
    s->log = 0;
    s->ega_flash = 1;
    s->speech = 1;
    strcpy(s->gamedir, "files");
    strcpy(s->amiga_dir, "amiga");
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

void settings_clamp(Settings *s)
{
    s->speed = clampi(s->speed, 25, 400);
    s->fps = clampi(s->fps, 10, 200);
    s->music = clampi(s->music, 25, 400);
    s->graphics = clampi(s->graphics, GFX_EGA, GFX_AMIGA);
    s->sound = clampi(s->sound, SOUND_OFF, SOUND_AMIGA);
    s->volume = clampi(s->volume, 0, 100);
    s->speech = s->speech != 0;
    s->fullscreen = s->fullscreen != 0;
    s->scale = clampi(s->scale, 1, 5);
    s->copy_protection = s->copy_protection != 0;
    s->joystick = s->joystick != 0;
    s->deadzone = clampi(s->deadzone, 5, 90);
    s->keyboard_mode = s->keyboard_mode != 0;
    s->jungle_order = s->jungle_order != 0;
    s->fixed_random = s->fixed_random != 0;
    s->speaker_filter = s->speaker_filter != 0;
    s->launchpad_stop = s->launchpad_stop != 0;
    s->rival_collisions = s->rival_collisions != 0;
    s->log = s->log != 0;
    s->ega_flash = clampi(s->ega_flash, 0, 2);
}

const char *settings_path(void)
{
    static char path[1024];
    if (!path[0]) {
        char *base = SDL_GetBasePath();
        snprintf(path, sizeof path, "%sducktales_dos.ini", base ? base : "");
        SDL_free(base);
    }
    return path;
}

int settings_load(Settings *s, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[640];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#' || line[0] == ';') continue;
        *eq = 0;
        char *key = line, *val = eq + 1;
        while (*key == ' ' || *key == '\t') key++;
        for (char *e = key + strlen(key); e > key && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
        while (*val == ' ' || *val == '\t') val++;
        val[strcspn(val, "\r\n")] = 0;
        int n = atoi(val);
        if (!strcmp(key, "speed")) s->speed = n;
        else if (!strcmp(key, "fps")) s->fps = n;
        else if (!strcmp(key, "music")) s->music = n;
        else if (!strcmp(key, "graphics")) s->graphics = n;
        else if (!strcmp(key, "sound")) s->sound = n;
        else if (!strcmp(key, "volume")) s->volume = n;
        else if (!strcmp(key, "speech")) s->speech = n;
        else if (!strcmp(key, "fullscreen")) s->fullscreen = n;
        else if (!strcmp(key, "scale")) s->scale = n;
        else if (!strcmp(key, "copy_protection")) s->copy_protection = n;
        else if (!strcmp(key, "joystick")) s->joystick = n;
        else if (!strcmp(key, "deadzone")) s->deadzone = n;
        else if (!strcmp(key, "keyboard_mode")) s->keyboard_mode = n;
        else if (!strcmp(key, "jungle_order")) s->jungle_order = n;
        else if (!strcmp(key, "fixed_random")) s->fixed_random = n;
        else if (!strcmp(key, "speaker_filter")) s->speaker_filter = n;
        else if (!strcmp(key, "launchpad_stop")) s->launchpad_stop = n;
        else if (!strcmp(key, "rival_collisions")) s->rival_collisions = n;
        else if (!strcmp(key, "log")) s->log = n;
        else if (!strcmp(key, "ega_flash")) s->ega_flash = n;
        else if (!strcmp(key, "amiga_dir") && *val)
            snprintf(s->amiga_dir, sizeof s->amiga_dir, "%s", val);
        else if (!strcmp(key, "gamedir") && *val)
            snprintf(s->gamedir, sizeof s->gamedir, "%s", val);
    }
    fclose(f);
    settings_clamp(s);
    return 1;
}

int settings_save(const Settings *s, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fprintf(f, "# DuckTales (DOS) - settings (settings_dos.exe)\n");
    fprintf(f, "speed = %d\n", s->speed);
    fprintf(f, "fps = %d\n", s->fps);
    fprintf(f, "music = %d\n", s->music);
    fprintf(f, "# graphics: 0 = EGA, 1 = CGA, 2 = Tandy, 3 = Hercules, 4 = Amiga (32 colours)\n");
    fprintf(f, "graphics = %d\n", s->graphics);
    fprintf(f, "# sound: 0 = off, 1 = PC speaker, 2 = Tandy, 3 = Amiga (needs amiga_dir)\n");
    fprintf(f, "sound = %d\n", s->sound);
    fprintf(f, "volume = %d\n", s->volume);
    fprintf(f, "# speech: the Amiga version's voices with Amiga sound (0 = off, 1 = on)\n");
    fprintf(f, "speech = %d\n", s->speech);
    fprintf(f, "fullscreen = %d\n", s->fullscreen);
    fprintf(f, "scale = %d\n", s->scale);
    fprintf(f, "copy_protection = %d\n", s->copy_protection);
    fprintf(f, "joystick = %d\n", s->joystick);
    fprintf(f, "deadzone = %d\n", s->deadzone);
    fprintf(f, "keyboard_mode = %d\n", s->keyboard_mode);
    fprintf(f, "# jungle_order: 1 = the nephews take turns in the Amiga order (green, blue, red)\n");
    fprintf(f, "jungle_order = %d\n", s->jungle_order);
    fprintf(f, "# fixed_random: 0 = the original random numbers, 1 = no short repeating cycles\n");
    fprintf(f, "fixed_random = %d\n", s->fixed_random);
    fprintf(f, "# ega_flash: the photo flash in EGA: 0 = off, 1 = border (as on the PC), 2 = whole screen\n");
    fprintf(f, "ega_flash = %d\n", s->ega_flash);
    fprintf(f, "# log: 1 = write logs/ducktales_<date>_<time>.log (one per run, for crash reports)\n");
    fprintf(f, "log = %d\n", s->log);
    fprintf(f, "# rival_collisions: 1 = the rival's plane is hit by obstacles as the player's (and dodges them)\n");
    fprintf(f, "rival_collisions = %d\n", s->rival_collisions);
    fprintf(f, "# speaker_filter: 1 = the PC speaker sounds as a small speaker (little bass), 0 = clean\n");
    fprintf(f, "speaker_filter = %d\n", s->speaker_filter);
    fprintf(f, "# launchpad_stop: Amiga sound: 1 = the music stops before Launchpad's screen (Amiga), 0 = plays on (DOS)\n");
    fprintf(f, "launchpad_stop = %d\n", s->launchpad_stop);
    fprintf(f, "gamedir = %s\n", s->gamedir);
    fprintf(f, "amiga_dir = %s\n", s->amiga_dir);
    fclose(f);
    return 1;
}
