"""Unpack IT.DRV (the game's video/sound drivers) and other LZW data.

Format of IT.DRV: u16 count, u32 offset[count]; each module starts with a
small header followed by LZW data, decoded like the game does (040E:0EFD):
variable-width codes LSB first, 9 bits growing to 12, code 256 = reset,
257 = end, first free code 258.

Usage: itdrv.py IT.DRV <outdir>
"""
import struct
import sys
from pathlib import Path


def lzw_decode(data, pos=0, limit=None):
    out = bytearray()
    bits = 0
    bitpos = pos * 8
    end = len(data) * 8
    width = 9
    table = {}
    next_code = 258
    prev = None
    while bitpos + width <= end:
        byte = bitpos >> 3
        chunk = int.from_bytes(data[byte:byte + 3].ljust(3, b"\0"), "little")
        code = (chunk >> (bitpos & 7)) & ((1 << width) - 1)
        bitpos += width
        if code == 257:
            break
        if code == 256:
            width = 9
            table = {}
            next_code = 258
            prev = None
            continue
        if code < 256:
            entry = bytes([code])
        elif code in table:
            entry = table[code]
        elif code == next_code and prev is not None:
            entry = prev + prev[:1]
        else:
            raise ValueError(f"bad LZW code {code} at bit {bitpos - width}")
        out += entry
        if prev is not None and next_code < 4096:
            table[next_code] = prev + entry[:1]
            next_code += 1
            if next_code >= (1 << width) and width < 12:
                width += 1
        prev = entry
        if limit and len(out) >= limit:
            break
    return bytes(out)


def modules(path):
    data = Path(path).read_bytes()
    n = struct.unpack_from("<H", data, 0)[0]
    offs = [struct.unpack_from("<I", data, 2 + 4 * i)[0] for i in range(n)] + [len(data)]
    for i in range(n):
        yield i, data[offs[i]:offs[i + 1]]


def main():
    out = Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    for i, blk in modules(sys.argv[1]):
        packed, mode, size = struct.unpack_from("<IHH", blk, 0)
        for start in (8, 10, 12, 14, 16):
            try:
                raw = lzw_decode(blk, start, size)
            except ValueError:
                continue
            if len(raw) >= size * 0.9:
                break
        (out / f"drv{i}.bin").write_bytes(raw)
        print(f"module {i}: header {blk[:16].hex()} packed {packed} unpacked {len(raw)}/{size} (data at +{start})")


if __name__ == "__main__":
    main()
