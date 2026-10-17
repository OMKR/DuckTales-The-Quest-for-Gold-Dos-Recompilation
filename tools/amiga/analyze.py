"""Recursive-descent analysis of DuckTales code: functions, call graph, coverage.

Code is addressed as (node, offset). Node 0 is the root; nodes 1..13 are the
overlay nodes in file order. A4 points to data hunk + $7FFE (Aztec C small
code/data model): the data hunk starts with a table of `jmp abs.l` entries for
root functions, followed by overlay stubs `bsr.w loader; dc.b ovl, 0; dc.w off`.
"""
import json
import struct
import sys
from collections import defaultdict
from pathlib import Path

import capstone

sys.path.insert(0, str(Path(__file__).parent))
from disasm import load  # noqa: E402

A4_BIAS = 0x7FFE
# Overlay ordinals in the stub table skip the empty ordinal 4.
OVERLAY_ORDINAL_TO_NODE = {1: 1, 2: 2, 3: 3, **{k: k - 1 for k in range(5, 15)}}

BRANCHES = {"bra", "bhi", "bls", "bcc", "bcs", "bne", "beq", "bvc", "bvs",
            "bpl", "bmi", "bge", "blt", "bgt", "ble", "bhs", "blo"}
DBCC = {"dbra", "dbf", "dbt", "dbhi", "dbls", "dbcc", "dbcs", "dbne", "dbeq",
        "dbvc", "dbvs", "dbpl", "dbmi", "dbge", "dblt", "dbgt", "dble"}


def s16(data, pos):
    return struct.unpack_from(">h", data, pos)[0]


def u32(data, pos):
    return struct.unpack_from(">I", data, pos)[0]


class Program:
    def __init__(self, exe):
        nodes = load(Path(exe).read_bytes())
        self.code = {0: nodes[0][0]["bytes"]}
        for i, hunks in enumerate(nodes[1:], start=1):
            self.code[i] = hunks[0]["bytes"]
        self.root_relocs = nodes[0][0]["relocs"]
        self.data = nodes[0][1]["bytes"]
        self.data_relocs = nodes[0][1]["relocs"]
        self.a4_targets = {}  # A4 displacement -> (node, offset)
        self._read_jump_tables()

    def _read_jump_tables(self):
        d = self.data
        off = 0
        while d[off:off + 2] == b"\x4e\xf9":
            assert self.data_relocs.get(off + 2) == 0
            self.a4_targets[off - A4_BIAS] = (0, u32(d, off + 2))
            off += 6
        self.root_entries = off // 6
        off += 2  # padding word
        self.stubs = []
        while d[off:off + 2] == b"\x61\x00":
            node = OVERLAY_ORDINAL_TO_NODE[d[off + 4]]
            target = (node, struct.unpack_from(">H", d, off + 6)[0])
            self.a4_targets[off - A4_BIAS] = target
            self.stubs.append((off - A4_BIAS, target))
            off += 8


class Analyzer:
    def __init__(self, prog):
        self.prog = prog
        self.md = capstone.Cs(capstone.CS_ARCH_M68K, capstone.CS_MODE_M68K_000)
        self.insns = {}            # (node, off) -> (size, mnemonic, op_str)
        self.functions = set()     # (node, off)
        self.calls = defaultdict(set)   # caller func -> callee funcs
        self.lib_calls = defaultdict(set)
        self.hw_refs = defaultdict(set)
        self.unresolved = []       # indirect jumps/calls: (node, off, text)
        self.func_of = {}          # (node, off) -> owning function
        self.switches = {}         # (node, jmp off) -> (table off, [targets])
        self.data_in_code = set()  # (node, off) of switch table words
        self.code_ptrs = set()     # (node, off) code addresses taken as values
        self.bodies = {}           # function entry -> set of (node, off) in it

    def switch_targets(self, node, off, op, history):
        """Decode `cmp #N,d0; ...; move.w T(pc,d0.w),d0; jmp B(pc,d0.w)`."""
        base = int(op.split("(")[0][1:], 16)
        table = count = None
        for _, mn, hop in reversed(history[-6:]):
            if table is None and mn == "move.w" and "(pc, d0.w)" in hop:
                table = int(hop.split("(")[0][1:], 16)
            if count is None and mn in ("cmp.l", "cmpi.l", "cmp.w", "cmpi.w") \
                    and hop.endswith("d0") and hop.startswith("#"):
                count = int(hop.split(",")[0][2:], 16)
        if table is None or count is None:
            return None
        code = self.prog.code[node]
        targets = [base + s16(code, table + 2 * i) for i in range(count)]
        self.switches[(node, off)] = (table, targets)
        for i in range(count):
            self.data_in_code.add((node, table + 2 * i))
        return targets

    def decode(self, node, off):
        code = self.prog.code[node]
        insn = next(self.md.disasm(code[off:off + 10], off), None)
        # Capstone builds differ for invalid words: the Windows build returns
        # nothing, the Linux build a "dc.w" pseudo instruction. Treat both as
        # undecodable so the analysis is the same on every platform.
        if insn is None or insn.mnemonic.startswith("dc."):
            return None
        return insn.size, insn.mnemonic, insn.op_str

    def target_of(self, node, off, size, op):
        """Resolve a direct control-flow target; None if indirect."""
        code = self.prog.code[node]
        op = op.strip()
        if op.endswith("(pc)") and "," not in op:
            return node, int(op[1:-4], 16)
        if op.startswith("$") and op[1:].replace(".w", "").replace(".l", "").isalnum() \
                and "(" not in op:
            if node == 0 and off + 2 in self.prog.root_relocs:
                return 0, u32(code, off + 2)
            if op[1:].isalnum():
                return node, int(op[1:], 16)
        if op.endswith("(a4)") and "," not in op and op != "(a4)":
            disp = int(op[:-4].replace("$", ""), 16) if not op.startswith("-") \
                else -int(op[1:-4].replace("$", ""), 16)
            return self.prog.a4_targets.get(disp)
        return None

    def pointer_table_targets(self, op, history):
        """Decode `lea.l TABLE.l,aN; movea.l (aN,d0.w),aN; ...; jmp (aN)`
        where TABLE is a run of relocated longwords in the data hunk."""
        reg = op.strip("()")
        if len(reg) != 2 or reg[0] != "a":
            return None
        table = count = None
        for _, mn, hop in reversed(history[-8:]):
            if table is None and mn == "lea.l" and hop.endswith(f", {reg}") \
                    and hop.endswith(f".l, {reg}"):
                table = int(hop.split(".l")[0][1:], 16)
            if count is None and mn.startswith("cmp") and hop.startswith("#") \
                    and hop.endswith("d0"):
                count = int(hop.split(",")[0][2:], 16) + 1
        if table is None:
            return None
        d, relocs = self.prog.data, self.prog.data_relocs
        targets = []
        pos = table
        while relocs.get(pos) == 0 and (count is None or len(targets) < count):
            targets.append(u32(d, pos))
            pos += 4
        if targets:
            self.switches[(0, -table)] = (table, targets)
        return targets

    def explore_function(self, entry):
        work = [entry]
        seen = self.bodies[entry] = set()
        while work:
            node, off = work.pop()
            history = []
            while (node, off) not in seen:
                if off >= len(self.prog.code[node]):
                    break
                seen.add((node, off))
                dec = self.decode(node, off)
                if dec is None:
                    self.unresolved.append((node, off, "undecodable"))
                    break
                size, mn, op = dec
                self.insns[(node, off)] = dec
                self.func_of.setdefault((node, off), entry)
                history.append(dec)
                if mn in ("lea.l", "pea.l") and op.split(",")[0].endswith("(pc)"):
                    self.code_ptrs.add((node, int(op.split("(")[0][1:], 16)))
                for tok in ("$dff", "$bfe", "$bfd"):
                    if tok in op and "(" not in op.split(tok)[0][-1:]:
                        self.hw_refs[entry].add(op)
                base = mn.split(".")[0]
                if base in ("jsr", "bsr"):
                    if op.endswith("(a6)"):
                        self.lib_calls[entry].add(op)
                    else:
                        tgt = self.target_of(node, off, size, op)
                        if tgt:
                            self.calls[entry].add(tgt)
                            self.functions.add(tgt)
                        else:
                            self.unresolved.append((node, off, f"{mn} {op}"))
                elif base in BRANCHES or base in DBCC:
                    tgt_text = op.split(",")[-1].strip()
                    tgt = (node, int(tgt_text[1:], 16))
                    if base == "bra":
                        off = tgt[1]
                        continue
                    work.append(tgt)
                elif base == "jmp" and op.endswith("(pc,d0.w)"):
                    targets = self.switch_targets(node, off, op, history)
                    if targets is None:
                        self.unresolved.append((node, off, f"{mn} {op}"))
                    else:
                        work.extend((node, t) for t in targets)
                    break
                elif base == "jmp" and node == 0 and \
                        (targets := self.pointer_table_targets(op, history)):
                    work.extend((0, t) for t in targets)
                    break
                elif base == "jmp":
                    tgt = self.target_of(node, off, size, op)
                    if tgt:
                        # tail call to another function vs. local jump
                        if tgt[0] != node or op.endswith("(a4)"):
                            self.calls[entry].add(tgt)
                            self.functions.add(tgt)
                        else:
                            off = tgt[1]
                            continue
                    elif not op.endswith("(a6)"):
                        self.unresolved.append((node, off, f"{mn} {op}"))
                    break
                elif base in ("rts", "rte", "rtr", "illegal"):
                    break
                off += size

    def looks_like_function(self, node, off):
        code = self.prog.code[node]
        if off % 2 or off + 4 > len(code) or (node, off) in self.insns:
            return False
        word = code[off:off + 2]
        # link.w a5 / movem.l regs,-(a7) / link.w a6
        return word in (b"\x4e\x55", b"\x48\xe7", b"\x4e\x56")

    def looks_like_text(self, node, off):
        blob = self.prog.code[node][off:off + 12]
        printable = sum(32 <= c < 127 or c in (0, 10) for c in blob)
        return len(blob) == 0 or printable / len(blob) > 0.75

    def decodes_cleanly(self, node, off, limit=400):
        """True if straight-line/branching code from off reaches rts/rte
        without undecodable words or leaving the hunk."""
        code = self.prog.code[node]
        work, seen = [off], set()
        returned = False
        while work and len(seen) < limit:
            pos = work.pop()
            while pos not in seen:
                if pos % 2 or pos >= len(code) or (node, pos) in self.data_in_code:
                    return False
                seen.add(pos)
                dec = self.decode(node, pos)
                if dec is None:
                    return False
                size, mn, op = dec
                base = mn.split(".")[0]
                if base in ("rts", "rte", "rtr"):
                    returned = True
                    break
                if base in ("illegal", "trap", "stop", "reset"):
                    return False
                if base in BRANCHES or base in DBCC:
                    tgt = int(op.split(",")[-1].strip()[1:], 16)
                    if base == "bra":
                        pos = tgt
                        continue
                    work.append(tgt)
                elif base == "jmp":
                    break
                pos += size
        return returned

    def _drain(self, done):
        while self.functions - done:
            f = min(self.functions - done)
            done.add(f)
            self.explore_function(f)

    def run(self):
        roots = {t for t in self.prog.a4_targets.values()}
        roots.add((0, 0))
        self.functions |= roots
        done = set()
        self._drain(done)

        # Function pointers: root relocations into root code, and pc-relative
        # address computations seen during exploration.
        reloc_ptrs = set()
        code0 = self.prog.code[0]
        for relocs, blob in ((self.prog.data_relocs, self.prog.data),
                             (self.prog.root_relocs, code0)):
            for pos, hunk in relocs.items():
                if hunk == 0:
                    reloc_ptrs.add((0, u32(blob, pos)))
        candidates = reloc_ptrs | self.code_ptrs
        self.pointer_funcs = {c for c in candidates if self.looks_like_function(*c)}
        # Hand-written routines (interrupt handlers etc.) have no prologue:
        # accept a relocated pointer if it decodes cleanly to a return.
        # pc-relative lea/pea mostly point at strings, so they need a prologue.
        self.pointer_funcs |= {c for c in reloc_ptrs - self.pointer_funcs
                               if c not in self.insns and self.decodes_cleanly(*c)}
        # Interrupt handlers are also set up with pc-relative lea (e.g. the
        # Paula handlers at $5646/$56b6/$5726): accept those that decode
        # cleanly and do not look like text.
        self.pointer_funcs |= {c for c in self.code_ptrs - self.pointer_funcs
                               if c[0] == 0 and c not in self.insns
                               and not self.looks_like_text(*c)
                               and self.decodes_cleanly(*c)}
        self.functions |= self.pointer_funcs
        self._drain(done)

        # Last resort: prologues in still-unexplored gaps.
        self.scanned_funcs = set()
        changed = True
        while changed:
            changed = False
            for node, code in self.prog.code.items():
                for off in range(0, len(code) - 4, 2):
                    if (node, off) in self.data_in_code:
                        continue
                    if code[off:off + 2] == b"\x4e\x55" and self.looks_like_function(node, off) \
                            and not self._inside_insn(node, off):
                        self.scanned_funcs.add((node, off))
                        self.functions.add((node, off))
                        changed = True
            self._drain(done)

        # Code only reachable through unresolved pointers: any remaining
        # gap that decodes cleanly is treated as a function entry.
        self.gap_funcs = set()
        reloc_bytes = {p + i for p in self.prog.root_relocs for i in range(4)}
        for node, code in self.prog.code.items():
            off = 0
            code_end = set()
            for (n, o), (s, _, _) in self.insns.items():
                if n == node:
                    code_end.add(o + s)
            while off < len(code):
                if (node, off) in self.insns or (node, off) in self.data_in_code:
                    off += self.insns.get((node, off), (2,))[0]
                    continue
                prologue = code[off:off + 2] in (b"\x4e\x55", b"\x48\xe7")
                if node == 0 and any(off + i in reloc_bytes for i in range(8)):
                    off += 2
                    continue
                if (prologue or off in code_end) and self.decodes_cleanly(node, off):
                    text = sum(32 <= c < 127 or c in (0, 10)
                               for c in code[off:off + 16]) / 16
                    if text < 0.8:
                        self.gap_funcs.add((node, off))
                        self.functions.add((node, off))
                        self._drain(done)
                        continue
                off += 2

    def _inside_insn(self, node, off):
        for back in (2, 4, 6, 8):
            dec = self.insns.get((node, off - back))
            if dec and dec[0] > back:
                return True
        return False

    def coverage(self):
        out = {}
        for node, code in self.prog.code.items():
            covered = sum(s for (n, _), (s, _, _) in self.insns.items() if n == node)
            out[node] = (covered, len(code))
        return out


def main():
    prog = Program(sys.argv[1])
    an = Analyzer(prog)
    an.run()
    print(f"root jump table: {prog.root_entries} entries, overlay stubs: {len(prog.stubs)}")
    print(f"functions found: {len(an.functions)} "
          f"(via pointers {len(an.pointer_funcs)}, via prologue scan {len(an.scanned_funcs)}, "
          f"via gap scan {len(an.gap_funcs)})")
    mnemonics = defaultdict(int)
    for _, mn, _ in an.insns.values():
        mnemonics[mn] += 1
    print(f"distinct mnemonics: {len(mnemonics)}")
    print("  " + " ".join(f"{m}:{c}" for m, c in
                          sorted(mnemonics.items(), key=lambda kv: -kv[1])))
    print(f"switch tables decoded: {len(an.switches)}")
    for node, (cov, total) in an.coverage().items():
        nf = sum(1 for f in an.functions if f[0] == node)
        print(f"  node{node:02d}: {nf:4d} funcs, code coverage {cov:6d}/{total:6d}"
              f" ({100 * cov / total:5.1f}%)")
    print(f"unresolved indirect transfers: {len(an.unresolved)}")
    kinds = defaultdict(int)
    for _, _, t in an.unresolved:
        kinds[t.split()[0] + " " + (t.split()[1] if len(t.split()) > 1 else "")] += 1
    for k, v in sorted(kinds.items(), key=lambda kv: -kv[1])[:15]:
        print(f"    {v:4d}  {k}")
    if len(sys.argv) > 2:
        out = {
            "functions": sorted(f"{n:02d}:{o:05x}" for n, o in an.functions),
            "calls": {f"{n:02d}:{o:05x}": sorted(f"{a:02d}:{b:05x}" for a, b in v)
                      for (n, o), v in an.calls.items()},
            "lib_calls": {f"{n:02d}:{o:05x}": sorted(v) for (n, o), v in an.lib_calls.items()},
            "hw_refs": {f"{n:02d}:{o:05x}": sorted(v) for (n, o), v in an.hw_refs.items()},
            "unresolved": [f"{n:02d}:{o:05x} {t}" for n, o, t in an.unresolved],
            "stubs": [[d, f"{n:02d}:{o:05x}"] for d, (n, o) in prog.stubs],
        }
        Path(sys.argv[2]).write_text(json.dumps(out, indent=1))


if __name__ == "__main__":
    main()
