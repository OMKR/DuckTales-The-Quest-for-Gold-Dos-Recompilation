/* Runtime support for recompiled 8086 code.
 *
 * The program runs in an emulated 1 MB real-mode address space. Registers
 * and flags live in the global R. The program is loaded at segment LOAD
 * (gen/layout86.h); relocated segment values in the generated code already
 * include it. Video memory is routed to the video emulation.
 */
#ifndef CPU86_H
#define CPU86_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define MEM86_SIZE 0x110000u

typedef struct {
    uint16_t ax, bx, cx, dx, si, di, bp, sp;
    uint16_t es, cs, ss, ds;
    uint32_t cf, pf, af, zf, sf, of, df, ifl;
} CPU86;

extern CPU86 R;
extern uint8_t *mem;

/* video.c: A0000-BFFFF goes through the EGA/CGA emulation */
uint32_t vram_read(uint32_t lin);
void vram_write(uint32_t lin, uint32_t v);
/* chips86.c: I/O ports */
uint32_t port_in(uint32_t port, int size);
void port_out(uint32_t port, uint32_t v, int size);
void rt_fatal(const char *fmt, ...);

static inline uint32_t LIN(uint32_t seg, uint32_t off)
{
    return ((seg << 4) + (off & 0xFFFF)) & 0xFFFFF;
}

static inline int is_vram(uint32_t lin) { return lin >= 0xA0000 && lin < 0xC0000; }

static inline uint32_t rb(uint32_t seg, uint32_t off)
{
    uint32_t a = LIN(seg, off);
    return is_vram(a) ? vram_read(a) : mem[a];
}

static inline void wb(uint32_t seg, uint32_t off, uint32_t v)
{
    uint32_t a = LIN(seg, off);
    if (is_vram(a)) vram_write(a, v & 0xFF);
    else mem[a] = (uint8_t)v;
}

static inline uint32_t rw(uint32_t seg, uint32_t off)
{
    return rb(seg, off) | (rb(seg, (off + 1) & 0xFFFF) << 8);
}

static inline void ww(uint32_t seg, uint32_t off, uint32_t v)
{
    wb(seg, off, v);
    wb(seg, (off + 1) & 0xFFFF, v >> 8);
}

/* 8-bit register halves */
#define LO(r) ((r) & 0xFF)
#define HI(r) ((r) >> 8)
#define SET_LO(r, v) ((r) = (uint16_t)(((r) & 0xFF00) | ((v) & 0xFF)))
#define SET_HI(r, v) ((r) = (uint16_t)(((r) & 0x00FF) | (((v) & 0xFF) << 8)))

/* stack */
static inline void push16(uint32_t v) { R.sp -= 2; ww(R.ss, R.sp, v); }
static inline uint32_t pop16(void) { uint32_t v = rw(R.ss, R.sp); R.sp += 2; return v; }

/* flags */
static inline uint32_t parity8(uint32_t v)
{
    v &= 0xFF;
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return (~v) & 1;
}

static inline void szp(uint32_t r, uint32_t bits)
{
    uint32_t m = bits == 8 ? 0xFF : 0xFFFF;
    R.zf = (r & m) == 0;
    R.sf = (r >> (bits - 1)) & 1;
    R.pf = parity8(r);
}

static inline uint32_t f_add(uint32_t a, uint32_t b, uint32_t c, uint32_t bits)
{
    uint32_t m = bits == 8 ? 0xFF : 0xFFFF, top = 1u << (bits - 1);
    uint32_t r = a + b + c;
    R.cf = r > m;
    R.of = ((a ^ r) & (b ^ r) & top) != 0;
    R.af = ((a ^ b ^ r) & 0x10) != 0;
    szp(r, bits);
    return r & m;
}

static inline uint32_t f_sub(uint32_t a, uint32_t b, uint32_t c, uint32_t bits)
{
    uint32_t m = bits == 8 ? 0xFF : 0xFFFF, top = 1u << (bits - 1);
    uint32_t r = (a - b - c) & m;
    R.cf = b + c > a;
    R.of = ((a ^ b) & (a ^ r) & top) != 0;
    R.af = ((a ^ b ^ r) & 0x10) != 0;
    szp(r, bits);
    return r;
}

static inline uint32_t f_logic(uint32_t r, uint32_t bits)
{
    R.cf = R.of = 0;
    R.af = 0;
    szp(r, bits);
    return r;
}

static inline uint32_t f_inc(uint32_t a, uint32_t bits)
{
    uint32_t cf = R.cf, r = f_add(a, 1, 0, bits);
    R.cf = cf;
    return r;
}

static inline uint32_t f_dec(uint32_t a, uint32_t bits)
{
    uint32_t cf = R.cf, r = f_sub(a, 1, 0, bits);
    R.cf = cf;
    return r;
}

static inline uint32_t get_flags(void)
{
    return R.cf | 2 | (R.pf << 2) | (R.af << 4) | (R.zf << 6) | (R.sf << 7) |
           (R.ifl << 9) | (R.df << 10) | (R.of << 11) | 0xF000;
}

static inline void set_flags(uint32_t f)
{
    R.cf = f & 1;
    R.pf = (f >> 2) & 1;
    R.af = (f >> 4) & 1;
    R.zf = (f >> 6) & 1;
    R.sf = (f >> 7) & 1;
    R.ifl = (f >> 9) & 1;
    R.df = (f >> 10) & 1;
    R.of = (f >> 11) & 1;
}

#define C_JO  (R.of)
#define C_JNO (!R.of)
#define C_JB  (R.cf)
#define C_JAE (!R.cf)
#define C_JE  (R.zf)
#define C_JNE (!R.zf)
#define C_JBE (R.cf || R.zf)
#define C_JA  (!R.cf && !R.zf)
#define C_JS  (R.sf)
#define C_JNS (!R.sf)
#define C_JP  (R.pf)
#define C_JNP (!R.pf)
#define C_JL  (R.sf != R.of)
#define C_JGE (R.sf == R.of)
#define C_JLE (R.zf || R.sf != R.of)
#define C_JG  (!R.zf && R.sf == R.of)

/* ops86.c */
uint32_t op_shift(int kind, uint32_t v, uint32_t count, uint32_t bits);
enum { SH_ROL, SH_ROR, SH_RCL, SH_RCR, SH_SHL, SH_SHR, SH_SAL, SH_SAR };
void op_mul8(uint32_t v);
void op_imul8(uint32_t v);
void op_mul16(uint32_t v);
void op_imul16(uint32_t v);
void op_div8(uint32_t v);
void op_idiv8(uint32_t v);
void op_div16(uint32_t v);
void op_idiv16(uint32_t v);
uint32_t op_imul3(uint32_t a, uint32_t b);   /* 186 imul r, r/m, imm */

/* fpu86.c: 8087 instructions, memory operands as seg:off */
void fpu_op(int op, int form, uint32_t seg, uint32_t off, int sti);

/* control transfer and environment (core86.c, dos.c) */
void dispatch_far(uint32_t seg, uint32_t off);       /* after the far return addr was pushed */
void dispatch_near(uint32_t cs, uint32_t off);       /* after the near return addr was pushed */
/* Indirect jumps: a jump to the active caller's return address (MSC
 * runtime routines pop their return address and jump back) is a return. */
void jump_far(uint32_t seg, uint32_t off);
void jump_near(uint32_t cs, uint32_t off);
extern uint32_t rs_stack[1024];
extern int rs_top;
#define RS_PUSH(seg, ip) (rs_stack[rs_top++ & 1023] = ((uint32_t)(seg) << 16) | ((ip) & 0xFFFF))
#define RS_POP() (rs_top--)
void do_int(uint32_t n, uint32_t ret_cs, uint32_t ret_ip);
void ovl_call(int n, uint32_t off, uint32_t ret_cs, uint32_t ret_ip);
void ret_far_fix(void);

/* frame clock and devices get time on backward branches */
extern int poll_count;
extern uint32_t poll_pc;
void hle_poll(void);
#define POLL(pc) do { if (--poll_count <= 0) { poll_pc = (pc); hle_poll(); } } while (0)

extern uint32_t ftrace_ring[64];
extern unsigned ftrace_pos;
#define FTRACE(a) (ftrace_ring[ftrace_pos++ & 63] = (a))

#endif
