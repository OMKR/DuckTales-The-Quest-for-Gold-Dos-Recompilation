"""Creates dist/DuckTalesDOS: a package without any of the game's code or
data. The user supplies their own copy of the game and builds the game
locally (settings -> "Build game from game files", or install.bat /
install.sh). Works on Windows, Linux and macOS (python make_dist.py) and
needs no game files.

The Windows settings program is cross-compiled with zig cc against the SDL2
MinGW files (SDL_DIR, see build.py); on Linux/macOS the installer builds
the settings program itself.
"""
import os
import re
import shutil
import sys
from pathlib import Path

import build

ROOT = Path(__file__).resolve().parent
OUT = ROOT / "dist" / "DuckTalesDOS"
FONTS = "font8x8_basic.h / font8x8_ext_latin.h: public domain (Daniel Hepper, IBM VGA fonts)\n"


def copy(src, dst):
    dst = Path(dst)
    if dst.is_dir():
        dst = dst / Path(src).name
    shutil.copyfile(ROOT / src, dst)


def game_material(out):
    """Files in the package that come from the game (must be none)."""
    names = re.compile(r"(\.arc|\.bin|^ducktale\.exe.*|^it\.drv|^ducktales_dos(\.exe)?|^img\d+\.c|^drv\.c)$", re.I)
    code = re.compile(rb"F00[0-9]_[0-9A-F]{4}_[0-9A-F]{4}\(void\)|Scrooge|Beagle")
    found = []
    for p in out.rglob("*"):
        if not p.is_file():
            continue
        if names.search(p.name) or (out / "src" in p.parents and code.search(p.read_bytes())):
            found.append(p)
    return found


def main():
    _, _, sdl = build.sdl_windows()
    if OUT.exists():
        shutil.rmtree(OUT)
    for d in ("src/tools/amiga", "src/runtime", "src/launcher", "src/sdl/lib", "licenses", "files"):
        (OUT / d).mkdir(parents=True)

    # programs and libraries that contain no game material
    build.zig_cc(["-O2", "-w", "-target", "x86_64-windows-gnu", "-Iruntime", f"-I{sdl / 'include' / 'SDL2'}",
                  "-mwindows", "launcher/settings.c", "launcher/diskimage.c", "runtime/settings86.c", str(sdl / "lib" / "libSDL2.dll.a"),
                  "-o", str(OUT / "settings.exe")])
    for pdb in OUT.glob("*.pdb"):
        pdb.unlink()
    shutil.copyfile(sdl / "bin" / "SDL2.dll", OUT / "SDL2.dll")
    for f in ("dist_src/install.bat", "dist_src/install.sh", "LICENSE", "dist_src/README.txt"):
        copy(f, OUT)
    os.chmod(OUT / "install.sh", 0o755)
    (OUT / "ducktales_dos.ini").write_text("# DuckTales (DOS) settings\ngamedir = files\n")

    # recompiler and runtime sources
    for f in ("recomp.py", "analyze.py", "dosexe.py", "itdrv.py", "unexepack.py", "extra_entries.txt"):
        copy(f"tools/{f}", OUT / "src/tools")
    copy("dist_src/install.py", OUT / "src/tools")
    for p in sorted((ROOT / "tools/amiga").glob("*.py")):        # Amiga sound option (sndgen.py)
        copy(p.relative_to(ROOT), OUT / "src/tools/amiga")
    for f in ("settings.c", "diskimage.c", "diskimage.h"):
        copy(f"launcher/{f}", OUT / "src/launcher")
    for p in sorted((ROOT / "runtime").iterdir()):
        if p.suffix in (".c", ".h"):
            copy(p.relative_to(ROOT), OUT / "src/runtime")
    shutil.copytree(sdl / "include", OUT / "src/sdl/include")
    shutil.copyfile(sdl / "lib" / "libSDL2.dll.a", OUT / "src/sdl/lib/libSDL2.dll.a")

    # licences
    shutil.copyfile(sdl.parent / "LICENSE.txt", OUT / "licenses/SDL2-LICENSE.txt")
    (OUT / "licenses/fonts.txt").write_text(FONTS)
    # .ipf disk images: the SPS decoder library, if at hand (.adf works without)
    caps = Path(os.environ.get("CAPS_DLL", ROOT.parent / "ducktales" / "build" / "CAPSImg.dll"))
    if caps.exists():
        shutil.copyfile(caps, OUT / "CAPSImg.dll")
        copy("launcher/CAPSImg-LICENCE.txt", OUT / "licenses")
    else:
        print(f"(no CAPSImg.dll at {caps}: .ipf import not included, .adf works)")

    # Safety check: nothing derived from the game may be in the package.
    found = game_material(OUT)
    if found:
        sys.exit("ERROR: game material found in the package:\n  " + "\n  ".join(map(str, found)))
    size = sum(p.stat().st_size for p in OUT.rglob("*") if p.is_file())
    print(f"Package: {OUT.relative_to(ROOT)} ({size / 1e6:.1f} MB)")


if __name__ == "__main__":
    main()
