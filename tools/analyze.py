"""Recursive-descent analysis of DUCKTALE.EXE (8086, Microsoft C 5.x).

A function is identified by (image, cs, ip): image 0 = root, 1..10 = the
overlay that must be loaded for the code to exist, DRIVER_BASE+k = IT.DRV
module k (cs = 0, the segment is chosen at run time). Code is decoded with
Capstone (16-bit mode) at image-linear addresses (cs * 16 + ip).

Control flow understood:
  near call/jmp/jcc/loop/jcxz           within the same CS
  far call/jmp with a relocated segment  to that segment
  INT 3Fh n off (3 inline bytes)         overlay call through the tables
  jmp cs:[reg+table]                     switch, table of near offsets in CS
  MSC 8087 emulator calls INT 34h-3Dh    decoded as the x87 instruction
Entry points: the program entry, the driver entry tables, interrupt
handlers installed with INT 21h AH=25h, and far pointers in the images (a
relocated segment word preceded by an offset word) whose target is code.
"""
import json
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

import capstone
from capstone import x86

sys.path.insert(0, str(Path(__file__).parent))
from dosexe import DGROUP, DRIVER_BASE, OVL_SEG, Program  # noqa: E402

JCC = {"jo", "jno", "jb", "jae", "je", "jne", "jbe", "ja", "js", "jns", "jp",
       "jnp", "jl", "jge", "jle", "jg", "jcxz", "loop", "loope", "loopne"}

# Jumps whose displacement is rewritten at run time: (image, linear) ->
# possible target offsets in CS. 040E:10EF `jmp rel16` in the LZW decoder is
# patched by `mov cs:[10F0h], ax` (040E:1124) with one of three handlers.
PATCHED_JUMPS = {(0, 0x40E0 + 0x10EF): [0x12F0, 0x122B, 0x112B, 0x1263]}

# Runtime calls inserted before an instruction: (image, linear) -> C
# function (runtime/settings86.c).
HOOKS = {
    # driver choice done, sound driver number DS:204B about to be used
    (0, 0x40E0 + 0x0909): "hook_sound_choice",
    (2, 0xCE1 * 16 + 0x26F): "hook_code_wheel_result",
}
# Hooks that may replace the rest of the function: `if (hook()) return;`
RETURN_HOOKS = {
    # overlay 2: the code wheel check (returns AX = 1 when answered)
    (2, OVL_SEG * 16 + 0x07AD): "hook_code_wheel",
    # "Do you have a joystick? (Y/N)": answered from the settings
    (0, 0x40E0 + 0x0AFA): "hook_joystick_question",
    # joystick calibration (move to the corners and press fire)
    (0, 0x40E0 + 0x0B25): "hook_joystick_calibration",
    # keyboard flush: keeps a key event that has only just arrived
    (0, 0x40E0 + 0x19A5): "hook_keyboard_flush",
}
HOOKS[(0, 0x40E0 + 0x199F)] = "hook_picture_loaded"
# the key reader returns (the keyboard flush keeps only unread keys)
HOOKS[(0, 0x40E0 + 0x1A35)] = "hook_key_read"
# Amiga sound voices (amigasnd.c): the plane has landed, the scales' result
HOOKS[(4, OVL_SEG * 16 + 0x0F27)] = "hook_plane_landed"
HOOKS[(3, OVL_SEG * 16 + 0x0A94)] = "hook_scales_result"
# jungle: the nephew who plays (settings.jungle_order)
HOOKS[(8, OVL_SEG * 16 + 0x1301)] = "hook_jungle_nephew"
HOOKS[(8, 0xE4C * 16 + 0x0236)] = "hook_jungle_hat_before"
HOOKS[(8, 0xE4C * 16 + 0x023E)] = "hook_jungle_hat_after"   # loader exit (amiga86.c)

# Opcodes rewritten at run time: (image, linear) -> the byte strings stored
# there. The EGA driver's line drawer (IT.DRV module 1, 0590h) selects
# inc/dec ax and turns a jmp short into or ax, ax.
PATCHED_OPCODES = {
    (DRIVER_BASE + 1, 0x650): [bytes([0x40]), bytes([0x48])],
    (DRIVER_BASE + 1, 0x659): [bytes([0xEB, 0x1A]), bytes([0x0B, 0xC0])],
    # overlay 2, 0CE1:026F: this copy of the game tests BP instead of AX
    # after the code wheel check, so every answer passes; the original
    # or ax, ax is put back when copy protection is on (hook_code_wheel_result)
    (2, 0xCE1 * 16 + 0x26F): [bytes([0x09, 0xED]), bytes([0x09, 0xC0])],
}

# Segments that are not program code: DGROUP, the 8087 emulator ("MSEM87",
# its x87 instructions run natively) and the emulator's data.
NOT_CODE_SEGS = {DGROUP, 0x11F7, 0x09C4}


def memory_slot(b):
    """(r/m, displacement) of a memory operand without segment prefix, as
    a key for matching stores and indirect calls through the same field;
    None for register operands."""
    if len(b) < 2:
        return None
    mod, rm = b[1] >> 6, b[1] & 7
    if mod == 0 and rm == 6 and len(b) >= 4:
        return (8, struct.unpack_from("<H", b, 2)[0])
    if mod == 1 and len(b) >= 3:
        return (rm, b[2])
    if mod == 2 and len(b) >= 4:
        return (rm, struct.unpack_from("<H", b, 2)[0])
    return None


class Insn:
    """A decoded instruction. Wraps Capstone; FP emulator calls are decoded
    as the x87 instruction they stand for (fpu=True, size = emulated size)."""

    def __init__(self, cs_insn, address, size, fpu=False, seg=None):
        self.cs = cs_insn
        self.address = address
        self.size = size
        self.fpu = fpu
        self.seg = seg                    # segment override of an INT 3Ch form
        self.mnemonic = cs_insn.mnemonic if cs_insn else "fwait"
        self.op_str = cs_insn.op_str if cs_insn else ""
        self.operands = cs_insn.operands if cs_insn else []
        self.bytes = cs_insn.bytes if cs_insn else bytes([0xCD, 0x3D])


# Microsoft C 8087 emulator: INT 34h-3Bh stand for the ESC opcodes D8h-DFh
# (the next bytes are the rest of the x87 instruction), INT 3Ch for an ESC
# with a segment override (bits 7-6 of the next byte: 2 = CS, the only one
# in this program), INT 3Dh for FWAIT.
FPU_SEG = {0: "es", 1: "ss", 2: "cs", 3: "ds"}


def decode_bytes(md, raw, lin):
    if len(raw) >= 2 and raw[0] == 0xCD and 0x34 <= raw[1] <= 0x3D:
        n = raw[1]
        if n == 0x3D:
            return Insn(None, lin, 2, fpu=True)
        if n == 0x3C:
            esc = (raw[2] & 0x3F) | 0xC0
            seg = FPU_SEG[raw[2] >> 6]
            body = bytes([esc]) + raw[3:]
            i = next(md.disasm(body, lin), None)
            return Insn(i, lin, 2 + i.size, fpu=True, seg=seg) if i else None
        body = bytes([0xD8 + n - 0x34]) + raw[2:]
        i = next(md.disasm(body, lin), None)
        return Insn(i, lin, 1 + i.size, fpu=True) if i else None
    i = next(md.disasm(raw, lin), None)
    if i is None or i.mnemonic.startswith(("(bad)", ".byte")):
        return None
    return Insn(i, lin, i.size)


def in_ovl_area(seg):
    return OVL_SEG <= seg < DGROUP


# Capstone is given image-linear addresses, so relative branch targets come
# back linear; they are converted to offsets in CS where used.


class Analyzer:
    def __init__(self, prog):
        self.p = prog
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_16)
        self.md.detail = True
        self.insns = {}                 # (img, lin) -> Insn
        self.functions = set()          # (img, cs, ip)
        self.bodies = {}                # func -> set of (img, lin)
        self.calls = defaultdict(set)
        self.switches = {}              # (img, lin of jmp) -> [target ips]
        self.int3f = {}                 # (img, lin of int) -> (n, off)
        self.indirect = []              # (img, lin, text)
        self.ints = Counter()
        self.int_handlers = set()
        self.pointer_funcs = set()

    # ---- decoding ----------------------------------------------------------------
    def image_for(self, img, lin):
        im = self.p.code_image(img, lin)
        return im if im.contains(lin) else None

    def decode(self, img, lin):
        key = (img, lin)
        if key in self.insns:
            return self.insns[key]
        im = self.image_for(img, lin)
        if im is None:
            return None
        off = lin - im.base
        insn = decode_bytes(self.md, im.data[off:off + 15], lin)
        if insn is None:
            return None
        self.insns[key] = insn
        return insn

    def variants(self, img, lin):
        """Decoded alternatives of a run-time rewritten opcode."""
        im = self.image_for(img, lin)
        out = []
        for v in PATCHED_OPCODES.get((img, lin), ()):
            off = lin - im.base
            raw = v + im.data[off + len(v):off + 15]
            out.append(decode_bytes(self.md, raw, lin))
        return out

    def patched_bytes(self):
        """(image, linear) of instruction bytes the code rewrites with
        `mov cs:[imm], ...`."""
        stores = set()
        for (img, lin), insn in list(self.insns.items()):
            ops = insn.operands
            if insn.fpu or insn.mnemonic != "mov" or len(ops) != 2:
                continue
            m = ops[0].mem if ops[0].type == x86.X86_OP_MEM else None
            if m is None or m.segment != x86.X86_REG_CS or m.base or m.index:
                continue
            cs = 0 if img >= DRIVER_BASE else (lin - (lin & 0xF)) // 16
            for f in self.functions:
                if f[0] == img and (img, lin) in self.bodies.get(f, ()):
                    cs = f[1]
                    break
            for k in range(ops[0].size):
                stores.add((img, cs * 16 + ((m.disp + k) & 0xFFFF)))
        covered = set()
        for (img, lin), insn in self.insns.items():
            covered.update((img, a) for a in range(lin, lin + insn.size))
        return stores & covered

    def owner_ovl(self, img, seg):
        """Image a far reference to `seg` lands in, seen from code in `img`."""
        if img >= DRIVER_BASE:
            return img
        return img if in_ovl_area(seg) else 0

    def far_target(self, img, insn):
        """(cs, ip) of `call/jmp far seg:off` if the segment is relocated."""
        im = self.image_for(img, insn.address)
        if im is not None and insn.size == 5 and (insn.address + 3) in im.relocs:
            ip, seg = struct.unpack_from("<HH", im.data, insn.address + 1 - im.base)
            return seg, ip
        return None

    # ---- exploration ------------------------------------------------------------
    def add_function(self, img, cs, ip):
        f = (self.owner_ovl(img, cs), cs, ip)
        self.functions.add(f)
        return f

    def explore(self, func):
        img, cs, entry = func
        body = self.bodies.setdefault(func, set())
        work = [entry]
        while work:
            ip = work.pop()
            history = []
            while True:
                lin = cs * 16 + ip
                if (img, lin) in body:
                    break
                insn = self.decode(img, lin)
                if insn is None:
                    self.indirect.append((img, lin, "undecodable"))
                    break
                body.add((img, lin))
                history.append(insn)
                for v in self.variants(img, lin)[1:]:
                    if v.bytes[0] in (0xE9, 0xEB):
                        work.append((v.operands[0].imm - cs * 16) & 0xFFFF)
                    else:
                        work.append((ip + v.size) & 0xFFFF)
                mn = insn.mnemonic
                nxt = (ip + insn.size) & 0xFFFF
                if insn.fpu:
                    self.ints["fpu"] += 1
                    ip = nxt
                    continue
                if mn == "int":
                    num = insn.operands[0].imm & 0xFF
                    self.ints[num] += 1
                    if num == 0x3F:
                        im = self.image_for(img, lin)
                        n = im.u8(lin + 2)
                        off = im.u16(lin + 3)
                        tovl, _ = self.p.int3f_target(n, off)
                        callee = (tovl, self.p.int3f_seg[n], off)
                        self.int3f[(img, lin)] = (n, off)
                        self.functions.add(callee)
                        self.calls[func].add(callee)
                        ip = (nxt + 3) & 0xFFFF     # 3 inline bytes skipped
                        continue
                    if num == 0x21:
                        self.note_setvect(img, cs, history)
                    ip = nxt
                    continue
                if mn in ("ret", "retf", "iret", "hlt"):
                    break
                if mn in ("call", "lcall"):
                    b0 = insn.bytes[0]
                    if b0 == 0xE8:
                        callee = self.add_function(img, cs, (insn.operands[0].imm - cs * 16) & 0xFFFF)
                        self.calls[func].add(callee)
                    elif b0 == 0x9A:
                        t = self.far_target(img, insn)
                        if t:
                            self.calls[func].add(self.add_function(img, *t))
                        else:
                            self.indirect.append((img, lin, f"{mn} {insn.op_str} (no reloc)"))
                    else:
                        self.indirect.append((img, lin, f"{mn} {insn.op_str}"))
                    ip = nxt
                    continue
                if (img, lin) in PATCHED_JUMPS:
                    work.extend(PATCHED_JUMPS[(img, lin)])
                    break
                if mn in ("jmp", "ljmp"):
                    b0 = insn.bytes[0]
                    if b0 in (0xE9, 0xEB):
                        ip = (insn.operands[0].imm - cs * 16) & 0xFFFF
                        continue
                    if b0 == 0xEA:
                        t = self.far_target(img, insn)
                        if t:
                            self.calls[func].add(self.add_function(img, *t))
                        else:
                            self.indirect.append((img, lin, f"{mn} {insn.op_str} (no reloc)"))
                        break
                    prev = [self.insns[k] for k in sorted(body)
                            if k[1] < lin and k in self.insns][-12:] + [insn]
                    targets = self.switch_targets(img, cs, insn, prev)
                    if targets is not None:
                        self.switches[(img, lin)] = targets
                        work.extend(targets)
                    else:
                        self.indirect.append((img, lin, f"{mn} {insn.op_str}"))
                    break
                if mn in JCC:
                    work.append((insn.operands[0].imm - cs * 16) & 0xFFFF)
                ip = nxt

    def note_setvect(self, img, cs, history):
        """INT 21h AH=25h with DX = handler offset and DS = CS before it."""
        ah = dx = None
        ds_is_cs = False
        for insn in reversed(history[-12:-1]):
            t = insn.op_str
            if ah is None and insn.mnemonic == "mov" and t.startswith("ah, 0x"):
                ah = int(t[4:], 16)
            if ah is None and insn.mnemonic == "mov" and t.startswith("ax, 0x25"):
                ah = 0x25
            if dx is None and insn.mnemonic == "mov" and t.startswith("dx, 0x"):
                dx = int(t[4:], 16)
            if (insn.mnemonic == "push" and t == "cs") or \
                    (insn.mnemonic == "mov" and t in ("ds, dx", "dx, cs", "ax, cs")):
                ds_is_cs = True
        if ah == 0x25 and dx is not None and ds_is_cs:
            self.int_handlers.add(self.add_function(img, cs, dx))

    def switch_targets(self, img, cs, insn, history):
        """MSC switch: `cmp reg, n; ja default; add ax, ax; xchg bx, ax;
        jmp cs:[bx + table]`."""
        ops = insn.operands
        if len(ops) != 1 or ops[0].type != x86.X86_OP_MEM:
            return None
        m = ops[0].mem
        if m.segment != x86.X86_REG_CS or m.index != 0 or m.base == 0:
            return None
        table = m.disp & 0xFFFF
        count = None
        for h in reversed(history[-12:-1]):
            if h.mnemonic == "cmp" and len(h.operands) == 2 and \
                    h.operands[1].type == x86.X86_OP_IMM:
                count = (h.operands[1].imm & 0xFFFF) + 1
                break
        if count is None or count > 256:
            return None
        im = self.image_for(img, cs * 16 + table)
        if im is None:
            return None
        return [im.u16(cs * 16 + table + 2 * i) for i in range(count)]

    # ---- entry points from data ---------------------------------------------------
    def code_segments(self):
        return {(f[0], f[1]) for f in self.functions}

    def decodes_cleanly(self, img, lin, limit=40):
        """Straight-line decode from lin reaches a return or jump without
        invalid opcodes."""
        for _ in range(limit):
            insn = self.decode(img, lin)
            if insn is None:
                return False
            if insn.mnemonic in ("ret", "retf", "iret", "jmp", "ljmp"):
                return True
            lin += insn.size
        return False

    def pointer_candidates(self):
        """Far pointers (offset, relocated segment) into known code segments."""
        segs = {s for s in self.code_segments() if s[1] not in NOT_CODE_SEGS}
        out = set()
        images = [(0, self.p.root)] + sorted(self.p.overlays.items())
        for img, im in images:
            for r in im.relocs:
                if not im.contains(r - 2) or not im.contains(r + 1):
                    continue
                seg = im.u16(r)
                off = im.u16(r - 2)
                towner = self.owner_ovl(img, seg)
                if (towner, seg) not in segs:
                    continue
                lin = seg * 16 + off
                tim = self.p.code_image(towner, lin)
                if not tim.contains(lin + 3):
                    continue
                start = tim.data[lin - tim.base:lin - tim.base + 3]
                if start == bytes([0x55, 0x8B, 0xEC]) or self.decodes_cleanly(towner, lin):
                    out.add((towner, seg, off))
        return out

    def code_table(self, d, im, lin, b):
        """Table offset of `mov r16, [bx+table]` (optionally cs:) whose
        register is jumped or called through within the next 3
        instructions, else None."""
        if b[:1] == bytes([0x2E]):
            b = b[1:]
        if len(b) != 4 or b[0] != 0x8B or (b[1] & 0xC7) != 0x87:
            return None
        reg = (b[1] >> 3) & 7
        nxt = lin + len(b) + (1 if im.data[lin] == 0x2E else 0)
        for _ in range(3):
            insn = self.insns.get((d, nxt))
            if insn is None:
                return None
            c = insn.bytes
            if len(c) == 2 and c[0] == 0xFF and c[1] in (0xD0 | reg, 0xE0 | reg):
                return struct.unpack_from("<H", b, 2)[0]
            nxt += insn.size
        return None

    def driver_pointer_candidates(self):
        """Near code pointers a driver stores into a variable (mov word [m],
        imm) and later calls or jumps through (call/jmp word [m])."""
        out = set()
        for d, im in self.p.drivers.items():
            stores, slots = {}, set()
            for (o, lin), insn in list(self.insns.items()):
                if o != d:
                    continue
                b = im.data[lin:lin + insn.size]
                slot = memory_slot(b)
                if slot and b[0] == 0xC7 and (b[1] >> 3) & 7 == 0:
                    stores.setdefault(slot, set()).add(
                        struct.unpack_from("<H", b, len(b) - 2)[0])
                elif slot and b[0] == 0xFF and (b[1] >> 3) & 7 in (2, 4):
                    slots.add(slot)
                else:
                    # mov reg, [cs:][bx+table] followed shortly by jmp/call
                    # reg: read the table while its entries look like code
                    t = self.code_table(d, im, lin, b)
                    if t is None:
                        continue
                    for k in range(128):
                        if t + 2 * k + 2 > len(im.data):
                            break
                        v = im.u16(t + 2 * k)
                        if v >= len(im.data) or not self.decodes_cleanly(d, v):
                            break
                        out.add((d, 0, v))
            for m in slots:
                for t in stores.get(m, ()):
                    if t < len(im.data) and self.decodes_cleanly(d, t):
                        out.add((d, 0, t))
        return out

    def run(self):
        cs, ip = self.p.entry
        self.functions.add((0, cs, ip))
        for d in self.p.drivers:
            for e in self.p.driver_entries(d):
                self.functions.add((d, 0, e))
        extra = Path(__file__).parent / "extra_entries.txt"
        if extra.exists():
            for line in extra.read_text().splitlines():
                parts = line.split("#")[0].split()
                if len(parts) == 3:
                    self.functions.add((int(parts[0], 16), int(parts[1], 16), int(parts[2], 16)))
        done = set()
        for _ in range(10):
            while self.functions - done:
                f = min(self.functions - done)
                done.add(f)
                self.explore(f)
            new = (self.pointer_candidates() | self.driver_pointer_candidates()) - self.functions
            if not new:
                break
            self.pointer_funcs |= new
            self.functions |= new

    def coverage(self):
        cov = {}
        for img, im in [(0, self.p.root)] + sorted(self.p.overlays.items()):
            covered = set()
            for (o, lin), insn in list(self.insns.items()):
                if o == img and im.contains(lin):
                    covered.update(range(lin, lin + insn.size))
            lo, hi = im.base, im.base + len(im.data)
            if img == 0:
                hi = OVL_SEG * 16    # root code ends where the overlay area starts
            cov[img] = (len([c for c in covered if lo <= c < hi]), hi - lo)
        return cov


def fname(f):
    return f"f{f[0]:02d}_{f[1]:04x}_{f[2]:04x}"


def main():
    prog = Program(sys.argv[1])
    an = Analyzer(prog)
    an.run()
    print(f"functions: {len(an.functions)} (via far pointers {len(an.pointer_funcs)}, "
          f"interrupt handlers {len(an.int_handlers)})")
    per = Counter(f[0] for f in an.functions)
    for img, (cov, total) in an.coverage().items():
        print(f"  overlay {img:2d}: {per[img]:4d} funcs, decoded {cov:6d}/{total:6d} bytes")
    print(f"  drivers: {sum(v for k, v in per.items() if k >= DRIVER_BASE)} funcs")
    print(f"switch tables: {len(an.switches)}, INT 3Fh calls: {len(an.int3f)}")
    print("interrupts used:", {(hex(k) if isinstance(k, int) else k): v for k, v in an.ints.items()})
    kinds = Counter(t.split(" (")[0] for _, _, t in an.indirect)
    print(f"unresolved: {len(an.indirect)}")
    for k, v in kinds.most_common(15):
        print(f"  {v:4d} {k}")
    if len(sys.argv) > 2:
        Path(sys.argv[2]).write_text(json.dumps({
            "functions": sorted(fname(f) for f in an.functions),
            "handlers": sorted(fname(f) for f in an.int_handlers),
            "calls": {fname(k): sorted(fname(c) for c in v) for k, v in an.calls.items()},
            "unresolved": [f"{o}:{lin:05x} {t}" for o, lin, t in an.indirect],
        }, indent=1))


if __name__ == "__main__":
    main()


