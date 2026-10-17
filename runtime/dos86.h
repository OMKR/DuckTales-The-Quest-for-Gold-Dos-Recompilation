#ifndef DOS86_H
#define DOS86_H

#include <stdint.h>

/* Interrupt vectors initially point to BIOS_SEG:(n*4); calls there are
 * handled by bios_vector(n). */
#define BIOS_SEG 0xF000u

void dos_init(uint32_t psp, uint32_t prog_end);
void bios_vector(uint32_t n);
const char *game_dir(void);
void raise_irq(uint32_t vector);
void ftrace_dump(void);

/* chips86.c */
void chips_init(void);
int key_get(int mode);  /* 1 wait+take, 0 take if any, -1 peek; ASCII|scan<<8 or -1 */
uint32_t bios_ticks(void);
extern int hle_trace;

/* video86.c */
void video_init(void);
void video_set_mode(int mode);
int video_get_mode(void);
uint32_t video_ega_rgb(int colour);
/* amiga86.c */
void amiga_init(void);
int amiga_driver(uint32_t drv_seg, uint32_t off);
void amiga_driver_after(uint32_t off);
void amiga_render(uint32_t start_addr, uint32_t *pixels, int W, int H, int dark);
int amiga_active(void);
/* video86.c: EGA planes */
uint8_t video_plane_byte(int plane, uint32_t off);
void video_plane_copy(uint32_t dst, uint32_t src, uint32_t n);
void video_plane_fill(uint32_t dst, uint32_t n, uint8_t v);
/* chips86.c */
void wait_frames(int n);
const char *dos_file_name(int handle);
void video_toggle_fullscreen(void);
void video_set_title(const char *title);
void video_vblank(void);
int video_save_bmp(const char *path);
void video_cga_palette(int bh, int bl);
int video_tandy_pages(int al, int *bh, int *bl);
void video_set_page(int page);
int video_get_page(void);
void video_text_out(int ch);
void video_frame(void);
void video_ega_palette(int index, int value);

#endif
