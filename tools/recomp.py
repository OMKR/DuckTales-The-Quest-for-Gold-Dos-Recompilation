"""Static recompiler: DUCKTALE.EXE (8086) -> portable C.

Usage: recomp.py <unpacked DUCKTALE.EXE> <output dir>

Every function found by analyze.py becomes `void F<img>_<cs>_<ip>(void)`
working on the global CPU state R and the emulated 1 MB memory. The program
is loaded at segment LOAD by the runtime; relocated segment values are
emitted with LOAD added. Overlay calls (INT 3Fh) load the overlay image
like the original manager. Driver code (IT.DRV) runs with a dynamic CS.
"""
import struct
import sys
from collections import defaultdict
from pathlib import Path

from capstone import x86

sys.path.insert(0, str(Path(__file__).parent))
from analyze import HOOKS, RETURN_HOOKS, JCC, PATCHED_JUMPS, PATCHED_OPCODES, Analyzer  # noqa: E402
from dosexe import DRIVER_BASE, Program  # noqa: E402

LOAD = 0x0300          # program load segment (PSP at LOAD - 10h)
COND = {"jo": "C_JO", "jno": "C_JNO", "jb": "C_JB", "jae": "C_JAE", "je": "C_JE",
        "jne": "C_JNE", "jbe": "C_JBE", "ja": "C_JA", "js": "C_JS", "jns": "C_JNS",
        "jp": "C_JP", "jnp": "C_JNP", "jl": "C_JL", "jge": "C_JGE", "jle": "C_JLE",
        "jg": "C_JG"}
REG16 = {"ax", "bx", "cx", "dx", "si", "di", "bp", "sp"}
REG8 = {"al": ("ax", "LO"), "ah": ("ax", "HI"), "bl": ("bx", "LO"), "bh": ("bx", "HI"),
        "cl": ("cx", "LO"), "ch": ("cx", "HI"), "dl": ("dx", "LO"), "dh": ("dx", "HI")}
SREG = {"es", "cs", "ss", "ds"}
SHIFTS = {"rol": "SH_ROL", "ror": "SH_ROR", "rcl": "SH_RCL", "rcr": "SH_RCR",
          "shl": "SH_SHL", "sal": "SH_SAL", "shr": "SH_SHR", "sar": "SH_SAR"}
SEG_PREFIX = {0x26: "es", 0x2E: "cs", 0x36: "ss", 0x3E: "ds"}


def h(v):
    return f"0x{v & 0xFFFFFFFF:X}u"


def fname(f):
    return f"F{f[0]:03d}_{f[1]:04X}_{f[2]:04X}"


class Unsupported(Exception):
    pass


class FuncGen:
    def __init__(self, rc, func):
        self.rc = rc
        self.an = rc.an
        self.func = func
        self.img_no, self.cs, self.entry = func
        self.driver = self.img_no >= DRIVER_BASE
        self.csv = "R.cs" if self.driver else h(self.cs + LOAD)
        self.lines = []
        self.tmp = 0

    # ---- helpers ----------------------------------------------------------------
    def emit(self, s):
        self.lines.append("    " + s)

    def ip_of(self, lin):
        return (lin - self.cs * 16) & 0xFFFF

    def reg_name(self, insn, reg):
        return insn.cs.reg_name(reg)

    def relocated(self, insn):
        """Offsets inside the instruction holding relocated segment words."""
        img = self.an.image_for(self.img_no, insn.address)
        if img is None:
            return set()
        return {a - insn.address for a in img.relocs
                if insn.address <= a < insn.address + insn.size}

    def seg_override(self, insn):
        for b in bytes(insn.bytes)[:4]:
            if b in SEG_PREFIX:
                return SEG_PREFIX[b]
        return None

    def sreg(self, name):
        return self.csv if name == "cs" else f"R.{name}"

    def mem_addr(self, insn, op):
        m = op.mem
        parts = []
        base = self.reg_name(insn, m.base) if m.base else None
        index = self.reg_name(insn, m.index) if m.index else None
        if base:
            parts.append(f"R.{base}")
        if index:
            parts.append(f"R.{index}")
        if m.disp or not parts:
            parts.append(self.patched_field(insn, "disp") or h(m.disp & 0xFFFF))
        off = f"(uint16_t)({' + '.join(parts)})"
        if m.segment:
            seg = self.reg_name(insn, m.segment)
        elif getattr(insn, "seg", None):
            seg = insn.seg
        else:
            seg = "ss" if base == "bp" else "ds"
        return self.sreg(seg), off

    def operand(self, insn, op, size=None):
        """(read expression, write function, pre statements) for an operand."""
        size = size or op.size
        if op.type == x86.X86_OP_REG:
            r = self.reg_name(insn, op.reg)
            if r in REG16:
                return f"R.{r}", (lambda v, r=r: f"R.{r} = (uint16_t)({v});"), []
            if r in REG8:
                full, half = REG8[r]
                return f"{half}(R.{full})", (lambda v, full=full, half=half:
                                             f"SET_{half}(R.{full}, {v});"), []
            if r in SREG:
                if r == "cs":
                    return self.csv, None, []
                return f"R.{r}", (lambda v, r=r: f"R.{r} = (uint16_t)({v});"), []
            raise Unsupported(f"register {r}")
        if op.type == x86.X86_OP_IMM:
            live = self.patched_field(insn, "imm", size)
            if live:
                return live, None, []
            v = op.imm & (0xFF if size == 1 else 0xFFFF)
            if self.relocated(insn) and size == 2:
                v = (v + LOAD) & 0xFFFF
            return h(v), None, []
        if op.type == x86.X86_OP_MEM:
            seg, off = self.mem_addr(insn, op)
            self.tmp += 1
            s, o = f"s{self.tmp}", f"o{self.tmp}"
            pre = [f"uint32_t {s} = {seg}, {o} = {off};"]
            rd = "rb" if size == 1 else "rw"
            wr = "wb" if size == 1 else "ww"
            return f"{rd}({s}, {o})", (lambda v, s=s, o=o, wr=wr: f"{wr}({s}, {o}, {v});"), pre
        raise Unsupported("operand type")

    def patched_field(self, insn, kind, size=2):
        """Run-time read of an immediate or displacement the program
        rewrites (self-modifying code), else None."""
        c = insn.cs
        if c is None or insn.fpu:
            return None
        off = c.imm_offset if kind == "imm" else c.disp_offset
        n = c.imm_size if kind == "imm" else c.disp_size
        if not n:
            return None
        lin = insn.address + off
        if not any((self.img_no, lin + k) in self.rc.patched for k in range(n)):
            return None
        at = h(self.ip_of(lin))
        if n == 2:
            return f"rw({self.csv}, {at})"
        if size == 1:
            return f"rb({self.csv}, {at})"
        return f"(uint16_t)(int8_t)rb({self.csv}, {at})"

    def label(self, ip):
        self.targets.add(ip)
        return f"L_{ip:04X}"

    def poll(self, insn, target_ip):
        return f"POLL({h(insn.address)}); " if target_ip <= self.ip_of(insn.address) else ""

    def call_known(self, callee, push_cs, ret_ip, tail):
        name = fname(callee)
        self.rc.called.add(callee)
        if tail:
            self.emit(f"{name}(); return;")
            return
        if push_cs:
            self.emit(f"push16({self.csv});")
        self.emit(f"push16({h(ret_ip)}); RS_PUSH({self.csv}, {h(ret_ip)}); {name}(); RS_POP();")

    def find_function(self, img_no, cs, ip):
        f = (img_no, cs, ip)
        return f if f in self.an.functions else None

    # ---- instructions -------------------------------------------------------------
    def gen(self, insn):
        e = self.emit
        lin = insn.address
        ip = self.ip_of(lin)
        nxt = (ip + insn.size) & 0xFFFF
        if insn.fpu:
            return self.gen_fpu(insn)
        mn = insn.mnemonic
        ops = insn.operands
        b0 = bytes(insn.bytes)[0]
        prefix_rep = ""
        if " " in mn and mn.split()[0] in ("rep", "repe", "repne"):
            prefix_rep, mn = mn.split()[0], mn.split()[1]
        bits = (ops[0].size * 8) if ops else 16

        if (self.img_no, lin) in self.rc.int3f_sites:
            n, off = self.rc.int3f_sites[(self.img_no, lin)]
            tovl, tlin = self.rc.p.int3f_target(n, off)
            callee = (tovl, self.rc.p.int3f_seg[n], off)
            self.rc.called.add(callee)
            e(f"{{ int prev_ = ovl_load({tovl}); push16({self.csv}); push16({h(nxt + 3)}); "
              f"RS_PUSH({self.csv}, {h(nxt + 3)}); {fname(callee)}(); RS_POP(); ovl_restore(prev_); }}")
            return nxt + 3

        if mn == "mov":
            src, _, pre1 = self.operand(insn, ops[1])
            dst_r, dst_w, pre2 = self.operand(insn, ops[0])
            for p in pre1 + pre2:
                e(p)
            e(dst_w(src))
        elif mn in ("push",):
            v, _, pre = self.operand(insn, ops[0])
            for p in pre:
                e(p)
            if v == "R.sp":
                e("{ uint32_t t_ = R.sp - 2; push16(t_); }")
            else:
                e(f"push16({v});")
        elif mn == "pop":
            _, w, pre = self.operand(insn, ops[0])
            e("{ uint32_t t_ = pop16();")
            for p in pre:
                e(p)
            e(w("t_") + " }")
        elif mn == "xchg":
            a, wa, pa = self.operand(insn, ops[0])
            b, wb_, pb = self.operand(insn, ops[1])
            for p in pa + pb:
                e(p)
            e(f"{{ uint32_t t_ = {a}; {wa(b)} {wb_('t_')} }}")
        elif mn == "lea":
            _, off = self.mem_addr(insn, ops[1])
            _, w, _ = self.operand(insn, ops[0])
            e(w(off))
        elif mn in ("lds", "les"):
            seg, off = self.mem_addr(insn, ops[1])
            _, w, _ = self.operand(insn, ops[0])
            e(f"{{ uint32_t s_ = {seg}, o_ = {off}; uint32_t v_ = rw(s_, o_); "
              f"R.{mn[1:]} = (uint16_t)rw(s_, (o_ + 2) & 0xFFFF); {w('v_')} }}")
        elif b0 == 0x98:      # cbw
            e("R.ax = (uint16_t)(int16_t)(int8_t)LO(R.ax);")
        elif b0 == 0x99:      # cwd
            e("R.dx = (R.ax & 0x8000) ? 0xFFFF : 0;")
        elif mn in ("add", "adc", "sub", "sbb", "cmp"):
            a, wa, pa = self.operand(insn, ops[0])
            b, _, pb = self.operand(insn, ops[1], ops[0].size)
            for p in pa + pb:
                e(p)
            fn = "f_add" if mn in ("add", "adc") else "f_sub"
            carry = "R.cf" if mn in ("adc", "sbb") else "0"
            if mn == "cmp":
                e(f"{fn}({a}, {b}, 0, {bits});")
            else:
                e(f"{{ uint32_t r_ = {fn}({a}, {b}, {carry}, {bits}); {wa('r_')} }}")
        elif mn in ("and", "or", "xor", "test"):
            a, wa, pa = self.operand(insn, ops[0])
            b, _, pb = self.operand(insn, ops[1], ops[0].size)
            for p in pa + pb:
                e(p)
            sym = {"and": "&", "test": "&", "or": "|", "xor": "^"}[mn]
            if mn == "test":
                e(f"f_logic({a} {sym} {b}, {bits});")
            else:
                e(f"{{ uint32_t r_ = f_logic({a} {sym} {b}, {bits}); {wa('r_')} }}")
        elif mn in ("inc", "dec"):
            a, wa, pa = self.operand(insn, ops[0])
            for p in pa:
                e(p)
            e(f"{{ uint32_t r_ = f_{mn}({a}, {bits}); {wa('r_')} }}")
        elif mn == "neg":
            a, wa, pa = self.operand(insn, ops[0])
            for p in pa:
                e(p)
            e(f"{{ uint32_t r_ = f_sub(0, {a}, 0, {bits}); {wa('r_')} }}")
        elif mn == "not":
            a, wa, pa = self.operand(insn, ops[0])
            for p in pa:
                e(p)
            e(wa(f"~{a} & {'0xFF' if bits == 8 else '0xFFFF'}"))
        elif mn in SHIFTS:
            a, wa, pa = self.operand(insn, ops[0])
            cnt = "1"
            if len(ops) > 1:
                cnt, _, _ = self.operand(insn, ops[1], 1)
            for p in pa:
                e(p)
            e(f"{{ uint32_t r_ = op_shift({SHIFTS[mn]}, {a}, {cnt}, {bits}); {wa('r_')} }}")
        elif mn in ("mul", "imul", "div", "idiv"):
            if mn == "imul" and len(ops) == 3:
                _, wd, _ = self.operand(insn, ops[0])
                a, _, pa = self.operand(insn, ops[1])
                b, _, _ = self.operand(insn, ops[2], 2)
                for p in pa:
                    e(p)
                e(wd(f"op_imul3({a}, {b})"))
            else:
                a, _, pa = self.operand(insn, ops[0])
                for p in pa:
                    e(p)
                e(f"op_{mn}{bits}({a});")
        elif mn == "xlatb":
            seg = self.sreg(self.seg_override(insn) or "ds")
            e(f"SET_LO(R.ax, rb({seg}, (uint16_t)(R.bx + LO(R.ax))));")
        elif mn in ("movsb", "movsw", "cmpsb", "cmpsw", "stosb", "stosw", "lodsb", "lodsw",
                    "scasb", "scasw"):
            size = 1 if mn.endswith("b") else 2
            seg = self.sreg(self.seg_override(insn) or "ds")
            rep = {"": 0, "rep": 1, "repe": 1, "repne": 2}[prefix_rep]
            if mn[:4] in ("movs", "stos", "lods") and rep == 2:
                rep = 1
            e(f"str_{mn[:4]}({size}, {seg}, {rep});")
        elif mn in ("clc", "stc", "cmc", "cld", "std", "cli", "sti"):
            e({"clc": "R.cf = 0;", "stc": "R.cf = 1;", "cmc": "R.cf ^= 1;", "cld": "R.df = 0;",
               "std": "R.df = 1;", "cli": "R.ifl = 0;", "sti": "R.ifl = 1;"}[mn])
        elif mn == "pushf":
            e("push16(get_flags());")
        elif mn == "popf":
            e("set_flags(pop16());")
        elif mn == "lahf":
            e("SET_HI(R.ax, get_flags() & 0xFF);")
        elif mn == "sahf":
            e("set_flags((get_flags() & 0xFF00) | HI(R.ax));")
        elif mn in ("nop", "wait"):
            pass
        elif mn == "in":
            port = "R.dx" if ops[1].type == x86.X86_OP_REG else h(ops[1].imm & 0xFF)
            size = ops[0].size
            _, w, _ = self.operand(insn, ops[0])
            e(w(f"port_in({port}, {size})"))
        elif mn == "out":
            port = "R.dx" if ops[0].type == x86.X86_OP_REG else h(ops[0].imm & 0xFF)
            v, _, _ = self.operand(insn, ops[1])
            e(f"port_out({port}, {v}, {ops[1].size});")
        elif mn == "int":
            n = ops[0].imm & 0xFF
            e(f"do_int({h(n)}, {self.csv}, {h(nxt)});")
        elif mn in COND:
            t = self.ip_of(ops[0].imm)
            e(f"if ({COND[mn]}) {{ {self.poll(insn, t)}goto {self.label(t)}; }}")
        elif mn in ("loop", "loope", "loopne", "jcxz"):
            t = self.ip_of(ops[0].imm)
            if mn == "jcxz":
                e(f"if (R.cx == 0) goto {self.label(t)};")
            else:
                extra = {"loop": "", "loope": " && R.zf", "loopne": " && !R.zf"}[mn]
                e(f"if (--R.cx != 0{extra}) {{ {self.poll(insn, t)}goto {self.label(t)}; }}")
        elif mn in ("jmp", "ljmp"):
            return self.gen_jmp(insn, ip, nxt)
        elif mn in ("call", "lcall"):
            self.gen_call(insn, nxt)
        elif mn == "ret":
            n = ops[0].imm if ops else 0
            e(f"pop16(); R.sp += {n}; return;")
        elif mn == "retf":
            n = ops[0].imm if ops else 0
            e(f"pop16(); pop16(); R.sp += {n}; return;")
        elif mn == "iret":
            e("pop16(); pop16(); set_flags(pop16()); return;")
        elif mn == "hlt":
            e('rt_fatal("hlt");')
        else:
            raise Unsupported(mn)
        return nxt

    def gen_jmp(self, insn, ip, nxt):
        e = self.emit
        b0 = bytes(insn.bytes)[0]
        key = (self.img_no, insn.address)
        if key in PATCHED_JUMPS:
            targets = PATCHED_JUMPS[key]
            e(f"switch ((uint16_t)({h(nxt)} + rw({self.csv}, {h(ip + 1)}))) {{")
            for t in targets:
                e(f"case {h(t)}: goto {self.label(t)};")
            e(f'default: rt_fatal("patched jump %04X at {fname(self.func)}+{ip:04X}", '
              f'(unsigned)rw({self.csv}, {h(ip + 1)})); }}')
            return nxt
        if b0 in (0xE9, 0xEB):
            t = self.ip_of(insn.operands[0].imm)
            if (self.img_no, insn.address) in self.an.insns and \
                    (self.img_no, self.cs * 16 + t) in self.body:
                e(f"{self.poll(insn, t)}goto {self.label(t)};")
            else:
                callee = self.find_function(self.img_no, self.cs, t) or \
                    self.find_function(0, self.cs, t)
                if callee:
                    self.call_known(callee, False, 0, tail=True)
                else:
                    e(f"jump_near({self.csv}, {h(t)}); return;")
            return nxt
        if b0 == 0xEA:
            off, seg = struct.unpack_from("<HH", bytes(insn.bytes), 1)
            if 3 in self.relocated(insn):
                callee = (self.an.owner_ovl(self.img_no, seg), seg, off)
                if callee in self.an.functions:
                    self.call_known(callee, False, 0, tail=True)
                    return nxt
                seg += LOAD
            e(f"jump_far({h(seg)}, {h(off)}); return;")
            return nxt
        op = insn.operands[0]
        if key in self.an.switches:
            v, _, pre = self.operand(insn, op)
            for p in pre:
                e(p)
            e(f"switch ({v}) {{")
            for t in sorted(set(self.an.switches[key])):
                e(f"case {h(t)}: goto {self.label(t)};")
            e(f"default: jump_near({self.csv}, {v}); return; }}")
            return nxt
        if insn.mnemonic == "ljmp":
            seg, off = self.mem_addr(insn, op)
            e(f"{{ uint32_t s_ = {seg}, o_ = {off}; jump_far(rw(s_, (o_ + 2) & 0xFFFF), rw(s_, o_)); }} return;")
            return nxt
        v, _, pre = self.operand(insn, op)
        for p in pre:
            e(p)
        e(f"jump_near({self.csv}, {v}); return;")
        return nxt

    def gen_call(self, insn, nxt):
        e = self.emit
        b0 = bytes(insn.bytes)[0]
        op = insn.operands[0]
        if b0 == 0xE8:
            t = self.ip_of(op.imm)
            callee = self.find_function(self.an.owner_ovl(self.img_no, self.cs), self.cs, t) \
                or self.find_function(self.img_no, self.cs, t)
            if callee:
                self.call_known(callee, False, nxt, tail=False)
            else:
                e(f"push16({h(nxt)}); RS_PUSH({self.csv}, {h(nxt)}); dispatch_near({self.csv}, {h(t)}); RS_POP();")
            return
        if b0 == 0x9A:
            off, seg = struct.unpack_from("<HH", bytes(insn.bytes), 1)
            if 3 in self.relocated(insn):
                callee = (self.an.owner_ovl(self.img_no, seg), seg, off)
                if callee in self.an.functions:
                    self.call_known(callee, True, nxt, tail=False)
                    return
                seg += LOAD
            e(f"push16({self.csv}); push16({h(nxt)}); RS_PUSH({self.csv}, {h(nxt)}); dispatch_far({h(seg)}, {h(off)}); RS_POP();")
            return
        if insn.mnemonic == "lcall":
            seg, off = self.mem_addr(insn, op)
            e(f"{{ uint32_t s_ = {seg}, o_ = {off}; uint32_t ts_ = rw(s_, (o_ + 2) & 0xFFFF), "
              f"to_ = rw(s_, o_); push16({self.csv}); push16({h(nxt)}); RS_PUSH({self.csv}, {h(nxt)}); "
              f"dispatch_far(ts_, to_); RS_POP(); }}")
            return
        v, _, pre = self.operand(insn, op)
        for p in pre:
            e(p)
        e(f"{{ uint32_t t_ = {v}; push16({h(nxt)}); RS_PUSH({self.csv}, {h(nxt)}); "
          f"dispatch_near({self.csv}, t_); RS_POP(); }}")

    # ---- x87 ------------------------------------------------------------------------
    def gen_fpu(self, insn):
        e = self.emit
        nxt = (self.ip_of(insn.address) + insn.size) & 0xFFFF
        mn = insn.mnemonic
        ops = insn.operands
        if mn == "fwait":
            return nxt
        mem = [o for o in ops if o.type == x86.X86_OP_MEM]
        regs = [int(self.reg_name(insn, o.reg)[3]) if self.reg_name(insn, o.reg) != "st0"
                and self.reg_name(insn, o.reg).startswith("st(") else 0
                for o in ops if o.type == x86.X86_OP_REG]
        if mem:
            seg, off = self.mem_addr(insn, mem[0])
            e(f"fpu_mem(FPU_{mn.upper()}, {mem[0].size}, {seg}, {off});")
        else:
            names = [self.reg_name(insn, o.reg) for o in ops if o.type == x86.X86_OP_REG]
            idx = [0 if n in ("st0", "st(0)") else int(n.strip("st()")) for n in names]
            if mn in ("fnstsw",):
                e("R.ax = (uint16_t)fpu_status();")
            else:
                a = idx[0] if idx else -1
                b = idx[1] if len(idx) > 1 else -1
                e(f"fpu_reg(FPU_{mn.upper()}, {a}, {b});")
        self.rc.fpu_ops.add(mn.upper())
        return nxt

    # ---- whole function ---------------------------------------------------------
    def generate(self):
        an = self.an
        self.body = {k for k in an.bodies.get(self.func, ()) if k in an.insns}
        lins = sorted(lin for _, lin in self.body)
        self.targets = {self.entry}
        code = []
        self.lines = code
        insns = {lin: an.insns[(self.img_no, lin)] for lin in lins}
        for lin in lins:
            insn = insns[lin]
            ip = self.ip_of(lin)
            code.append(f"L_{ip:04X}:")
            text = f"{insn.mnemonic} {insn.op_str}".replace("*/", "* /")
            code.append(f"    /* {ip:04X}: {text} */")
            if (self.img_no, lin) in HOOKS:
                code.append(f"    {HOOKS[(self.img_no, lin)]}();")
            if (self.img_no, lin) in RETURN_HOOKS:
                code.append(f"    if ({RETURN_HOOKS[(self.img_no, lin)]}()) return;")
            try:
                vs = an.variants(self.img_no, lin)
                if vs:
                    # rewritten opcode: pick the variant by the first byte
                    # in memory in which the variants differ
                    at = next(i for i in range(len(vs[0].bytes))
                              if len({bytes(v.bytes)[i:i + 1] for v in vs}) > 1)
                    for k, v in enumerate(vs):
                        cond = f"rb({self.csv}, {h((ip + at) & 0xFFFF)}) == {h(v.bytes[at])}"
                        code.append(f"    {'if' if k == 0 else '} else if'} ({cond}) {{")
                        self.gen(v)
                    code.append(f'    }} else rt_fatal("bad patched opcode at {fname(self.func)}+{ip:04X}");')
                    nxt_ip = (ip + insn.size) & 0xFFFF
                else:
                    nxt_ip = self.gen(insn)
            except Unsupported as ex:
                self.rc.unsupported[str(ex)] += 1
                self.emit(f'rt_fatal("unsupported {ex} at {fname(self.func)}+{ip:04X}");')
                nxt_ip = (ip + insn.size) & 0xFFFF
            ends = insn.mnemonic in ("ret", "retf", "iret", "jmp", "ljmp", "hlt")
            if not ends and (self.img_no, self.cs * 16 + nxt_ip) not in self.body:
                code.append(f'    rt_fatal("fell off {fname(self.func)} at {ip:04X}");')
        if not lins:
            code.append(f"L_{self.entry:04X}:")
            code.append(f'    rt_fatal("no code at {fname(self.func)}");')
        used = {f"L_{t:04X}:" for t in self.targets}
        body = [ln for ln in code if not (ln.startswith("L_") and ln not in used)]
        head = [f"void {fname(self.func)}(void)", "{",
                f"    FTRACE({h((min(self.img_no, 15) if self.img_no < DRIVER_BASE else 15) << 28 | (self.cs & 0xFFF) << 16 | self.entry)});",
                f"    goto L_{self.entry:04X};"]
        return head + body + ["}", ""]


class Recompiler:
    def __init__(self, exe):
        self.p = Program(exe)
        self.an = Analyzer(self.p)
        self.an.run()
        self.int3f_sites = self.an.int3f
        self.called = set()
        self.fpu_ops = set()
        self.unsupported = defaultdict(int)
        self.patched = self.an.patched_bytes()

    def write(self, outdir):
        out = Path(outdir)
        out.mkdir(parents=True, exist_ok=True)
        funcs = sorted(self.an.functions)
        (out / "functions86.h").write_text(
            "/* Generated by tools/recomp.py. */\n#ifndef FUNCTIONS86_H\n#define FUNCTIONS86_H\n"
            + "".join(f"void {fname(f)}(void);\n" for f in funcs)
            + "/* runtime hooks (runtime/hooks86.c) */\n"
            + "".join(f"void {n}(void);\n" for n in sorted(set(HOOKS.values())))
            + "".join(f"int {n}(void);\n" for n in sorted(set(RETURN_HOOKS.values())))
            + "#endif\n")
        groups = defaultdict(list)
        for f in funcs:
            groups["drv" if f[0] >= DRIVER_BASE else f"img{f[0]:02d}"].append(f)
        for g, fl in groups.items():
            src = [f"/* Generated by tools/recomp.py ({g}). Do not edit. */",
                   '#include "cpu86.h"', '#include "functions86.h"', '#include "fpu86.h"',
                   "int ovl_load(int n);", "void ovl_restore(int n);",
                   "void str_movs(int, uint32_t, int); void str_cmps(int, uint32_t, int);",
                   "void str_stos(int, uint32_t, int); void str_lods(int, uint32_t, int);",
                   "void str_scas(int, uint32_t, int);", ""]
            for f in fl:
                src += FuncGen(self, f).generate()
            (out / f"{g}.c").write_text("\n".join(src))
        # dispatch table: (image, cs, ip) sorted
        tab = ['/* Generated by tools/recomp.py. */', '#include "cpu86.h"', '#include "functions86.h"',
               '#include "dispatch86.h"', "", "const Func86 func86_table[] = {"]
        tab += [f"    {{{f[0]}, {h(f[1])}, {h(f[2])}, {fname(f)}}}," for f in funcs]
        tab += ["};", f"const int func86_count = {len(funcs)};", ""]
        (out / "dispatch86.c").write_text("\n".join(tab))
        # driver fingerprints: FNV-1a of the first 16 bytes (identifies a
        # driver the game loaded at some segment; no game bytes stored)
        fp = []
        for d, img in sorted(self.p.drivers.items()):
            v = 0x811C9DC5
            for b in img.data[:16]:   # entry table; the game patches later bytes
                v = ((v ^ b) * 0x01000193) & 0xFFFFFFFF
            fp.append(f"    {{{d}, {h(v)}}},")
        (out / "layout86.h").write_text("\n".join([
            "/* Generated by tools/recomp.py. */", "#ifndef LAYOUT86_H", "#define LAYOUT86_H",
            f"#define LOAD_SEG {h(LOAD)}", f"#define ENTRY_CS {h(self.p.entry[0] + LOAD)}",
            f"#define ENTRY_IP {h(self.p.entry[1])}", f"#define STACK_SS {h(self.p.stack[0] + LOAD)}",
            f"#define STACK_SP {h(self.p.stack[1])}", f"#define DRIVER_BASE {DRIVER_BASE}",
            "static const struct { int img; uint32_t hash; } driver_fp[] = {", *fp, "};",
            "#endif", ""]))
        return funcs


def main():
    rc = Recompiler(sys.argv[1])
    funcs = rc.write(sys.argv[2])
    print(f"{len(funcs)} functions")
    print("unsupported:", dict(rc.unsupported))
    print("fpu ops:", sorted(rc.fpu_ops))


if __name__ == "__main__":
    main()
