"""Disassemble the DuckTales hunk executable, including overlay nodes.

Writes one .s file per code hunk into the output directory, with
relocated longwords annotated as hunk references.
"""
import struct
import sys
from pathlib import Path

import capstone

HUNK_HEADER, HUNK_CODE, HUNK_DATA, HUNK_BSS = 0x3F3, 0x3E9, 0x3EA, 0x3EB
HUNK_RELOC32, HUNK_SYMBOL, HUNK_DEBUG, HUNK_END = 0x3EC, 0x3F0, 0x3F1, 0x3F2
HUNK_OVERLAY, HUNK_BREAK = 0x3F5, 0x3F6


def u32(data, pos):
    return struct.unpack_from(">I", data, pos)[0]


def read_header(data, pos):
    """Parse a HUNK_HEADER at pos; return (first_hunk, sizes, pos_after)."""
    assert u32(data, pos) == HUNK_HEADER
    pos += 4
    while u32(data, pos):
        pos += 4 + u32(data, pos) * 4
    pos += 4
    first, last = u32(data, pos + 4), u32(data, pos + 8)
    pos += 12
    sizes = [u32(data, pos + 4 * i) & 0x3FFFFFFF for i in range(last - first + 1)]
    return first, sizes, pos + 4 * len(sizes)


def read_hunks(data, pos, first):
    """Read hunks until HUNK_OVERLAY, HUNK_BREAK or EOF."""
    hunks, num = [], first
    cur = None
    while pos < len(data):
        kind = u32(data, pos) & 0x3FFFFFFF
        pos += 4
        if kind in (HUNK_CODE, HUNK_DATA):
            n = u32(data, pos) * 4
            cur = {"num": num, "kind": "CODE" if kind == HUNK_CODE else "DATA",
                   "bytes": data[pos + 4:pos + 4 + n], "relocs": {}}
            hunks.append(cur)
            num += 1
            pos += 4 + n
        elif kind == HUNK_BSS:
            hunks.append({"num": num, "kind": "BSS", "bytes": b"", "relocs": {}})
            num += 1
            pos += 4
        elif kind == HUNK_RELOC32:
            while (n := u32(data, pos)):
                target = u32(data, pos + 4)
                for i in range(n):
                    cur["relocs"][u32(data, pos + 8 + 4 * i)] = target
                pos += 8 + n * 4
            pos += 4
        elif kind == HUNK_SYMBOL:
            while (n := u32(data, pos)):
                pos += 4 + n * 4 + 4
            pos += 4
        elif kind == HUNK_DEBUG:
            pos += 4 + u32(data, pos) * 4
        elif kind == HUNK_END:
            pass
        elif kind in (HUNK_OVERLAY, HUNK_BREAK):
            return hunks, pos - 4
        else:
            raise ValueError(f"unknown hunk 0x{kind:x} at 0x{pos - 4:x}")
    return hunks, pos


def set_alloc(hunks, first, sizes):
    """Memory size of each hunk from the header (may exceed file size)."""
    for hk in hunks:
        hk["alloc"] = sizes[hk["num"] - first] * 4


def load(data):
    """Return list of nodes; node 0 is the root, the rest are overlays."""
    first, sizes, pos = read_header(data, 0)
    root, pos = read_hunks(data, pos, first)
    set_alloc(root, first, sizes)
    nodes = [root]
    if u32(data, pos) == HUNK_OVERLAY:
        pos += 8 + (u32(data, pos + 4) + 1) * 4
        while pos < len(data):
            if u32(data, pos) == HUNK_BREAK:
                pos += 4
                continue
            first, sizes, pos = read_header(data, pos)
            hunks, pos = read_hunks(data, pos, first)
            set_alloc(hunks, first, sizes)
            nodes.append(hunks)
    return nodes


def disassemble(hunk, out):
    md = capstone.Cs(capstone.CS_ARCH_M68K, capstone.CS_MODE_M68K_000)
    code, relocs = hunk["bytes"], hunk["relocs"]
    pos = 0
    while pos < len(code):
        insn = next(md.disasm(code[pos:pos + 10], pos), None)
        if insn is None:
            out.write(f"{pos:06x}:  dc.w    ${code[pos]:02x}{code[pos + 1]:02x}\n")
            pos += 2
            continue
        notes = [f"-> hunk{relocs[o]}+${u32(code, o):x}"
                 for o in range(pos, pos + insn.size) if o in relocs]
        raw = code[pos:pos + insn.size].hex()
        line = f"{pos:06x}:  {raw:20} {insn.mnemonic:8} {insn.op_str}"
        out.write(line + (f"   ; {' '.join(notes)}" if notes else "") + "\n")
        pos += insn.size


def main():
    exe, outdir = Path(sys.argv[1]), Path(sys.argv[2])
    outdir.mkdir(parents=True, exist_ok=True)
    nodes = load(exe.read_bytes())
    for node_index, hunks in enumerate(nodes):
        for hunk in hunks:
            name = f"node{node_index:02d}_hunk{hunk['num']:02d}_{hunk['kind'].lower()}"
            print(f"{name}: {len(hunk['bytes'])} bytes, {len(hunk['relocs'])} relocs")
            if hunk["kind"] == "CODE":
                with open(outdir / f"{name}.s", "w") as out:
                    disassemble(hunk, out)
            elif hunk["kind"] == "DATA":
                (outdir / f"{name}.bin").write_bytes(hunk["bytes"])


if __name__ == "__main__":
    main()
