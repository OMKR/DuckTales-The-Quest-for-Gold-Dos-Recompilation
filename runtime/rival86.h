/* The plane's rival under the plane's collision rules (rival86.c) */
#ifndef RIVAL86_H
#define RIVAL86_H

extern int rival_probe;     /* the game's collision test is running for the rival */

void hook_rival_step_begin(void);
void hook_rival_step_end(void);
void hook_rival_draw_before(void);
void hook_rival_draw_after(void);

#endif
