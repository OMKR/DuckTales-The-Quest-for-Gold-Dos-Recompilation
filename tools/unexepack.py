"""Unpack a Microsoft EXEPACK compressed DOS executable.

Usage: unexepack.py <packed.exe> <out.exe>

EXEPACK layout: the MZ load image holds the compressed program, followed at
CS:0 by the EXEPACK header (real IP/CS/SP/SS, sizes, "RB" signature), the
unpacker stub and the packed relocation table. The compressed data is
decoded backwards from its end with two commands:
  0xB0/0xB1: fill  <count:u16> <byte>
  0xB2/0xB3: copy  <count:u16> <count bytes>
  bit 0 set = last command.
Any data appended after the load image (overlays) is kept unchanged.
"""
import struct
import sys


def unpack(data):
    (sig, last, pages, nrel, hdr_par, minalloc, maxalloc, ss, sp, csum, ip, cs,
     relo_off, ovl) = struct.unpack_from("<2s13H", data, 0)
    assert sig == b"MZ"
    image_start = hdr_par * 16
    image_end = (pages - 1) * 512 + (last or 512)
    image = bytearray(data[image_start:image_end])
    tail = data[image_end:]

    eh = cs * 16                      # EXEPACK header at CS:0 in the image
    (real_ip, real_cs, mem_start, exepack_size, real_sp, real_ss,
     dest_len) = struct.unpack_from("<7H", image, eh)
    skip_len = 1
    if image[eh + 0x0E:eh + 0x10] == b"RB":
        hlen = 0x10
    else:  # 18-byte header variant with skip_len
        skip_len = struct.unpack_from("<H", image, eh + 0x0E)[0]
        assert image[eh + 0x10:eh + 0x12] == b"RB", "not EXEPACK"
        hlen = 0x12

    # decompress backwards
    packed_end = eh - (skip_len - 1) * 16
    out_size = dest_len * 16
    buf = bytearray(max(out_size, packed_end))
    buf[:packed_end] = image[:packed_end]
    src = packed_end - 1
    while src >= 0 and buf[src] == 0xFF:      # padding
        src -= 1
    dst = out_size - 1
    while True:
        cmd = buf[src]
        count = buf[src - 1] << 8 | buf[src - 2]
        src -= 3
        if cmd & 0xFE == 0xB0:
            val = buf[src]
            src -= 1
            for _ in range(count):
                buf[dst] = val
                dst -= 1
        elif cmd & 0xFE == 0xB2:
            for _ in range(count):
                buf[dst] = buf[src]
                dst -= 1
                src -= 1
        else:
            raise ValueError(f"bad EXEPACK command {cmd:#x}")
        if cmd & 1:
            break
    # the rest of the output below dst is unchanged packed-image data
    program = bytes(buf[:out_size])

    # packed relocation table follows the stub ("Packed file is corrupt" text)
    stub = bytes(image[eh:eh + exepack_size])
    msg = stub.find(b"Packed file is corrupt")
    pos = msg + len(b"Packed file is corrupt") if msg >= 0 else exepack_size - 16 * 2 * 2
    relocs = []
    for seg in range(16):
        n = struct.unpack_from("<H", stub, pos)[0]
        pos += 2
        for _ in range(n):
            off = struct.unpack_from("<H", stub, pos)[0]
            pos += 2
            relocs.append((seg * 0x1000, off))

    # build a normal MZ exe
    hdr_size = 0x20 + 4 * len(relocs)
    hdr_size = (hdr_size + 15) // 16 * 16
    total = hdr_size + len(program)
    out = bytearray(hdr_size)
    struct.pack_into("<2s13H", out, 0, b"MZ", total % 512, (total + 511) // 512,
                     len(relocs), hdr_size // 16, minalloc, maxalloc,
                     real_ss, real_sp, 0, real_ip, real_cs, 0x20, 0)
    for i, (seg, off) in enumerate(relocs):
        struct.pack_into("<HH", out, 0x20 + 4 * i, off, seg)
    out += program
    return bytes(out), tail, dict(ip=real_ip, cs=real_cs, ss=real_ss, sp=real_sp,
                                  relocs=len(relocs), size=len(program),
                                  minalloc=minalloc, maxalloc=maxalloc)


def main():
    data = open(sys.argv[1], "rb").read()
    exe, tail, info = unpack(data)
    open(sys.argv[2], "wb").write(exe)
    if tail:
        open(sys.argv[2] + ".tail", "wb").write(tail)
    print(info, "appended data:", len(tail))


if __name__ == "__main__":
    main()
