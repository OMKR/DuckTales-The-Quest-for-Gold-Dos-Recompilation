"""Load the unpacked DUCKTALE.EXE and its Microsoft LINK overlays.

All addresses are image-linear: segment * 16 + offset, with segments as
stored in the EXE (before relocation by the DOS load segment).

Root: the whole load image. Overlays 1..10: MZ blocks in the appended data;
each is loaded at the start of the overlay area (segment OVL_SEG) and has
its own relocation table (fixups relative to the program's load segment,
like the root's). The INT 3Fh call `CD 3F n off` calls segment
OVL_SEGS[n]:off in overlay OVL_NUM[n] (tables in DGROUP at 3234h/3270h).
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from itdrv import lzw_decode, modules  # noqa: E402

DGROUP = 0x120E
OVL_SEG = 0x0C17
OVL_COUNT = 10
INT3F_ENTRIES = 30
DRIVER_BASE = 100          # image numbers 100..106 = IT.DRV modules 0..6
# entry table sizes: video drivers (modules 0-3) 15 far pointers, sound
# drivers (4-6) 9 (copied to DGROUP:1FD4 / DGROUP:2010 by 040E:0617)
DRIVER_ENTRIES = {0: 15, 1: 15, 2: 15, 3: 15, 4: 9, 5: 9, 6: 9}


class Image:
    """One loadable unit: bytes at image-linear base, relocation offsets."""

    def __init__(self, number, base, data, relocs):
        self.number = number          # 0 = root, 1..10 = overlays
        self.base = base              # image-linear address of data[0]
        self.data = bytes(data)
        self.relocs = set(relocs)     # image-linear addresses of segment words

    def contains(self, lin):
        return self.base <= lin < self.base + len(self.data)

    def u8(self, lin):
        return self.data[lin - self.base]

    def u16(self, lin):
        return struct.unpack_from("<H", self.data, lin - self.base)[0]


def _mz(data, pos=0):
    (sig, last, pages, nrel, hdr_par, minalloc, maxalloc, ss, sp, csum, ip, cs,
     relo, ovl) = struct.unpack_from("<2s13H", data, pos)
    assert sig == b"MZ", hex(pos)
    size = (pages - 1) * 512 + (last or 512)
    relocs = []
    for k in range(nrel):
        off, seg = struct.unpack_from("<HH", data, pos + relo + 4 * k)
        relocs.append(seg * 16 + off)
    image = data[pos + hdr_par * 16:pos + size]
    return dict(size=size, relocs=relocs, image=image, ip=ip, cs=cs, ss=ss, sp=sp,
                ovl=ovl, minalloc=minalloc)


class Program:
    def __init__(self, exe_path):
        exe_path = Path(exe_path)
        data = exe_path.read_bytes()
        root = _mz(data)
        self.entry = (root["cs"], root["ip"])
        self.stack = (root["ss"], root["sp"])
        self.minalloc = root["minalloc"]
        self.root = Image(0, 0, root["image"], root["relocs"])
        tail = (exe_path.parent / (exe_path.name + ".tail")).read_bytes()
        self.overlays = {}
        pos = tail.find(b"MZ")
        while pos >= 0:
            o = _mz(tail, pos)
            # overlay relocations are relative to the program start, the
            # image itself is loaded at the overlay area
            self.overlays[o["ovl"]] = Image(o["ovl"], OVL_SEG * 16, o["image"], o["relocs"])
            nxt = pos + ((o["size"] + 511) // 512) * 512
            pos = tail.find(b"MZ", nxt) if nxt < len(tail) else -1
        # drivers: code addressed by offset only (CS is set at run time)
        self.drivers = {}
        drv_path = exe_path.parent / "IT.DRV"
        if not drv_path.exists():
            drv_path = exe_path.parent.parent / "files" / "IT.DRV"
        for k, blk in modules(drv_path):
            size = struct.unpack_from("<H", blk, 6)[0]
            code = lzw_decode(blk, 16, size)
            self.drivers[DRIVER_BASE + k] = Image(DRIVER_BASE + k, 0, code, [])
        ds = DGROUP * 16
        self.int3f_seg = [self.root.u16(ds + 0x3234 + 2 * i) for i in range(INT3F_ENTRIES)]
        self.int3f_ovl = [self.root.u8(ds + 0x3270 + i) for i in range(INT3F_ENTRIES)]

    def image(self, number):
        if number >= DRIVER_BASE:
            return self.drivers[number]
        return self.root if number == 0 else self.overlays[number]

    def driver_entries(self, number):
        img = self.drivers[number]
        return [img.u16(2 * i) for i in range(DRIVER_ENTRIES[number - DRIVER_BASE])]

    def code_image(self, number, lin):
        """Image holding code at lin for a function in overlay `number`."""
        if number >= DRIVER_BASE:
            return self.drivers[number]
        if number and self.overlays[number].contains(lin):
            return self.overlays[number]
        return self.root

    def int3f_target(self, n, off):
        """(overlay, image-linear address) called by `INT 3Fh n off`."""
        return self.int3f_ovl[n], self.int3f_seg[n] * 16 + off


if __name__ == "__main__":
    import sys
    p = Program(sys.argv[1])
    print("entry", [hex(x) for x in p.entry], "root", len(p.root.data), "relocs", len(p.root.relocs))
    for n, o in sorted(p.overlays.items()):
        print(f"overlay {n}: {len(o.data)} bytes at {o.base:#x}, {len(o.relocs)} relocs")
    print("int3f:", [(i, p.int3f_ovl[i], hex(p.int3f_seg[i])) for i in range(INT3F_ENTRIES)])
