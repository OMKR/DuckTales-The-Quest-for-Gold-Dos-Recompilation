/* Amiga sound for the DOS port (runtime/amigasnd.c): the Amiga version's own
 * sound engine and instruments, run in a small Amiga memory. */
#ifndef AMIGASND_H
#define AMIGASND_H

#include <stdint.h>

/* Loads the Amiga program and its startup instruments from
 * <amiga_dir>/DT1/DuckTales; 0 if not available. */
int amigasnd_init(const char *amiga_dir);
int amigasnd_active(void);
/* A sound request (the numbers are the same in both versions). */
void amigasnd_request(int n);
/* Loads instrument set n (as the Amiga game does for each scene). */
void amigasnd_scene(int n);
/* The DOS game's overlay n runs (each minigame is one): its instruments. */
void amigasnd_overlay(int n);
/* With Tandy or PC speaker sound: only the Amiga voices are played here
 * (the DOS driver plays the rest); amigasnd_music() = the Amiga engine
 * plays all sounds. */
void amigasnd_voices_only(int on);
int amigasnd_music(void);
/* voices on/off (settings: speech) */
void amigasnd_set_speech(int on);
/* a voice is being spoken (the Amiga game waits for it before going on) */
int amigasnd_speaking(void);
/* Voices (the Amiga version speaks; the DOS game shows the same scenes):
 * a text the game draws, a picture it loads, or one of these events. */
void amigasnd_speech(const int *sets, int n);
void amigasnd_text(const char *text);
void amigasnd_picture(const char *file, int entry);
enum { AMIGASND_LANDED, AMIGASND_SCALES_WON, AMIGASND_SCALES_LOST };
void amigasnd_event(int event);
/* Adds n mono samples at `rate` Hz to buf; the engine ticks at 50 Hz scaled
 * by music_pct. */
void amigasnd_render(int32_t *buf, int n, int rate, int music_pct);

#endif
