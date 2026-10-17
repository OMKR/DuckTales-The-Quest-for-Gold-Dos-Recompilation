/* The Amiga runtime interface (am_m68k.h, from the Amiga port) for the
 * Amiga sound code (gen/amigasnd_gen.c, runtime/amigasnd.c): its global
 * names get an am_ prefix so they do not clash with the DOS runtime's. */
#ifndef AMIGASND_RT_H
#define AMIGASND_RT_H

#define CPU am_CPU
#define R am_R
#define mem am_mem
#define hw_read am_hw_read
#define hw_write am_hw_write
#define lib_call am_lib_call
#define call_indirect am_call_indirect
#define rt_fatal am_rt_fatal
#define watch_lo am_watch_lo
#define watch_len am_watch_len
#define watch_hit am_watch_hit
#define op_divu am_op_divu
#define op_divs am_op_divs
#define op_asl am_op_asl
#define op_asr am_op_asr
#define op_lsl am_op_lsl
#define op_lsr am_op_lsr
#define op_rol am_op_rol
#define op_ror am_op_ror
#define op_roxl am_op_roxl
#define op_roxr am_op_roxr
#define poll_count am_poll_count
#define poll_pc am_poll_pc
#define hle_poll am_hle_poll
#define ftrace_ring am_ftrace_ring
#define ftrace_pos am_ftrace_pos
#define ftrace_break am_ftrace_break
#define logfn_count am_logfn_count
#define logfn_check am_logfn_check
#define ftrace_dump am_ftrace_dump
#define rt_program_exit am_rt_program_exit
#define copy_protection_off am_copy_protection_off
#define music_gate am_music_gate
#define music_tick am_music_tick
#define call_indirect_ret am_call_indirect_ret
#define call_indirect_tail am_call_indirect_tail

#include "am_m68k.h"

typedef struct { uint32_t addr; void (*fn)(void); } AmFunc;
extern const AmFunc am_func_table[];
extern const int am_func_table_len;
void amiga_overlay_call(uint32_t node_off);

#endif
