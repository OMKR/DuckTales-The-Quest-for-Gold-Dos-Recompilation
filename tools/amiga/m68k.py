"""MC68000 instruction decoder producing structured operands.

Only the plain 68000 instruction set is supported (no 68020+ addressing).
decode() returns an Insn with effective addresses as EA tuples so that the
recompiler can generate code without parsing disassembly text.
"""
import struct
from dataclasses import dataclass, field

# Effective address kinds
DREG, AREG, IND, POSTINC, PREDEC, DISP, INDEX, ABSW, ABSL, PCDISP, PCINDEX, IMM = range(12)

CONDITIONS = ["t", "f", "hi", "ls", "cc", "cs", "ne", "eq",
              "vc", "vs", "pl", "mi", "ge", "lt", "gt", "le"]
SIZE_BITS = {0: 1, 1: 2, 2: 4}
SUFFIX = {1: "b", 2: "w", 4: "l"}


@dataclass
class EA:
    kind: int
    reg: int = 0
    disp: int = 0          # displacement, absolute address or immediate
    xreg: int = 0          # index register 0-15 (8-15 = A0-A7)
    xlong: bool = False    # index register size
    ext_off: int = 0       # offset of the extension word from insn start

    def __str__(self):
        r = f"a{self.reg}"
        x = ("a" if self.xreg >= 8 else "d") + str(self.xreg & 7) + (".l" if self.xlong else ".w")
        return {
            DREG: f"d{self.reg}", AREG: r, IND: f"({r})", POSTINC: f"({r})+",
            PREDEC: f"-({r})", DISP: f"{self.disp}({r})", INDEX: f"{self.disp}({r},{x})",
            ABSW: f"${self.disp & 0xFFFFFFFF:x}.w", ABSL: f"${self.disp:x}.l",
            PCDISP: f"${self.disp:x}(pc)", PCINDEX: f"${self.disp:x}(pc,{x})",
            IMM: f"#${self.disp & 0xFFFFFFFF:x}",
        }[self.kind]


@dataclass
class Insn:
    pc: int
    length: int
    op: str
    size: int = 0                 # operand size in bytes, 0 if not applicable
    src: EA = None
    dst: EA = None
    cond: str = None              # for bcc/dbcc/scc
    target: int = None            # branch target (hunk offset)
    regmask: list = field(default_factory=list)   # movem registers 0-15

    def __str__(self):
        op = "b" + self.cond if self.op == "bcc" else "db" + self.cond if self.op == "dbcc" else "s" + self.cond if self.op == "scc" else self.op
        name = op + (f".{SUFFIX[self.size]}" if self.size else "")
        args = [str(e) for e in (self.src, self.dst) if e is not None]
        if self.target is not None:
            args.append(f"${self.target:x}")
        if self.regmask:
            regs = "/".join(("a" if r >= 8 else "d") + str(r & 7) for r in self.regmask)
            args.insert(0 if self.dst else 1, regs)
        return f"{name:9} {', '.join(args)}"


class DecodeError(Exception):
    pass


class Decoder:
    def __init__(self, code):
        self.code = code

    def w(self, pos):
        if pos + 2 > len(self.code):
            raise DecodeError("past end")
        return struct.unpack_from(">H", self.code, pos)[0]

    def sw(self, pos):
        v = self.w(pos)
        return v - 0x10000 if v & 0x8000 else v

    def l(self, pos):
        return (self.w(pos) << 16) | self.w(pos + 2)

    def ea(self, mode, reg, size, pos, pc):
        """Decode EA; pos is where its extension words start. Returns (EA, n)."""
        if mode == 0:
            return EA(DREG, reg), 0
        if mode == 1:
            return EA(AREG, reg), 0
        if mode == 2:
            return EA(IND, reg), 0
        if mode == 3:
            return EA(POSTINC, reg), 0
        if mode == 4:
            return EA(PREDEC, reg), 0
        if mode == 5:
            return EA(DISP, reg, self.sw(pos), ext_off=pos - pc), 2
        if mode == 6:
            return self.brief(INDEX, reg, pos, pc), 2
        if reg == 0:
            return EA(ABSW, 0, self.sw(pos), ext_off=pos - pc), 2
        if reg == 1:
            return EA(ABSL, 0, self.l(pos), ext_off=pos - pc), 4
        if reg == 2:
            return EA(PCDISP, 0, pos + self.sw(pos), ext_off=pos - pc), 2
        if reg == 3:
            e = self.brief(PCINDEX, 0, pos, pc)
            e.disp += pos
            return e, 2
        if reg == 4:
            if size == 4:
                return EA(IMM, 0, self.l(pos), ext_off=pos - pc), 4
            v = self.w(pos)
            return EA(IMM, 0, v & 0xFF if size == 1 else v, ext_off=pos - pc), 2
        raise DecodeError(f"bad EA mode 7 reg {reg}")

    def brief(self, kind, reg, pos, pc):
        ext = self.w(pos)
        if ext & 0x0100:
            raise DecodeError("68020 full extension word")
        disp = ext & 0xFF
        if disp & 0x80:
            disp -= 0x100
        return EA(kind, reg, disp, xreg=(ext >> 12) & 15, xlong=bool(ext & 0x0800),
                  ext_off=pos - pc)

    def decode(self, pc):
        op = self.w(pc)
        hi = op >> 12
        handler = getattr(self, f"g{hi:x}")
        insn = handler(op, pc)
        if insn is None:
            raise DecodeError(f"unsupported opcode {op:04x} at {pc:x}")
        return insn

    # helpers -------------------------------------------------------------
    def with_ea(self, pc, op, name, size, mode, reg, n=2, src=None, as_dst=True, **kw):
        e, k = self.ea(mode, reg, size, pc + n, pc)
        if as_dst:
            return Insn(pc, n + k, name, size, src=src, dst=e, **kw)
        return Insn(pc, n + k, name, size, src=e, dst=src, **kw)

    # 0000: immediate ops, bit ops ----------------------------------------
    def g0(self, op, pc):
        mode, reg = (op >> 3) & 7, op & 7
        if op & 0x0100:
            if mode == 1:
                return None  # movep
            kind = ["btst", "bchg", "bclr", "bset"][(op >> 6) & 3]
            size = 4 if mode == 0 else 1
            return self.with_ea(pc, op, kind, size, mode, reg,
                                src=EA(DREG, (op >> 9) & 7))
        sel = (op >> 9) & 7
        if sel == 4:
            kind = ["btst", "bchg", "bclr", "bset"][(op >> 6) & 3]
            size = 4 if mode == 0 else 1
            bit = self.w(pc + 2) & 0xFF
            return self.with_ea(pc, op, kind, size, mode, reg, n=4,
                                src=EA(IMM, 0, bit, ext_off=2))
        name = {0: "ori", 1: "andi", 2: "subi", 3: "addi", 5: "eori", 6: "cmpi"}.get(sel)
        if name is None or (op >> 6) & 3 == 3:
            return None
        size = SIZE_BITS[(op >> 6) & 3]
        if mode == 7 and reg == 4:
            # to CCR / SR
            imm, _ = self.ea(7, 4, 2, pc + 2, pc)
            return Insn(pc, 4, name + ("_ccr" if size == 1 else "_sr"), 0, src=imm)
        imm, k = self.ea(7, 4, size, pc + 2, pc)
        e, k2 = self.ea(mode, reg, size, pc + 2 + k, pc)
        return Insn(pc, 2 + k + k2, name, size, src=imm, dst=e)

    # 0001-0011: move ------------------------------------------------------
    def move(self, op, pc, size):
        src, k = self.ea((op >> 3) & 7, op & 7, size, pc + 2, pc)
        dmode, dreg = (op >> 6) & 7, (op >> 9) & 7
        dst, k2 = self.ea(dmode, dreg, size, pc + 2 + k, pc)
        return Insn(pc, 2 + k + k2, "movea" if dmode == 1 else "move", size, src=src, dst=dst)

    def g1(self, op, pc):
        return self.move(op, pc, 1)

    def g2(self, op, pc):
        return self.move(op, pc, 4)

    def g3(self, op, pc):
        return self.move(op, pc, 2)

    # 0100: miscellaneous ---------------------------------------------------
    def g4(self, op, pc):
        mode, reg = (op >> 3) & 7, op & 7
        simple = {0x4E75: "rts", 0x4E73: "rte", 0x4E77: "rtr", 0x4E71: "nop",
                  0x4AFC: "illegal", 0x4E70: "reset", 0x4E76: "trapv"}
        if op in simple:
            return Insn(pc, 2, simple[op])
        if op & 0xFFF0 == 0x4E40:
            return Insn(pc, 2, "trap", src=EA(IMM, 0, op & 15))
        if op & 0xFFF8 == 0x4E50:
            return Insn(pc, 4, "link", src=EA(AREG, reg), dst=EA(IMM, 0, self.sw(pc + 2), ext_off=2))
        if op & 0xFFF8 == 0x4E58:
            return Insn(pc, 2, "unlk", src=EA(AREG, reg))
        if op & 0xFFC0 == 0x4E80:
            return self.with_ea(pc, op, "jsr", 0, mode, reg)
        if op & 0xFFC0 == 0x4EC0:
            return self.with_ea(pc, op, "jmp", 0, mode, reg)
        if op & 0xF1C0 == 0x41C0:
            return self.with_ea(pc, op, "lea", 4, mode, reg, as_dst=False,
                                src=EA(AREG, (op >> 9) & 7))
        if op & 0xF1C0 == 0x4180:
            return self.with_ea(pc, op, "chk", 2, mode, reg, as_dst=False,
                                src=EA(DREG, (op >> 9) & 7))
        if op & 0xFFF8 == 0x4840:
            return Insn(pc, 2, "swap", 4, dst=EA(DREG, reg))
        if op & 0xFFF8 == 0x4880:
            return Insn(pc, 2, "ext", 2, dst=EA(DREG, reg))
        if op & 0xFFF8 == 0x48C0:
            return Insn(pc, 2, "ext", 4, dst=EA(DREG, reg))
        if op & 0xFFC0 == 0x4840:
            return self.with_ea(pc, op, "pea", 4, mode, reg)
        if op & 0xFB80 == 0x4880:
            size = 4 if op & 0x40 else 2
            mask = self.w(pc + 2)
            e, k = self.ea(mode, reg, size, pc + 4, pc)
            if mode == 4:  # predecrement: reversed mask
                regs = [15 - i for i in range(16) if mask & (1 << i)]
            else:
                regs = [i for i in range(16) if mask & (1 << i)]
            regs.sort()
            if op & 0x0400:
                return Insn(pc, 4 + k, "movem", size, src=e, regmask=regs)
            return Insn(pc, 4 + k, "movem", size, dst=e, regmask=regs)
        if op & 0xFFC0 == 0x40C0:
            return self.with_ea(pc, op, "move_from_sr", 2, mode, reg)
        if op & 0xFFC0 == 0x44C0:
            return self.with_ea(pc, op, "move_to_ccr", 2, mode, reg, as_dst=False)
        if op & 0xFFC0 == 0x46C0:
            return self.with_ea(pc, op, "move_to_sr", 2, mode, reg, as_dst=False)
        if op & 0xFFC0 == 0x4AC0:
            return self.with_ea(pc, op, "tas", 1, mode, reg)
        unary = {0x4000: "negx", 0x4200: "clr", 0x4400: "neg", 0x4600: "not", 0x4A00: "tst"}
        name = unary.get(op & 0xFF00)
        if name and (op >> 6) & 3 != 3:
            return self.with_ea(pc, op, name, SIZE_BITS[(op >> 6) & 3], mode, reg)
        return None

    # 0101: addq/subq/scc/dbcc -----------------------------------------------
    def g5(self, op, pc):
        mode, reg = (op >> 3) & 7, op & 7
        if (op >> 6) & 3 == 3:
            cond = CONDITIONS[(op >> 8) & 15]
            if mode == 1:
                return Insn(pc, 4, "dbcc", 2, dst=EA(DREG, reg), cond=cond,
                            target=pc + 2 + self.sw(pc + 2))
            return self.with_ea(pc, op, "scc", 1, mode, reg, cond=cond)
        q = (op >> 9) & 7 or 8
        name = "subq" if op & 0x100 else "addq"
        return self.with_ea(pc, op, name, SIZE_BITS[(op >> 6) & 3], mode, reg,
                            src=EA(IMM, 0, q))

    # 0110: branches ------------------------------------------------------
    def g6(self, op, pc):
        cond = (op >> 8) & 15
        disp = op & 0xFF
        if disp == 0:
            target, n = pc + 2 + self.sw(pc + 2), 4
        else:
            target, n = pc + 2 + (disp - 0x100 if disp & 0x80 else disp), 2
        name = {0: "bra", 1: "bsr"}.get(cond, "bcc")
        return Insn(pc, n, name, cond=CONDITIONS[cond] if cond > 1 else None, target=target)

    def g7(self, op, pc):
        if op & 0x100:
            return None
        v = op & 0xFF
        return Insn(pc, 2, "moveq", 4, src=EA(IMM, 0, v - 0x100 if v & 0x80 else v),
                    dst=EA(DREG, (op >> 9) & 7))

    # arithmetic/logic groups ------------------------------------------------
    def arith(self, op, pc, name, has_addr):
        mode, reg, dn = (op >> 3) & 7, op & 7, (op >> 9) & 7
        opmode = (op >> 6) & 7
        if has_addr and opmode in (3, 7):
            size = 2 if opmode == 3 else 4
            return self.with_ea(pc, op, name + "a", size, mode, reg, as_dst=False,
                                src=EA(AREG, dn))
        size = SIZE_BITS.get(opmode & 3)
        if opmode & 4:
            return self.with_ea(pc, op, name, size, mode, reg, src=EA(DREG, dn))
        return self.with_ea(pc, op, name, size, mode, reg, as_dst=False, src=EA(DREG, dn))

    def xop(self, op, pc, name):
        """addx/subx/abcd/sbcd/cmpm: register or predecrement memory forms."""
        size = SIZE_BITS[(op >> 6) & 3] if name not in ("abcd", "sbcd") else 1
        rx, ry = (op >> 9) & 7, op & 7
        if op & 8:
            kind = POSTINC if name == "cmpm" else PREDEC
            return Insn(pc, 2, name, size, src=EA(kind, ry), dst=EA(kind, rx))
        return Insn(pc, 2, name, size, src=EA(DREG, ry), dst=EA(DREG, rx))

    def g8(self, op, pc):
        opmode = (op >> 6) & 7
        if opmode in (3, 7):
            return self.with_ea(pc, op, "divu" if opmode == 3 else "divs", 2,
                                (op >> 3) & 7, op & 7, as_dst=False,
                                src=EA(DREG, (op >> 9) & 7))
        if op & 0x1F0 == 0x100:
            return self.xop(op, pc, "sbcd")
        return self.arith(op, pc, "or", False)

    def g9(self, op, pc):
        if op & 0x130 == 0x100 and (op >> 6) & 3 != 3:
            return self.xop(op, pc, "subx")
        return self.arith(op, pc, "sub", True)

    def gb(self, op, pc):
        opmode = (op >> 6) & 7
        if opmode in (3, 7):
            return self.arith(op, pc, "cmp", True)
        if opmode & 4:
            if (op >> 3) & 7 == 1:
                return self.xop(op, pc, "cmpm")
            return self.arith(op, pc, "eor", False)
        return self.arith(op, pc, "cmp", False)

    def gc(self, op, pc):
        opmode = (op >> 6) & 7
        if opmode in (3, 7):
            return self.with_ea(pc, op, "mulu" if opmode == 3 else "muls", 2,
                                (op >> 3) & 7, op & 7, as_dst=False,
                                src=EA(DREG, (op >> 9) & 7))
        if op & 0x1F0 == 0x100:
            return self.xop(op, pc, "abcd")
        if op & 0x1F8 in (0x140, 0x148, 0x188):
            rx, ry = (op >> 9) & 7, op & 7
            kinds = {0x140: (DREG, DREG), 0x148: (AREG, AREG), 0x188: (DREG, AREG)}
            a, b = kinds[op & 0x1F8]
            return Insn(pc, 2, "exg", 4, src=EA(a, rx), dst=EA(b, ry))
        return self.arith(op, pc, "and", False)

    def gd(self, op, pc):
        if op & 0x130 == 0x100 and (op >> 6) & 3 != 3:
            return self.xop(op, pc, "addx")
        return self.arith(op, pc, "add", True)

    def ge(self, op, pc):
        kinds = ["as", "ls", "rox", "ro"]
        direction = "l" if op & 0x100 else "r"
        if (op >> 6) & 3 == 3:
            name = kinds[(op >> 9) & 3] + direction
            return self.with_ea(pc, op, name, 2, (op >> 3) & 7, op & 7,
                                src=EA(IMM, 0, 1))
        name = kinds[(op >> 3) & 3] + direction
        size = SIZE_BITS[(op >> 6) & 3]
        cnt = (op >> 9) & 7
        src = EA(DREG, cnt) if op & 0x20 else EA(IMM, 0, cnt or 8)
        return Insn(pc, 2, name, size, src=src, dst=EA(DREG, op & 7))

    def ga(self, op, pc):
        return Insn(pc, 2, "linea", src=EA(IMM, 0, op))

    def gf(self, op, pc):
        return Insn(pc, 2, "linef", src=EA(IMM, 0, op))
