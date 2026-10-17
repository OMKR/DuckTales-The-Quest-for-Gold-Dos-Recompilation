/* Settings shared by the game runtime and the settings program
 * (ducktales_dos.ini next to the programs). */
#ifndef SETTINGS86_H
#define SETTINGS86_H

enum { SOUND_OFF, SOUND_SPEAKER, SOUND_TANDY, SOUND_AMIGA };
enum { GFX_EGA, GFX_CGA, GFX_TANDY, GFX_HERCULES, GFX_AMIGA };
/* Amiga graphics: the game sees an EGA card (runtime/amiga86.c) */
#define GFX_IS_EGA(g) ((g) == GFX_EGA || (g) == GFX_AMIGA)

/* joystick port timing in port reads (runtime/chips86.c) */
#define JOY_MIN 20
#define JOY_MID 200
#define JOY_MAX 380

typedef struct {
    int speed;              /* game speed: the game's tick counter, % of the original */
    int fps;                /* frame rate limit: vertical retrace rate (Hz) */
    int music;              /* music speed: sound driver timer rate, % of the original */
    int graphics;           /* GFX_EGA / GFX_CGA / GFX_TANDY / GFX_HERCULES / GFX_AMIGA */
    int sound;              /* SOUND_OFF / SOUND_SPEAKER / SOUND_TANDY / SOUND_AMIGA */
    int volume;             /* 0-100 */
    int speech;             /* Amiga sound: the Amiga version's voices on/off */
    int fullscreen;
    int scale;              /* window = 320*scale x 200*scale */
    int copy_protection;    /* 0 = skip the code wheel check */
    int joystick;           /* game uses joystick controls (controller) */
    int deadzone;           /* controller stick dead zone, % */
    int keyboard_mode;      /* arrows + Space/Ctrl drive the joystick */
    int jungle_order;       /* jungle nephews in the Amiga order (green first) */
    char gamedir[512];
    char amiga_dir[512];    /* Amiga game folder (DT1, DT2) */
} Settings;

extern Settings settings;

void settings_defaults(Settings *s);
void settings_clamp(Settings *s);
const char *settings_path(void);
int settings_load(Settings *s, const char *path);
int settings_save(const Settings *s, const char *path);

#endif
