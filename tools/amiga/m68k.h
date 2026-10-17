/* Runtime support for recompiled MC68000 code.
 *
 * The original program runs against an emulated 24-bit big-endian address
 * space. Registers and condition codes live in the global CPU state R.
 * Addresses in the custom chip and CIA ranges are routed to hw_read/hw_write.
 */
#ifndef M68K_H
#define M68K_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define MEM_SIZE 0x01000000u
#define ADDR_MASK 0x00FFFFFFu

typedef struct {
    uint32_t d[8];
    uint32_t a[8];
    uint32_t x, n, z, v, c;  /* condition codes, each 0 or 1 */
    uint16_t sr_hi;          /* supervisor byte of SR, kept for move to/from sr */
} CPU;

extern CPU R;
extern uint8_t *mem;

/* Hardware and OS emulation, implemented in hle/. */
uint32_t hw_read(uint32_t addr, int size);
void hw_write(uint32_t addr, uint32_t value, int size);
void lib_call(uint32_t base, int32_t offset);
void call_indirect(uint32_t addr);
void rt_fatal(const char *fmt, ...);

static inline int is_hw(uint32_t a)
{
    return a >= 0x00BF0000u && a < 0x00E00000u;
}

/* Debug write watch (DT_WATCH=start,len): reports which functions write there. */
extern uint32_t watch_lo, watch_len;
void watch_hit(uint32_t addr);
#define WATCH(a) do { if ((a) - watch_lo < watch_len) watch_hit(a); } while (0)

static inline uint32_t rb(uint32_t a)
{
    a &= ADDR_MASK;
    if (is_hw(a)) return hw_read(a, 1);
    return mem[a];
}

static inline uint32_t rw(uint32_t a)
{
    a &= ADDR_MASK;
    if (is_hw(a)) return hw_read(a, 2);
    return ((uint32_t)mem[a] << 8) | mem[a + 1];
}

static inline uint32_t rl(uint32_t a)
{
    a &= ADDR_MASK;
    if (is_hw(a)) return hw_read(a, 4);
    return ((uint32_t)mem[a] << 24) | ((uint32_t)mem[a + 1] << 16) |
           ((uint32_t)mem[a + 2] << 8) | mem[a + 3];
}

static inline void wb(uint32_t a, uint32_t v)
{
    a &= ADDR_MASK;
    if (is_hw(a)) { hw_write(a, v & 0xFF, 1); return; }
    WATCH(a);
    mem[a] = (uint8_t)v;
}

static inline void ww(uint32_t a, uint32_t v)
{
    a &= ADDR_MASK;
    if (is_hw(a)) { hw_write(a, v & 0xFFFF, 2); return; }
    WATCH(a);
    mem[a] = (uint8_t)(v >> 8);
    mem[a + 1] = (uint8_t)v;
}

static inline void wl(uint32_t a, uint32_t v)
{
    a &= ADDR_MASK;
    if (is_hw(a)) { hw_write(a, v, 4); return; }
    WATCH(a);
    mem[a] = (uint8_t)(v >> 24);
    mem[a + 1] = (uint8_t)(v >> 16);
    mem[a + 2] = (uint8_t)(v >> 8);
    mem[a + 3] = (uint8_t)v;
}

/* Sign extension helpers. */
#define SX8(v)  ((uint32_t)(int32_t)(int8_t)(v))
#define SX16(v) ((uint32_t)(int32_t)(int16_t)(v))

/* Size masks and sign bits for .b/.w/.l results. */
#define MASK1 0xFFu
#define MASK2 0xFFFFu
#define MASK4 0xFFFFFFFFu
#define MSB1 0x80u
#define MSB2 0x8000u
#define MSB4 0x80000000u

/* Write the low `size` bytes of a data register, keeping the rest. */
static inline void set_d(int r, uint32_t v, int size)
{
    if (size == 1) R.d[r] = (R.d[r] & ~0xFFu) | (v & 0xFFu);
    else if (size == 2) R.d[r] = (R.d[r] & ~0xFFFFu) | (v & 0xFFFFu);
    else R.d[r] = v;
}

/* Condition-code helpers. res/src/dst are already masked to the size. */
static inline void flags_logic(uint32_t res, uint32_t msb)
{
    R.n = (res & msb) != 0;
    R.z = res == 0;
    R.v = 0;
    R.c = 0;
}

static inline void flags_add(uint32_t src, uint32_t dst, uint32_t res, uint32_t msb, uint32_t mask)
{
    R.n = (res & msb) != 0;
    R.z = (res & mask) == 0;
    R.v = (((src ^ res) & (dst ^ res)) & msb) != 0;
    R.c = R.x = (((src & dst) | (~res & (src | dst))) & msb) != 0;
}

static inline void flags_sub(uint32_t src, uint32_t dst, uint32_t res, uint32_t msb, uint32_t mask)
{
    R.n = (res & msb) != 0;
    R.z = (res & mask) == 0;
    R.v = (((src ^ dst) & (res ^ dst)) & msb) != 0;
    R.c = R.x = (((src & ~dst) | (res & ~dst) | (src & res)) & msb) != 0;
}

static inline void flags_cmp(uint32_t src, uint32_t dst, uint32_t res, uint32_t msb, uint32_t mask)
{
    uint32_t x = R.x;
    flags_sub(src, dst, res, msb, mask);
    R.x = x;  /* cmp leaves X alone */
}

/* Condition tests. */
#define CC_T  1
#define CC_F  0
#define CC_HI (!R.c && !R.z)
#define CC_LS (R.c || R.z)
#define CC_CC (!R.c)
#define CC_CS (R.c)
#define CC_NE (!R.z)
#define CC_EQ (R.z)
#define CC_VC (!R.v)
#define CC_VS (R.v)
#define CC_PL (!R.n)
#define CC_MI (R.n)
#define CC_GE (R.n == R.v)
#define CC_LT (R.n != R.v)
#define CC_GT (!R.z && R.n == R.v)
#define CC_LE (R.z || R.n != R.v)

static inline uint32_t get_ccr(void)
{
    return (R.x << 4) | (R.n << 3) | (R.z << 2) | (R.v << 1) | R.c;
}

static inline void set_ccr(uint32_t v)
{
    R.x = (v >> 4) & 1;
    R.n = (v >> 3) & 1;
    R.z = (v >> 2) & 1;
    R.v = (v >> 1) & 1;
    R.c = v & 1;
}

/* Stack helpers. */
static inline void push32(uint32_t v) { R.a[7] -= 4; wl(R.a[7], v); }
static inline uint32_t pop32(void) { uint32_t v = rl(R.a[7]); R.a[7] += 4; return v; }

/* Division with the 68000's overflow and divide-by-zero behaviour. */
void op_divu(int dn, uint32_t src);
void op_divs(int dn, uint32_t src);

/* Shifts and rotates; set N, Z, V, C (and X where the 68000 does). */
uint32_t op_asl(uint32_t v, uint32_t cnt, int size);
uint32_t op_asr(uint32_t v, uint32_t cnt, int size);
uint32_t op_lsl(uint32_t v, uint32_t cnt, int size);
uint32_t op_lsr(uint32_t v, uint32_t cnt, int size);
uint32_t op_rol(uint32_t v, uint32_t cnt, int size);
uint32_t op_ror(uint32_t v, uint32_t cnt, int size);
uint32_t op_roxl(uint32_t v, uint32_t cnt, int size);
uint32_t op_roxr(uint32_t v, uint32_t cnt, int size);

/* Backward branches give the frame clock, interrupts and input a chance
 * to run, like the real machine would between instructions. */
extern int poll_count;
extern uint32_t poll_pc;
void hle_poll(void);
#define POLL(pc) do { if (--poll_count <= 0) { poll_pc = (pc); hle_poll(); } } while (0)

/* Ring buffer of recently entered functions, for diagnostics. */
extern uint32_t ftrace_ring[64];
extern unsigned ftrace_pos;
extern uint32_t ftrace_break;
#define FTRACE(addr) do { ftrace_ring[ftrace_pos++ & 63] = (addr); \
    if ((addr) == ftrace_break) ftrace_dump(); \
    if (logfn_count) logfn_check(addr); } while (0)
/* DT_LOGFN=addr,addr,...: one line per call with frame and key buffer state */
extern int logfn_count;
void logfn_check(uint32_t addr);
void ftrace_dump(void);

/* rts. Returning to the program's entry return address means the program
 * exited back to AmigaDOS (Aztec's exit() resets a7 and returns from
 * anywhere), so the whole C call stack must be left, not just one level. */
#define RTS_EXIT_ADDR 0x00FFFFF0u
void rt_program_exit(void);
#define RTS() do { uint32_t ret_ = rl(R.a[7]); R.a[7] += 4; \
    if (ret_ == RTS_EXIT_ADDR) rt_program_exit(); return; } while (0)

/* Settings checked by patched game code (core.c). */
int copy_protection_off(void);

/* Music player gate and fixed-rate tick (core.c). */
int music_gate(void);
void music_tick(void);

/* Indirect control transfers (core.c). */
void call_indirect_ret(uint32_t addr, uint32_t ret);
void call_indirect_tail(uint32_t addr);

#endif
