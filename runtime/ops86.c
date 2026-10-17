/* 8086 shifts, multiply/divide and string instructions. */
#include "cpu86.h"

uint32_t op_shift(int kind, uint32_t v, uint32_t count, uint32_t bits)
{
    uint32_t m = bits == 8 ? 0xFF : 0xFFFF, top = 1u << (bits - 1);
    count &= 0xFF;
    if (count == 0) return v;
    v &= m;
    for (uint32_t i = 0; i < count; i++) {
        switch (kind) {
        case SH_SHL: case SH_SAL:
            R.cf = (v & top) != 0;
            v = (v << 1) & m;
            break;
        case SH_SHR:
            R.cf = v & 1;
            v >>= 1;
            break;
        case SH_SAR:
            R.cf = v & 1;
            v = (v >> 1) | (v & top);
            break;
        case SH_ROL:
            R.cf = (v & top) != 0;
            v = ((v << 1) | R.cf) & m;
            break;
        case SH_ROR:
            R.cf = v & 1;
            v = (v >> 1) | (R.cf ? top : 0);
            break;
        case SH_RCL: {
            uint32_t out = (v & top) != 0;
            v = ((v << 1) | R.cf) & m;
            R.cf = out;
            break;
        }
        case SH_RCR: {
            uint32_t out = v & 1;
            v = (v >> 1) | (R.cf ? top : 0);
            R.cf = out;
            break;
        }
        }
    }
    uint32_t msb = (v & top) != 0;
    switch (kind) {
    case SH_SHL: case SH_SAL: R.of = msb ^ R.cf; szp(v, bits); break;
    case SH_SHR: R.of = 0; szp(v, bits); break;   /* OF for count 1 = old MSB; rarely tested */
    case SH_SAR: R.of = 0; szp(v, bits); break;
    case SH_ROL: case SH_RCL: R.of = msb ^ R.cf; break;
    case SH_ROR: case SH_RCR: R.of = msb ^ ((v & (top >> 1)) != 0); break;
    }
    return v;
}

void op_mul8(uint32_t v)
{
    R.ax = (uint16_t)(LO(R.ax) * (v & 0xFF));
    R.cf = R.of = (R.ax >> 8) != 0;
}

void op_imul8(uint32_t v)
{
    int16_t r = (int16_t)((int8_t)LO(R.ax) * (int8_t)v);
    R.ax = (uint16_t)r;
    R.cf = R.of = r != (int8_t)r;
}

void op_mul16(uint32_t v)
{
    uint32_t r = (uint32_t)R.ax * (v & 0xFFFF);
    R.ax = (uint16_t)r;
    R.dx = (uint16_t)(r >> 16);
    R.cf = R.of = R.dx != 0;
}

void op_imul16(uint32_t v)
{
    int32_t r = (int32_t)(int16_t)R.ax * (int16_t)v;
    R.ax = (uint16_t)r;
    R.dx = (uint16_t)((uint32_t)r >> 16);
    R.cf = R.of = r != (int16_t)r;
}

uint32_t op_imul3(uint32_t a, uint32_t b)
{
    int32_t r = (int32_t)(int16_t)a * (int16_t)b;
    R.cf = R.of = r != (int16_t)r;
    return (uint16_t)r;
}

static void div_error(void) { rt_fatal("divide error"); }

void op_div8(uint32_t v)
{
    v &= 0xFF;
    if (!v) div_error();
    uint32_t q = R.ax / v, r = R.ax % v;
    if (q > 0xFF) div_error();
    R.ax = (uint16_t)((r << 8) | q);
}

void op_idiv8(uint32_t v)
{
    int32_t d = (int8_t)v;
    if (!d) div_error();
    int32_t n = (int16_t)R.ax, q = n / d, r = n % d;
    if (q > 127 || q < -128) div_error();
    R.ax = (uint16_t)(((r & 0xFF) << 8) | (q & 0xFF));
}

void op_div16(uint32_t v)
{
    v &= 0xFFFF;
    if (!v) div_error();
    uint32_t n = ((uint32_t)R.dx << 16) | R.ax, q = n / v, r = n % v;
    if (q > 0xFFFF) div_error();
    R.ax = (uint16_t)q;
    R.dx = (uint16_t)r;
}

void op_idiv16(uint32_t v)
{
    int32_t d = (int16_t)v;
    if (!d) div_error();
    int32_t n = (int32_t)(((uint32_t)R.dx << 16) | R.ax);
    int32_t q = n / d, r = n % d;
    if (q > 32767 || q < -32768) div_error();
    R.ax = (uint16_t)q;
    R.dx = (uint16_t)r;
}

/* String instructions. rep: 0 none, 1 rep/repe, 2 repne. */
#define STEP(size) (R.df ? -(size) : (size))

void str_movs(int size, uint32_t seg, int rep)
{
    do {
        if (rep && R.cx == 0) return;
        if (size == 1) wb(R.es, R.di, rb(seg, R.si));
        else ww(R.es, R.di, rw(seg, R.si));
        R.si += STEP(size);
        R.di += STEP(size);
        if (rep) R.cx--;
    } while (rep);
}

void str_stos(int size, uint32_t seg, int rep)
{
    (void)seg;
    do {
        if (rep && R.cx == 0) return;
        if (size == 1) wb(R.es, R.di, LO(R.ax));
        else ww(R.es, R.di, R.ax);
        R.di += STEP(size);
        if (rep) R.cx--;
    } while (rep);
}

void str_lods(int size, uint32_t seg, int rep)
{
    do {
        if (rep && R.cx == 0) return;
        if (size == 1) SET_LO(R.ax, rb(seg, R.si));
        else R.ax = (uint16_t)rw(seg, R.si);
        R.si += STEP(size);
        if (rep) R.cx--;
    } while (rep);
}

void str_cmps(int size, uint32_t seg, int rep)
{
    do {
        if (rep && R.cx == 0) return;
        uint32_t a = size == 1 ? rb(seg, R.si) : rw(seg, R.si);
        uint32_t b = size == 1 ? rb(R.es, R.di) : rw(R.es, R.di);
        f_sub(a, b, 0, size * 8);
        R.si += STEP(size);
        R.di += STEP(size);
        if (rep) {
            R.cx--;
            if (rep == 1 && !R.zf) return;
            if (rep == 2 && R.zf) return;
        }
    } while (rep);
}

void str_scas(int size, uint32_t seg, int rep)
{
    (void)seg;
    do {
        if (rep && R.cx == 0) return;
        uint32_t a = size == 1 ? LO(R.ax) : R.ax;
        uint32_t b = size == 1 ? rb(R.es, R.di) : rw(R.es, R.di);
        f_sub(a, b, 0, size * 8);
        R.di += STEP(size);
        if (rep) {
            R.cx--;
            if (rep == 1 && !R.zf) return;
            if (rep == 2 && R.zf) return;
        }
    } while (rep);
}
