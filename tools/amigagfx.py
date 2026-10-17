"""Amiga graphics for the DOS port.

The Amiga version's .arc files have the same layout as the DOS ones
(u16 count, u32 offset[count+1], entries with a 16-byte header):

    u32 size, u16 type, u16 width, u16 height, u16 frames/flag, ...

Type 1 = full picture, type 2 = sprite strip, type 4 = text. In the Amiga
files header word 5 is 1 (DOS: 0), which selects the RLE decoder the DOS
program also contains (040E:13CE): a byte with bit 7 clear repeats the next
byte (n & 7Fh) times, bit 7 set copies (n & 7Fh) bytes.

The decoded Amiga image has 5 bitplanes (32 colours), stored by 8-pixel
column: for each byte column, plane 0..4, each `height` bytes. Sprite
strips hold `frames` such images side by side in the same layout. Widths
are rounded up to 16 pixels.

The 32-colour palettes (12-bit RGB words) are in the Amiga program's data
hunk; `match_palettes` picks one per full picture by comparing it with the
DOS picture at the same position in the archive.

Usage: amigagfx.py <amiga game folder (with DT1, DT2)> <dos game folder> [out]
writes a preview sheet per archive to out/ (default build/amiga_gfx).
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from itdrv import lzw_decode  # noqa: E402



def arc_entries(path):
    d = Path(path).read_bytes()
    n = struct.unpack_from("<H", d, 0)[0]
    offs = [struct.unpack_from("<I", d, 2 + 4 * k)[0] for k in range(n + 1)]
    if n and not offs[n - 1] < offs[n] <= len(d):   # last entry: to the end of the file
        offs[n] = len(d)
    return [d[offs[i]:offs[i + 1]] for i in range(n)]


def header(e):
    if len(e) < 12:
        return None
    size, typ, w, h, f = struct.unpack_from("<IHHHH", e, 0)
    return {"size": size, "type": typ, "w": w, "h": h, "f": f}


def rle(e, pos=16):
    out = bytearray()
    while pos < len(e):
        c = e[pos]
        pos += 1
        n = c & 0x7F
        if c & 0x80:
            out += e[pos:pos + n]
            pos += n
        else:
            out += bytes([e[pos]]) * n
            pos += 1
    return bytes(out)


def amiga_indices(e):
    """Colour indices (rows of ints) of an Amiga picture or sprite strip."""
    hd = header(e)
    data = rle(e)
    w, h = hd["w"], hd["h"]
    cols = len(data) // (5 * h)          # byte columns (all frames)
    rows = []
    for y in range(h):
        row = []
        for xb in range(cols):
            base = xb * h * 5 + y
            planes = [data[base + p * h] for p in range(5)]
            for bit in range(7, -1, -1):
                c = 0
                for p in range(5):
                    c |= ((planes[p] >> bit) & 1) << p
                row.append(c)
        rows.append(row)
    return rows


def dos_indices(e):
    """Colour indices of a DOS picture (LZW, 4 bits per pixel)."""
    hd = header(e)
    data = lzw_decode(e, 16)
    w, h = hd["w"], hd["h"]
    rows = []
    for y in range(h):
        row = []
        for x in range(w):
            b = data[(y * w + x) // 2]
            row.append(b >> 4 if x % 2 == 0 else b & 15)
        rows.append(row)
    return rows


EGA16 = [(0, 0, 0), (0, 0, 170), (0, 170, 0), (0, 170, 170), (170, 0, 0), (170, 0, 170),
         (170, 85, 0), (170, 170, 170), (85, 85, 85), (85, 85, 255), (85, 255, 85),
         (85, 255, 255), (255, 85, 85), (255, 85, 255), (255, 255, 85), (255, 255, 255)]


def find_palettes(exe):
    """The palette table: the longest run of 32-word palettes (colour 0
    black, 12-bit words) 64 bytes apart (0xB066-0xB5E6)."""
    def valid(i):
        if i + 64 > len(exe):
            return False
        ws = struct.unpack_from(">32H", exe, i)
        return ws[0] == 0 and max(ws) < 0x1000 and len(set(ws)) >= 4
    best, best_n, st = 0, 0, 0
    while st + 64 <= len(exe):
        n = 0
        while valid(st + 64 * n):
            n += 1
        if n > best_n:
            best, best_n = st, n
        st += 64 * (n - 1) if n > 1 else 2
    return [(best + 64 * k, [(((w >> 8) & 15) * 17, ((w >> 4) & 15) * 17, (w & 15) * 17)
                             for w in struct.unpack_from(">32H", exe, best + 64 * k)]) for k in range(best_n)]


def match_palette(amiga_rows, dos_rows, pals):
    """Palette offset whose colours best reproduce the DOS picture."""
    h = min(len(amiga_rows), len(dos_rows))
    w = min(len(amiga_rows[0]), len(dos_rows[0]))
    best = None
    for off, pal in pals:
        err = 0
        for y in range(0, h, 4):
            for x in range(0, w, 4):
                a = pal[amiga_rows[y][x]]
                d = EGA16[dos_rows[y][x]]
                err += sum((a[k] - d[k]) ** 2 for k in range(3))
        if best is None or err < best[0]:
            best = (err, off)
    return best[1]


def amiga_path(amiga_dir, name):
    for disk in ("DT1", "DT2"):
        p = Path(amiga_dir) / disk / name
        if p.exists():
            return p
    return None


def main():
    from PIL import Image
    amiga_dir, dos_dir = sys.argv[1], sys.argv[2]
    out = Path(sys.argv[3] if len(sys.argv) > 3 else "build/amiga_gfx")
    out.mkdir(parents=True, exist_ok=True)
    exe = (Path(amiga_dir) / "DT1" / "DuckTales").read_bytes()
    pals = find_palettes(exe)
    print(f"{len(pals)} palettes")
    for dos_arc in sorted(Path(dos_dir).glob("*.[Aa][Rr][Cc]")):
        ap = amiga_path(amiga_dir, dos_arc.name.lower())
        if ap is None:
            continue
        A, D = arc_entries(ap), arc_entries(dos_arc)
        pics = []
        cur_pal = pals[0][1]
        for i, e in enumerate(A):
            hd = header(e)
            if not hd or hd["type"] not in (1, 2):
                continue
            rows = amiga_indices(e)
            if hd["type"] == 1:
                dh = header(D[i]) if i < len(D) else None
                if dh and dh["type"] == 1 and abs(dh["w"] - hd["w"]) < 16:
                    off = match_palette(rows, dos_indices(D[i]), pals)
                    cur_pal = dict(pals)[off]
                    print(f"{dos_arc.name} {i}: picture {hd['w']}x{hd['h']} palette {off:#x}")
            im = Image.new("RGB", (len(rows[0]), len(rows)))
            im.putdata([cur_pal[c] for r in rows for c in r])
            pics.append(im)
        if not pics:
            continue
        cols = 4
        cw, ch = 320, max(p.height for p in pics)
        sheet = Image.new("RGB", (cols * cw, ((len(pics) + cols - 1) // cols) * ch))
        for k, p in enumerate(pics):
            sheet.paste(p.crop((0, 0, min(p.width, cw), p.height)), ((k % cols) * cw, (k // cols) * ch))
        sheet.save(out / f"{dos_arc.stem.lower()}.png")


if __name__ == "__main__":
    main()
