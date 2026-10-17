/* 8087 emulation for the recompiled code (the game used MSC's emulator). */
#ifndef FPU86_H
#define FPU86_H

#include <stdint.h>

enum {
    FPU_FLD, FPU_FILD, FPU_FST, FPU_FSTP, FPU_FIST, FPU_FISTP,
    FPU_FADD, FPU_FADDP, FPU_FSUB, FPU_FSUBP, FPU_FSUBR, FPU_FSUBRP,
    FPU_FMUL, FPU_FMULP, FPU_FDIV, FPU_FDIVP, FPU_FDIVR, FPU_FDIVRP,
    FPU_FIADD, FPU_FISUB, FPU_FISUBR, FPU_FIMUL, FPU_FIDIV, FPU_FIDIVR,
    FPU_FCOM, FPU_FCOMP, FPU_FCOMPP, FPU_FICOM, FPU_FICOMP, FPU_FTST,
    FPU_FCHS, FPU_FABS, FPU_FLDZ, FPU_FLD1, FPU_FXCH, FPU_FSQRT, FPU_FRNDINT,
    FPU_FLDCW, FPU_FNSTCW, FPU_FNSTSW, FPU_FNCLEX, FPU_FNINIT, FPU_FFREE,
    FPU_FINCSTP, FPU_FDECSTP,
};

/* memory operand of `size` bytes at seg:off */
void fpu_mem(int op, int size, uint32_t seg, uint32_t off);
/* register forms: a, b are st(i) indexes or -1 when absent */
void fpu_reg(int op, int a, int b);
uint32_t fpu_status(void);

#endif
