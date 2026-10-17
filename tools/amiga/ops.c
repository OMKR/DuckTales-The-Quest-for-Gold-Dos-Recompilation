/* 68000 shift, rotate and divide instructions with exact condition codes. */
#include "m68k.h"

static uint32_t mask_of(int size) { return size == 1 ? MASK1 : size == 2 ? MASK2 : MASK4; }
static uint32_t msb_of(int size) { return size == 1 ? MSB1 : size == 2 ? MSB2 : MSB4; }

static void nz(uint32_t v, int size)
{
    R.n = (v & msb_of(size)) != 0;
    R.z = (v & mask_of(size)) == 0;
}

uint32_t op_asl(uint32_t v, uint32_t cnt, int size)
{
    uint32_t m = mask_of(size), msb = msb_of(size);
    R.v = 0;
    R.c = 0;
    v &= m;
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t before = v;
        R.c = R.x = (v & msb) != 0;
        v = (v << 1) & m;
        if ((before ^ v) & msb) R.v = 1;
    }
    nz(v, size);
    return v;
}

uint32_t op_asr(uint32_t v, uint32_t cnt, int size)
{
    uint32_t m = mask_of(size), msb = msb_of(size);
    R.v = 0;
    R.c = 0;
    v &= m;
    for (uint32_t i = 0; i < cnt; i++) {
        R.c = R.x = v & 1;
        v = ((v >> 1) | (v & msb)) & m;
    }
    nz(v, size);
    return v;
}

uint32_t op_lsl(uint32_t v, uint32_t cnt, int size)
{
    uint32_t m = mask_of(size), msb = msb_of(size);
    R.v = 0;
    R.c = 0;
    v &= m;
    for (uint32_t i = 0; i < cnt; i++) {
        R.c = R.x = (v & msb) != 0;
        v = (v << 1) & m;
    }
    nz(v, size);
    return v;
}

uint32_t op_lsr(uint32_t v, uint32_t cnt, int size)
{
    uint32_t m = mask_of(size);
    R.v = 0;
    R.c = 0;
    v &= m;
    for (uint32_t i = 0; i < cnt; i++) {
        R.c = R.x = v & 1;
        v >>= 1;
    }
    nz(v, size);
    return v;
}

uint32_t op_rol(uint32_t v, uint32_t cnt, int size)
{
    uint32_t m = mask_of(size), msb = msb_of(size);
    R.v = 0;
    R.c = 0;
    v &= m;
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t out = (v & msb) != 0;
        v = ((v << 1) | out) & m;
        R.c = out;
    }
    nz(v, size);
    return v;
}

uint32_t op_ror(uint32_t v, uint32_t cnt, int size)
{
    uint32_t m = mask_of(size), msb = msb_of(size);
    R.v = 0;
    R.c = 0;
    v &= m;
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t out = v & 1;
        v = (v >> 1) | (out ? msb : 0);
        R.c = out;
    }
    nz(v, size);
    return v;
}

uint32_t op_roxl(uint32_t v, uint32_t cnt, int size)
{
    uint32_t m = mask_of(size), msb = msb_of(size);
    R.v = 0;
    v &= m;
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t out = (v & msb) != 0;
        v = ((v << 1) | R.x) & m;
        R.x = out;
    }
    R.c = R.x;
    nz(v, size);
    return v;
}

uint32_t op_roxr(uint32_t v, uint32_t cnt, int size)
{
    uint32_t m = mask_of(size), msb = msb_of(size);
    R.v = 0;
    v &= m;
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t out = v & 1;
        v = (v >> 1) | (R.x ? msb : 0);
        R.x = out;
    }
    R.c = R.x;
    nz(v, size);
    return v;
}

void op_divu(int dn, uint32_t src)
{
    src &= 0xFFFF;
    if (src == 0) rt_fatal("divu by zero");
    uint32_t q = R.d[dn] / src, r = R.d[dn] % src;
    R.c = 0;
    if (q > 0xFFFF) { R.v = 1; return; }
    R.d[dn] = (r << 16) | q;
    R.v = 0;
    R.n = (q & 0x8000) != 0;
    R.z = q == 0;
}

void op_divs(int dn, uint32_t src)
{
    int32_t s = (int16_t)src;
    if (s == 0) rt_fatal("divs by zero");
    int32_t dividend = (int32_t)R.d[dn];
    if (dividend == INT32_MIN && s == -1) { R.v = 1; R.c = 0; return; }
    int32_t q = dividend / s, r = dividend % s;
    R.c = 0;
    if (q < -32768 || q > 32767) { R.v = 1; return; }
    R.d[dn] = ((uint32_t)(r & 0xFFFF) << 16) | (q & 0xFFFF);
    R.v = 0;
    R.n = (q & 0x8000) != 0;
    R.z = (q & 0xFFFF) == 0;
}
