"""Static recompiler: DuckTales 68000 code -> portable C.

Usage: recomp.py <DuckTales executable> <output dir>

Each discovered function becomes `void fNN_OOOOO(void)` operating on the
global CPU state R and the emulated memory. The original executable is
loaded at run time (runtime/loader.c) at the addresses in layout.h, so the
generated C contains code only, no game data.
"""
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from analyze import Analyzer, Program  # noqa: E402
from disasm import load  # noqa: E402
from m68k import (ABSL, ABSW, AREG, DISP, DREG, IMM, IND, INDEX, PCDISP,  # noqa: E402
                  PCINDEX, POSTINC, PREDEC, Decoder)

LOAD_BASE = 0x00010000

# Functions that only run when a runtime gate allows it. The music player is
# called from the vertical blank server; the runtime calls it itself at a
# fixed 50 Hz so the music tempo does not follow the adjustable frame rate.
GATED = {
    (0, 0x58BC): "music_gate()",
}

# The game's memory pool: main allocates a fixed 266000-byte pool (AllocMem
# + its own Allocate/Deallocate) and frees it with the same constant. On the
# original 512K machine that was all it could get, and some scenes come close
# to exhausting it ("alloc ... FAILED"). There is plenty of memory now, so
# every use of the constant is enlarged consistently.
POOL_SIZE_OLD = 0x40F10
POOL_SIZE_NEW = 0x80000

# Code run at the start of a function, before the original body.
# Copy protection: node 11 0x942 shows the code wheel symbols and returns
# d0 = 1 for a correct answer, 0 otherwise. With the setting off it returns 1
# at once, without showing the screen.
PROLOGUE = {
    (11, 0x942): "if (copy_protection_off()) { R.d[0] = 1; RTS(); }",
}

# Functions whose original body is replaced by a plain return.
REPLACED = {
    # Aztec C overlay preload (called by main with a stub address): it
    # LoadSegs the node unless the stub was already patched to a jmp. All
    # overlay nodes are resident and stubs are direct calls here.
    (0, 0x7978): "overlay preload: every overlay node is already resident",
}
ALIGN = 0x1000


def hunk_of_node(node):
    return 0 if node == 0 else node + 2


def fname(node, off):
    return f"f{node:02d}_{off:05x}"


def h(v):
    return f"0x{v & 0xFFFFFFFF:X}u"


class Layout:
    """Hunk base addresses: hunks placed consecutively from LOAD_BASE."""

    def __init__(self, nodes):
        sizes = {hk["num"]: hk["alloc"] for hunks in nodes for hk in hunks}
        self.base, addr = {}, LOAD_BASE
        for num in sorted(sizes):
            self.base[num] = addr
            addr = (addr + sizes[num] + ALIGN - 1) & ~(ALIGN - 1)
        self.sizes = sizes
        self.end = addr

    def addr(self, node, off):
        return self.base[hunk_of_node(node)] + off


class Operand:
    """C access to one effective address of a given size."""

    def __init__(self, gen, insn, ea, size, var):
        self.gen, self.insn, self.ea, self.size, self.var = gen, insn, ea, size, var
        self.pre = []
        self.addr = None
        k = ea.kind
        if k in (IND, DISP, INDEX, ABSW, ABSL, PCDISP, PCINDEX):
            self.pre.append(f"{var} = {gen.address(insn, ea)};")
            self.addr = var
        elif k == POSTINC:
            step = 2 if size == 1 and ea.reg == 7 else size
            self.pre.append(f"{var} = R.a[{ea.reg}]; R.a[{ea.reg}] += {step};")
            self.addr = var
        elif k == PREDEC:
            step = 2 if size == 1 and ea.reg == 7 else size
            self.pre.append(f"R.a[{ea.reg}] -= {step}; {var} = R.a[{ea.reg}];")
            self.addr = var

    def read(self):
        k, s = self.ea.kind, self.size
        if k == DREG:
            return f"R.d[{self.ea.reg}]" if s == 4 else f"(R.d[{self.ea.reg}] & MASK{s})"
        if k == AREG:
            return f"R.a[{self.ea.reg}]" if s == 4 else f"(R.a[{self.ea.reg}] & MASK{s})"
        if k == IMM:
            return self.gen.immediate(self.insn, self.ea, s)
        return {1: "rb", 2: "rw", 4: "rl"}[s] + f"({self.addr})"

    def write(self, value):
        k, s = self.ea.kind, self.size
        if k == DREG:
            return f"set_d({self.ea.reg}, {value}, {s});"
        if k == AREG:
            return f"R.a[{self.ea.reg}] = {value};"
        return {1: "wb", 2: "ww", 4: "wl"}[s] + f"({self.addr}, {value});"


class FunctionGen:
    def __init__(self, rc, node, entry):
        self.rc, self.node, self.entry = rc, node, entry
        self.relocs = rc.relocs.get(hunk_of_node(node), {})
        self.lines = []

    # address helpers -----------------------------------------------------
    def reloc(self, insn, ea, value):
        target = self.relocs.get(insn.pc + ea.ext_off)
        if target is not None:
            return self.rc.layout.base[target] + value
        return None

    def index_expr(self, ea):
        reg = f"R.a[{ea.xreg - 8}]" if ea.xreg >= 8 else f"R.d[{ea.xreg}]"
        return reg if ea.xlong else f"SX16({reg})"

    def address(self, insn, ea):
        k = ea.kind
        if k == IND:
            return f"R.a[{ea.reg}]"
        if k == DISP:
            return f"(R.a[{ea.reg}] + {h(ea.disp)})"
        if k == INDEX:
            return f"(R.a[{ea.reg}] + {h(ea.disp)} + {self.index_expr(ea)})"
        if k == ABSW:
            return h(ea.disp)
        if k == ABSL:
            rel = self.reloc(insn, ea, ea.disp)
            if rel is None and ea.disp == POOL_SIZE_OLD and insn.op == "pea":
                return h(POOL_SIZE_NEW)
            return h(rel if rel is not None else ea.disp)
        if k == PCDISP:
            return h(self.rc.layout.addr(self.node, ea.disp))
        if k == PCINDEX:
            return f"({h(self.rc.layout.addr(self.node, ea.disp))} + {self.index_expr(ea)})"
        raise ValueError(f"no address for EA kind {k}")

    def immediate(self, insn, ea, size):
        if size == 4:
            rel = self.reloc(insn, ea, ea.disp)
            if rel is not None:
                return h(rel)
        return h(ea.disp & {1: 0xFF, 2: 0xFFFF, 4: 0xFFFFFFFF}[size])

    def ret_addr(self, insn):
        return h(self.rc.layout.addr(self.node, insn.pc + insn.length))

    def poll(self, insn):
        """POLL() on backward branches, so loops let time pass."""
        if insn.target > insn.pc:
            return ""
        return f"POLL({h(self.rc.layout.addr(self.node, insn.pc))}); "

    def label(self, off):
        return f"L_{off:05x}"

    def emit(self, line):
        self.lines.append("    " + line)

    # control flow ----------------------------------------------------------
    def call_known(self, insn, target, tail):
        name = fname(*target)
        self.rc.called.add(target)
        if tail:
            self.emit(f"{name}(); return;")
        else:
            self.emit(f"push32({self.ret_addr(insn)}); {name}();")

    def resolve_static(self, insn):
        """Static target (node, off) of jsr/jmp, or None."""
        an = self.rc.an
        ea = insn.dst
        if ea.kind == PCDISP:
            return self.node, ea.disp
        if ea.kind == ABSL and self.reloc(insn, ea, 0) is not None:
            if self.relocs[insn.pc + ea.ext_off] == 0:
                return 0, ea.disp
        if ea.kind == DISP and ea.reg == 4:
            return an.prog.a4_targets.get(ea.disp)
        return None

    def gen_jsr_jmp(self, insn):
        tail = insn.op == "jmp"
        ea = insn.dst
        if ea.kind == DISP and ea.reg == 6 and ea.disp < 0:
            self.emit(f"lib_call(R.a[6], {ea.disp});")
            if tail:
                self.emit("R.a[7] += 4; return;")
            return
        if tail and (self.node, insn.pc) in self.rc.an.switches:
            self.gen_switch(insn, self.rc.an.switches[(self.node, insn.pc)][1])
            return
        if tail and ea.kind == IND and self.rc.table_jumps.get((self.node, insn.pc)):
            self.gen_switch(insn, self.rc.table_jumps[(self.node, insn.pc)])
            return
        target = self.resolve_static(insn)
        if target is not None:
            if tail and target[0] == self.node and target in self.body \
                    and target not in self.rc.an.functions:
                self.emit(f"goto {self.label(target[1])};")
            else:
                self.call_known(insn, target, tail)
            return
        addr = self.address(insn, ea)
        if tail:
            self.emit(f"call_indirect_tail({addr}); return;")
        else:
            self.emit(f"call_indirect_ret({addr}, {self.ret_addr(insn)});")

    def gen_switch(self, insn, targets):
        addr = self.address(insn, insn.dst)
        self.emit(f"switch ({addr}) {{")
        for t in sorted(set(targets)):
            self.emit(f"case {h(self.rc.layout.addr(self.node, t))}: goto {self.label(t)};")
            self.jump_targets.add(t)
        self.emit(f"default: rt_fatal(\"bad switch target %x at {fname(self.node, insn.pc)}\", "
                  f"(unsigned){addr});")
        self.emit("}")
        self.emit("return;")

    # instruction translation -------------------------------------------------
    def gen(self, insn):
        op, s = insn.op, insn.size
        m, msb = f"MASK{s}", f"MSB{s}"
        e = self.emit

        def opnd(ea, var, size=s):
            o = Operand(self, insn, ea, size, var)
            for line in o.pre:
                e(line)
            return o

        if op in ("move",):
            src = opnd(insn.src, "ea1")
            e(f"t = {src.read()};")
            dst = opnd(insn.dst, "ea2")
            e(dst.write("t"))
            e(f"flags_logic(t, {msb});")
        elif op == "movea":
            src = opnd(insn.src, "ea1")
            v = f"SX16({src.read()})" if s == 2 else src.read()
            e(f"R.a[{insn.dst.reg}] = {v};")
        elif op == "moveq":
            e(f"R.d[{insn.dst.reg}] = {h(insn.src.disp)}; flags_logic(R.d[{insn.dst.reg}], MSB4);")
        elif op == "clr":
            dst = opnd(insn.dst, "ea2")
            e(dst.write("0"))
            e("R.n = 0; R.z = 1; R.v = 0; R.c = 0;")
        elif op == "tst":
            src = opnd(insn.dst, "ea2")
            e(f"flags_logic({src.read()}, {msb});")
        elif op == "lea":
            e(f"R.a[{insn.dst.reg}] = {self.address(insn, insn.src)};")
        elif op == "pea":
            e(f"push32({self.address(insn, insn.dst)});")
        elif op == "ext":
            r = insn.dst.reg
            if s == 2:
                e(f"set_d({r}, SX8(R.d[{r}]), 2); flags_logic(R.d[{r}] & MASK2, MSB2);")
            else:
                e(f"R.d[{r}] = SX16(R.d[{r}]); flags_logic(R.d[{r}], MSB4);")
        elif op == "swap":
            r = insn.dst.reg
            e(f"R.d[{r}] = (R.d[{r}] >> 16) | (R.d[{r}] << 16); flags_logic(R.d[{r}], MSB4);")
        elif op == "link":
            r = insn.src.reg
            e(f"push32(R.a[{r}]); R.a[{r}] = R.a[7]; R.a[7] += {h(insn.dst.disp)};")
        elif op == "unlk":
            r = insn.src.reg
            e(f"R.a[7] = R.a[{r}]; R.a[{r}] = pop32();")
        elif op in ("add", "sub", "addi", "subi", "addq", "subq"):
            base = op.rstrip("iq")
            sym = "+" if base == "add" else "-"
            if insn.dst.kind == AREG:  # addq/subq to An: whole register, no flags
                e(f"R.a[{insn.dst.reg}] {sym}= {h(insn.src.disp)};")
                return
            src = opnd(insn.src, "ea1")
            e(f"s = {src.read()};")
            dst = opnd(insn.dst, "ea2")
            e(f"d = {dst.read()}; t = (d {sym} s) & {m};")
            e(dst.write("t"))
            e(f"flags_{base}(s, d, t, {msb}, {m});")
        elif op in ("adda", "suba"):
            src = opnd(insn.src, "ea1")
            v = f"SX16({src.read()})" if s == 2 else src.read()
            sym = "+" if op == "adda" else "-"
            e(f"R.a[{insn.dst.reg}] {sym}= {v};")
        elif op in ("addx", "subx"):
            src = opnd(insn.src, "ea1")
            e(f"s = {src.read()};")
            dst = opnd(insn.dst, "ea2")
            sym = "+" if op == "addx" else "-"
            e(f"d = {dst.read()}; t = (d {sym} s {sym} R.x) & {m};")
            e(dst.write("t"))
            e(f"{{ uint32_t z = R.z; flags_{op[:3]}(s, d, t, {msb}, {m}); R.z = z && t == 0; }}")
        elif op in ("cmp", "cmpi", "cmpm"):
            src = opnd(insn.src, "ea1")
            e(f"s = {src.read()};")
            dst = opnd(insn.dst, "ea2")
            e(f"d = {dst.read()}; t = (d - s) & {m}; flags_cmp(s, d, t, {msb}, {m});")
        elif op == "cmpa":
            src = opnd(insn.src, "ea1")
            v = f"SX16({src.read()})" if s == 2 else src.read()
            e(f"s = {v}; d = R.a[{insn.dst.reg}]; t = d - s; flags_cmp(s, d, t, MSB4, MASK4);")
        elif op in ("and", "or", "eor", "andi", "ori", "eori"):
            sym = {"and": "&", "or": "|", "eor": "^"}[op.rstrip("i")]
            src = opnd(insn.src, "ea1")
            e(f"s = {src.read()};")
            dst = opnd(insn.dst, "ea2")
            e(f"t = ({dst.read()} {sym} s) & {m};")
            e(dst.write("t"))
            e(f"flags_logic(t, {msb});")
        elif op in ("andi_ccr", "ori_ccr", "eori_ccr", "andi_sr", "ori_sr", "eori_sr"):
            sym = {"and": "&", "or": "|", "eor": "^"}[op.split("_")[0][:-1]]
            e(f"set_ccr((get_ccr() {sym} {h(insn.src.disp)}) & 0x1F);")
            if op.endswith("_sr"):
                e(f"R.sr_hi = (R.sr_hi {sym} ({h(insn.src.disp)} >> 8)) & 0xFF;")
        elif op == "not":
            dst = opnd(insn.dst, "ea2")
            e(f"t = ~{dst.read()} & {m};")
            e(dst.write("t"))
            e(f"flags_logic(t, {msb});")
        elif op in ("neg", "negx"):
            dst = opnd(insn.dst, "ea2")
            x = " - R.x" if op == "negx" else ""
            e(f"d = {dst.read()}; t = (0 - d{x}) & {m};")
            e(dst.write("t"))
            if op == "negx":
                e(f"{{ uint32_t z = R.z; flags_sub(d, 0, t, {msb}, {m}); R.z = z && t == 0; }}")
            else:
                e(f"flags_sub(d, 0, t, {msb}, {m});")
        elif op in ("muls", "mulu"):
            src = opnd(insn.src, "ea1")
            r = insn.dst.reg
            if op == "muls":
                e(f"R.d[{r}] = (uint32_t)((int32_t)(int16_t)R.d[{r}] * (int32_t)(int16_t){src.read()});")
            else:
                e(f"R.d[{r}] = (R.d[{r}] & 0xFFFFu) * ({src.read()} & 0xFFFFu);")
            e(f"flags_logic(R.d[{r}], MSB4);")
        elif op in ("divs", "divu"):
            src = opnd(insn.src, "ea1")
            e(f"op_{op}({insn.dst.reg}, {src.read()});")
        elif op in ("asl", "asr", "lsl", "lsr", "rol", "ror", "roxl", "roxr"):
            dst = opnd(insn.dst, "ea2")
            if insn.src.kind == IMM:
                cnt = str(insn.src.disp)
            else:
                cnt = f"(R.d[{insn.src.reg}] & 63)"
            e(f"t = op_{op}({dst.read()}, {cnt}, {s});")
            e(dst.write("t"))
        elif op in ("btst", "bset", "bclr", "bchg"):
            src = opnd(insn.src, "ea1", 1 if insn.src.kind == IMM else 4)
            dst = opnd(insn.dst, "ea2")
            bits = 31 if s == 4 else 7
            e(f"s = {src.read()} & {bits}; d = {dst.read()}; R.z = ((d >> s) & 1) == 0;")
            if op != "btst":
                expr = {"bset": "d | (1u << s)", "bclr": "d & ~(1u << s)",
                        "bchg": "d ^ (1u << s)"}[op]
                e(dst.write(expr))
        elif op == "scc":
            dst = opnd(insn.dst, "ea2")
            e(dst.write(f"(CC_{insn.cond.upper()} ? 0xFF : 0)"))
        elif op == "exg":
            a = f"R.{'a' if insn.src.kind == AREG else 'd'}[{insn.src.reg}]"
            b = f"R.{'a' if insn.dst.kind == AREG else 'd'}[{insn.dst.reg}]"
            e(f"t = {a}; {a} = {b}; {b} = t;")
        elif op == "movem":
            self.gen_movem(insn)
        elif op == "move_to_ccr":
            src = opnd(insn.src, "ea1")
            e(f"set_ccr({src.read()} & 0x1F);")
        elif op == "move_to_sr":
            src = opnd(insn.src, "ea1")
            e(f"t = {src.read()}; set_ccr(t & 0x1F); R.sr_hi = (t >> 8) & 0xFF;")
        elif op == "move_from_sr":
            dst = opnd(insn.dst, "ea2")
            e(dst.write("(R.sr_hi << 8) | get_ccr()"))
        elif op == "nop":
            pass
        elif op == "bra":
            e(f"{self.poll(insn)}goto {self.label(insn.target)};")
        elif op == "bcc":
            e(f"if (CC_{insn.cond.upper()}) {{ {self.poll(insn)}goto {self.label(insn.target)}; }}")
        elif op == "dbcc":
            r = insn.dst.reg
            e(f"if (!CC_{insn.cond.upper()}) {{ t = (R.d[{r}] - 1) & 0xFFFF; set_d({r}, t, 2); "
              f"if (t != 0xFFFF) {{ {self.poll(insn)}goto {self.label(insn.target)}; }} }}")
        elif op == "bsr":
            target = (self.node, insn.target)
            self.call_known(insn, target, tail=False)
        elif op in ("jsr", "jmp"):
            self.gen_jsr_jmp(insn)
        elif op == "rts":
            e("RTS();")
        elif op in ("rte", "rtr"):
            e("return;")
        else:
            e(f"rt_fatal(\"unsupported instruction {op} at {fname(self.node, insn.pc)}\");")

    def gen_movem(self, insn):
        e, s = self.emit, insn.size
        regs = insn.regmask

        def reg(i):
            return f"R.{'a' if i >= 8 else 'd'}[{i & 7}]"

        rd, wr = ("rw", "ww") if s == 2 else ("rl", "wl")
        if insn.dst is not None:          # registers -> memory
            ea = insn.dst
            if ea.kind == PREDEC:
                e(f"ea2 = R.a[{ea.reg}];")
                for i in sorted(regs, reverse=True):
                    e(f"ea2 -= {s}; {wr}(ea2, {reg(i)});")
                e(f"R.a[{ea.reg}] = ea2;")
            else:
                e(f"ea2 = {self.address(insn, ea)};")
                for i in sorted(regs):
                    e(f"{wr}(ea2, {reg(i)}); ea2 += {s};")
        else:                             # memory -> registers
            ea = insn.src
            if ea.kind == POSTINC:
                e(f"ea1 = R.a[{ea.reg}];")
            else:
                e(f"ea1 = {self.address(insn, ea)};")
            for i in sorted(regs):
                v = "SX16(rw(ea1))" if s == 2 else "rl(ea1)"
                e(f"{reg(i)} = {v}; ea1 += {s};")
            if ea.kind == POSTINC and ea.reg + 8 not in regs:
                e(f"R.a[{ea.reg}] = ea1;")

    # whole function ---------------------------------------------------------
    def generate(self):
        if self.entry in REPLACED:
            return [f"/* {REPLACED[self.entry]} */",
                    f"void {fname(*self.entry)}(void)", "{",
                    f"    FTRACE({h(self.rc.layout.addr(*self.entry))});",
                    "    RTS();", "}", ""]
        an = self.rc.an
        dec = self.rc.decoders[self.node]
        self.body = {(n, o) for n, o in an.bodies[self.entry]
                     if (n, o) in an.insns}
        offs = sorted(o for _, o in self.body)
        insns = {o: dec.decode(o) for o in offs}
        self.jump_targets = {self.entry[1]}
        for i in insns.values():
            if i.op in ("bra", "bcc", "dbcc"):
                self.jump_targets.add(i.target)
            if i.op == "jmp":
                tgt = self.resolve_static(i)
                if tgt and tgt[0] == self.node and tgt in self.body \
                        and tgt not in an.functions:
                    self.jump_targets.add(tgt[1])
        code = []
        self.lines = code
        for o in offs:
            insn = insns[o]
            code.append(f"L_{o:05x}:")
            code.append(f"    /* {o:05x}: {insn} */")
            self.gen(insn)
            # Consecutive instructions fall through naturally; a gap after an
            # instruction that can continue means the analysis missed code.
            if o + insn.length not in insns and \
                    insn.op not in ("bra", "jmp", "rts", "rte", "rtr", "illegal"):
                code.append(f"    rt_fatal(\"fell off {fname(self.node, o)}\");")
        # Entry may not be the lowest address in the body.
        head = [f"void {fname(*self.entry)}(void)", "{",
                "    uint32_t s, d, t, ea1, ea2;",
                "    (void)s; (void)d; (void)t; (void)ea1; (void)ea2;",
                f"    FTRACE({h(self.rc.layout.addr(*self.entry))});"]
        if self.entry in PROLOGUE:
            head.append(f"    {PROLOGUE[self.entry]}")
        if self.entry in GATED:
            head.append(f"    if (!{GATED[self.entry]}) {{ RTS(); }}")
        head.append(f"    goto L_{self.entry[1]:05x};")
        # Drop labels nobody jumps to, so compilers don't warn.
        used = {f"L_{t:05x}:" for t in self.jump_targets}
        body = [ln for ln in code if not (ln.startswith("L_") and ln not in used)]
        return head + body + ["}", ""]


class Recompiler:
    def __init__(self, exe):
        self.prog = Program(exe)
        self.an = Analyzer(self.prog)
        self.an.run()
        nodes = load(Path(exe).read_bytes())
        self.layout = Layout(nodes)
        self.relocs = {hk["num"]: hk["relocs"] for hunks in nodes for hk in hunks}
        self.decoders = {n: Decoder(c) for n, c in self.prog.code.items()}
        self.called = set()
        # jmp (aN) through a pointer table: (node, jmp off) -> [targets]
        self.table_jumps = {}
        self._find_table_jumps()

    def _find_table_jumps(self):
        tables = {-o: t for (n, o), (_, t) in self.an.switches.items() if o < 0}
        for (node, off), (size, mn, op) in self.an.insns.items():
            if node == 0 and mn == "jmp" and op.startswith("(a"):
                # find nearest preceding lea of a known table
                for back in range(2, 40, 2):
                    prev = self.an.insns.get((0, off - back))
                    if prev and prev[1] == "lea.l" and prev[2].endswith(".l, " + op[1:3]):
                        table = int(prev[2].split(".l")[0][1:], 16)
                        if table in tables:
                            self.table_jumps[(0, off)] = tables[table]
                        break

    def write(self, outdir):
        outdir = Path(outdir)
        outdir.mkdir(parents=True, exist_ok=True)
        funcs = sorted(self.an.functions)
        by_node = defaultdict(list)
        for f in funcs:
            by_node[f[0]].append(f)

        decls = [f"void {fname(*f)}(void);" for f in funcs]
        (outdir / "functions.h").write_text(
            "/* Generated by tools/recomp.py. */\n#ifndef FUNCTIONS_H\n#define FUNCTIONS_H\n\n"
            + "\n".join(decls) + "\n\n#endif\n")

        layout = ["/* Generated by tools/recomp.py: where the loader puts each hunk. */",
                  "#ifndef LAYOUT_H", "#define LAYOUT_H", "",
                  f"#define NUM_HUNKS {len(self.layout.base)}",
                  f"#define LOAD_END {h(self.layout.end)}",
                  f"#define ENTRY_FUNC {fname(0, 0)}",
                  f"#define A4_VALUE {h(self.layout.base[1] + 0x7FFE)}",
                  "static const uint32_t hunk_base[NUM_HUNKS] = {",
                  "    " + ", ".join(h(self.layout.base[i]) for i in sorted(self.layout.base)),
                  "};", "", "#endif", ""]
        (outdir / "layout.h").write_text("\n".join(layout))

        stats = {}
        for node, flist in sorted(by_node.items()):
            out = ["/* Generated by tools/recomp.py from overlay node "
                   f"{node} of DuckTales. Do not edit. */",
                   '#include "m68k.h"', '#include "functions.h"',
                   "void call_indirect_ret(uint32_t addr, uint32_t ret);",
                   "void call_indirect_tail(uint32_t addr);", ""]
            for f in flist:
                out.extend(FunctionGen(self, f[0], f).generate())
            (outdir / f"node{node:02d}.c").write_text("\n".join(out))
            stats[node] = len(out)

        table = ["/* Generated by tools/recomp.py: address -> function, sorted. */",
                 '#include "m68k.h"', '#include "functions.h"', '#include "dispatch.h"', "",
                 "const FuncEntry func_table[] = {"]
        entries = sorted((self.layout.addr(*f), fname(*f)) for f in funcs)
        table += [f"    {{{h(a)}, {n}}}," for a, n in entries]
        table += ["};", f"const int func_table_len = {len(entries)};", ""]
        (outdir / "dispatch.c").write_text("\n".join(table))
        return funcs, stats


def main():
    rc = Recompiler(sys.argv[1])
    funcs, stats = rc.write(sys.argv[2])
    print(f"{len(funcs)} functions, C lines per node: {stats}")
    print("hunk bases:", {k: hex(v) for k, v in rc.layout.base.items()})


if __name__ == "__main__":
    main()
