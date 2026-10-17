/* 8087 floating point on host doubles (the game's use is ordinary C
 * float/double arithmetic; 80-bit precision is not needed). */
#include <math.h>
#include <string.h>

#include "cpu86.h"
#include "fpu86.h"

static double st[8];
static int top;                     /* st(0) = st[top] */
static uint32_t sw_cond, cw = 0x037F;

#define ST(i) st[(top + (i)) & 7]

static void push(double v) { top = (top - 1) & 7; st[top] = v; }
static double pop(void) { double v = st[top]; top = (top + 1) & 7; return v; }

uint32_t fpu_status(void)
{
    return sw_cond | ((uint32_t)top << 11);
}

static void compare(double a, double b)
{
    /* C3 C2 C0: > 000, < 001, = 100, unordered 111 */
    if (a != a || b != b) sw_cond = 0x4500;
    else if (a > b) sw_cond = 0;
    else if (a < b) sw_cond = 0x0100;
    else sw_cond = 0x4000;
}

static double round_cw(double v)
{
    switch ((cw >> 10) & 3) {
    case 0: return nearbyint(v);
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}

static double load_float(int size, uint32_t seg, uint32_t off)
{
    uint8_t b[10];
    for (int i = 0; i < size; i++) b[i] = (uint8_t)rb(seg, off + i);
    if (size == 4) { float f; memcpy(&f, b, 4); return f; }
    if (size == 8) { double d; memcpy(&d, b, 8); return d; }
    /* 80-bit extended */
    uint64_t mant; memcpy(&mant, b, 8);
    int ex = (b[8] | (b[9] << 8)) & 0x7FFF, sign = b[9] & 0x80;
    double v = ex ? ldexp((double)mant, ex - 16383 - 63) : 0.0;
    return sign ? -v : v;
}

static void store_float(int size, uint32_t seg, uint32_t off, double v)
{
    uint8_t b[10] = {0};
    if (size == 4) { float f = (float)v; memcpy(b, &f, 4); }
    else if (size == 8) memcpy(b, &v, 8);
    else {
        int sign = v < 0, ex = 0;
        double m = frexp(fabs(v), &ex);
        uint64_t mant = v == 0 ? 0 : (uint64_t)ldexp(m, 64);
        int e = v == 0 ? 0 : ex - 1 + 16383;
        memcpy(b, &mant, 8);
        b[8] = (uint8_t)e;
        b[9] = (uint8_t)((e >> 8) | (sign ? 0x80 : 0));
    }
    for (int i = 0; i < size; i++) wb(seg, off + i, b[i]);
}

static int64_t load_int(int size, uint32_t seg, uint32_t off)
{
    if (size == 2) return (int16_t)rw(seg, off);
    if (size == 4) return (int32_t)(rw(seg, off) | (rw(seg, off + 2) << 16));
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | rb(seg, off + i);
    return (int64_t)v;
}

static void store_int(int size, uint32_t seg, uint32_t off, double v)
{
    double r = round_cw(v);
    int64_t i = (int64_t)r;
    if (size == 2 && (r > 32767 || r < -32768)) i = -32768;
    if (size == 4 && (r > 2147483647.0 || r < -2147483648.0)) i = INT32_MIN;
    for (int k = 0; k < size; k++) wb(seg, off + k, (uint32_t)(i >> (8 * k)));
}

static double arith(int op, double a, double b)
{
    switch (op) {
    case FPU_FADD: case FPU_FADDP: case FPU_FIADD: return a + b;
    case FPU_FSUB: case FPU_FSUBP: case FPU_FISUB: return a - b;
    case FPU_FSUBR: case FPU_FSUBRP: case FPU_FISUBR: return b - a;
    case FPU_FMUL: case FPU_FMULP: case FPU_FIMUL: return a * b;
    case FPU_FDIV: case FPU_FDIVP: case FPU_FIDIV: return a / b;
    case FPU_FDIVR: case FPU_FDIVRP: case FPU_FIDIVR: return b / a;
    }
    rt_fatal("fpu arith %d", op);
    return 0;
}

void fpu_mem(int op, int size, uint32_t seg, uint32_t off)
{
    switch (op) {
    case FPU_FLD: push(load_float(size, seg, off)); return;
    case FPU_FILD: push((double)load_int(size, seg, off)); return;
    case FPU_FST: store_float(size, seg, off, ST(0)); return;
    case FPU_FSTP: store_float(size, seg, off, pop()); return;
    case FPU_FIST: store_int(size, seg, off, ST(0)); return;
    case FPU_FISTP: store_int(size, seg, off, pop()); return;
    case FPU_FADD: case FPU_FSUB: case FPU_FSUBR: case FPU_FMUL: case FPU_FDIV: case FPU_FDIVR:
        ST(0) = arith(op, ST(0), load_float(size, seg, off));
        return;
    case FPU_FIADD: case FPU_FISUB: case FPU_FISUBR: case FPU_FIMUL: case FPU_FIDIV: case FPU_FIDIVR:
        ST(0) = arith(op, ST(0), (double)load_int(size, seg, off));
        return;
    case FPU_FCOM: compare(ST(0), load_float(size, seg, off)); return;
    case FPU_FCOMP: compare(ST(0), load_float(size, seg, off)); pop(); return;
    case FPU_FICOM: compare(ST(0), (double)load_int(size, seg, off)); return;
    case FPU_FICOMP: compare(ST(0), (double)load_int(size, seg, off)); pop(); return;
    case FPU_FLDCW: cw = rw(seg, off); return;
    case FPU_FNSTCW: ww(seg, off, cw); return;
    case FPU_FNSTSW: ww(seg, off, fpu_status()); return;
    }
    rt_fatal("fpu mem op %d", op);
}

void fpu_reg(int op, int a, int b)
{
    switch (op) {
    case FPU_FLD: push(ST(a)); return;
    case FPU_FST: ST(a) = ST(0); return;
    case FPU_FSTP: ST(a) = ST(0); pop(); return;
    case FPU_FXCH: { double t = ST(0); ST(0) = ST(a < 0 ? 1 : a); ST(a < 0 ? 1 : a) = t; return; }
    case FPU_FADD: case FPU_FSUB: case FPU_FSUBR: case FPU_FMUL: case FPU_FDIV: case FPU_FDIVR:
        if (b < 0) { b = a; a = 0; }
        ST(a) = arith(op, ST(a), ST(b));
        return;
    case FPU_FADDP: case FPU_FSUBP: case FPU_FSUBRP: case FPU_FMULP: case FPU_FDIVP: case FPU_FDIVRP:
        if (a < 0) a = 1;
        ST(a) = arith(op, ST(a), ST(0));
        pop();
        return;
    case FPU_FCOM: compare(ST(0), ST(a < 0 ? 1 : a)); return;
    case FPU_FCOMP: compare(ST(0), ST(a < 0 ? 1 : a)); pop(); return;
    case FPU_FCOMPP: compare(ST(0), ST(1)); pop(); pop(); return;
    case FPU_FTST: compare(ST(0), 0.0); return;
    case FPU_FCHS: ST(0) = -ST(0); return;
    case FPU_FABS: ST(0) = fabs(ST(0)); return;
    case FPU_FLDZ: push(0.0); return;
    case FPU_FLD1: push(1.0); return;
    case FPU_FSQRT: ST(0) = sqrt(ST(0)); return;
    case FPU_FRNDINT: ST(0) = round_cw(ST(0)); return;
    case FPU_FNCLEX: return;
    case FPU_FNINIT: top = 0; sw_cond = 0; cw = 0x037F; return;
    case FPU_FFREE: return;
    case FPU_FINCSTP: top = (top + 1) & 7; return;
    case FPU_FDECSTP: top = (top - 1) & 7; return;
    }
    rt_fatal("fpu reg op %d", op);
}
