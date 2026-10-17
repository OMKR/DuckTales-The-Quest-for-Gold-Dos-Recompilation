"""Development build: regenerates C from the game files and builds the game
and the settings program with zig cc and SDL2. Works on Windows, Linux and
macOS (python build.py).

Needs Python 3 with the `capstone` and `ziglang` packages, the DOS game in
files/ and SDL2:
  Windows: the SDL2 MinGW development files (SDL_DIR; else
           ../ducktales/tools/sdl/SDL2-2.30.9/x86_64-w64-mingw32 if present,
           else downloaded into toolchain/ with a pinned checksum)
  Linux/macOS: the system SDL2 (pkg-config sdl2)
Optional: the Amiga game (AMIGA, default ../ducktales/files) for the Amiga
sound option.

Writes build/ducktales_dos[.exe] and build/settings[.exe].
"""
import hashlib
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent
WINDOWS = sys.platform == "win32"
EXE = ".exe" if WINDOWS else ""
DEFAULT_SDL = ROOT.parent / "ducktales" / "tools" / "sdl" / "SDL2-2.30.9" / "x86_64-w64-mingw32"
# else downloaded into toolchain/ (official release, checksum pinned)
SDL_URL = "https://github.com/libsdl-org/SDL/releases/download/release-2.30.9/SDL2-devel-2.30.9-mingw.tar.gz"
SDL_SHA256 = "b188d165dc4a0372042bff3d0c18e9bb90772581e92a7b8b47b521b47714f75c"


def run(args, quiet=False):
    subprocess.run([str(a) for a in args], check=True, cwd=ROOT,
                   stdout=subprocess.DEVNULL if quiet else None)


def need(*modules):
    """Stops with a clear message if a Python package is missing."""
    import importlib.util
    missing = [m for m in modules if importlib.util.find_spec(m) is None]
    if missing:
        sys.exit(f"Missing Python packages for {sys.executable}: pip install {' '.join(missing)}")


def zig_cc(args):
    need("ziglang")
    run([sys.executable, "-m", "ziglang", "cc", *args])


def sdl_windows():
    """(compile flags, link inputs, folder) of the SDL2 MinGW files."""
    if os.environ.get("SDL_DIR"):
        sdl = Path(os.environ["SDL_DIR"])
    elif (DEFAULT_SDL / "include" / "SDL2").is_dir():
        sdl = DEFAULT_SDL
    else:
        sdl = download_sdl()
    if not (sdl / "include" / "SDL2").is_dir():
        sys.exit(f"SDL2 MinGW files not found in {sdl} (set SDL_DIR)")
    return ["-target", "x86_64-windows-gnu", f"-I{sdl / 'include' / 'SDL2'}"], [str(sdl / "lib" / "libSDL2.dll.a")], sdl


def download_sdl():
    """The SDL2 MinGW development files in toolchain/ (downloaded once)."""
    tc = ROOT / "toolchain"
    sdl = tc / "SDL2-2.30.9" / "x86_64-w64-mingw32"
    if (sdl / "include" / "SDL2").is_dir():
        return sdl
    tc.mkdir(exist_ok=True)
    archive = tc / "SDL2-mingw.tar.gz"
    print(f"Downloading {SDL_URL}", flush=True)
    with urllib.request.urlopen(SDL_URL) as r, open(archive, "wb") as f:
        shutil.copyfileobj(r, f)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SDL_SHA256:
        archive.unlink()
        sys.exit(f"checksum mismatch for {SDL_URL}")
    with tarfile.open(archive) as t:
        members = [m for m in t.getmembers() if m.name.startswith("SDL2-2.30.9/")]
        try:
            t.extractall(tc, members=members, filter="data")
        except TypeError:                       # Python before 3.12
            t.extractall(tc, members=members)
    archive.unlink()
    return sdl


def sdl_flags():
    """Compile flags and link inputs for SDL2 on this system."""
    if WINDOWS:
        cflags, libs, _ = sdl_windows()
        return cflags, libs
    try:
        cflags = subprocess.run(["pkg-config", "--cflags", "sdl2"], capture_output=True, text=True,
                                check=True).stdout.split()
        libs = subprocess.run(["pkg-config", "--libs", "sdl2"], capture_output=True, text=True,
                              check=True).stdout.split()
    except (OSError, subprocess.CalledProcessError):
        sys.exit("SDL2 not found (pkg-config sdl2). Install the SDL2 development package.")
    return cflags, libs + ["-lm"] + (["-ldl"] if sys.platform.startswith("linux") else [])


def main():
    need("capstone", "ziglang")
    game = ROOT / "files" / "DUCKTALE.EXE"
    if not game.exists():
        sys.exit("files/DUCKTALE.EXE not found (copy the DOS game into files/)")
    (ROOT / "unpacked").mkdir(exist_ok=True)
    run([sys.executable, "-I", "tools/unexepack.py", game, "unpacked/DUCKTALE.EXE"], quiet=True)
    run([sys.executable, "-I", "tools/recomp.py", "unpacked/DUCKTALE.EXE", "gen"])

    # Amiga sound option: the Amiga version's sound code, from its program
    amiga = Path(os.environ.get("AMIGA", ROOT.parent / "ducktales" / "files")) / "DT1" / "DuckTales"
    if amiga.exists():
        run([sys.executable, "tools/amiga/sndgen.py", amiga, "gen"], quiet=True)
    else:
        for f in ("amigasnd_gen.c", "amigasnd_layout.h"):
            (ROOT / "gen" / f).unlink(missing_ok=True)

    cflags, libs = sdl_flags()
    build = ROOT / "build"
    build.mkdir(exist_ok=True)
    sources = sorted(str(p.relative_to(ROOT)) for p in (ROOT / "runtime").glob("*.c"))
    sources += sorted(str(p.relative_to(ROOT)) for p in (ROOT / "gen").glob("*.c"))
    zig_cc(["-O2", "-w", "-Iruntime", "-Igen", *cflags, *sources, *libs, "-o", f"build/ducktales_dos{EXE}"])
    # settings program (stand-alone, writes ducktales_dos.ini)
    zig_cc(["-O2", "-w", "-Iruntime", *cflags, *(["-mwindows"] if WINDOWS else []),
            "launcher/settings.c", "launcher/diskimage.c", "runtime/settings86.c", *libs, "-o", f"build/settings{EXE}"])
    if WINDOWS:
        shutil.copyfile(sdl_windows()[2] / "bin" / "SDL2.dll", build / "SDL2.dll")


if __name__ == "__main__":
    main()
