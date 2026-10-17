"""Cross-platform installer for the DuckTales (DOS) port.

Usage: install.py [game folder] [--amiga]       (default: files)

Works on Windows (x64, ARM64 via x64 emulation), Linux (x86_64, aarch64) and
macOS (Intel, Apple Silicon):

1. Downloads the build tools into toolchain/ (first run only) and verifies
   their SHA-256 checksums:
     Zig 0.16.0 (C compiler)  - ziglang.org, checksums pinned below
     Capstone 5.0.7           - PyPI, checksum taken from PyPI's metadata
2. Finds SDL2: bundled on Windows; on Linux/macOS the system library
   (pkg-config sdl2, or the installed libSDL2 runtime with the bundled
   headers).
3. Builds the settings program when it is missing.
4. If <game folder>/DUCKTALE.EXE exists, unpacks it, recompiles the user's
   copy of the game (with IT.DRV) to C and builds the game. If the Amiga
   game folder (amiga_dir in ducktales_dos.ini) holds the Amiga version, its
   sound code is recompiled too (the "Amiga" sound option). With --amiga
   (settings: "Add Amiga sound") a game already converted only gets the
   Amiga sound code and is compiled again.

Everything made from the game stays in this folder on this machine.
"""
import hashlib
import json
import os
import platform
import runpy
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"
TC = ROOT / "toolchain"
ZIG_VERSION = "0.16.0"
ZIG = {
    "x86_64-windows": "68659eb5f1e4eb1437a722f1dd889c5a322c9954607f5edcf337bc3684a75a7e",
    "aarch64-windows": "aee38316ee4111717900f45dd3130145c39289e105541d737eb8c5ed653c78ef",
    "x86_64-linux": "70e49664a74374b48b51e6f3fdfbf437f6395d42509050588bd49abe52ba3d00",
    "aarch64-linux": "ea4b09bfb22ec6f6c6ceac57ab63efb6b46e17ab08d21f69f3a48b38e1534f17",
    "x86_64-macos": "0387557ed1877bc6a2e1802c8391953baddba76081876301c522f52977b52ba7",
    "aarch64-macos": "b23d70deaa879b5c2d486ed3316f7eaa53e84acf6fc9cc747de152450d401489",
}
CAPSTONE_VERSION = "5.0.7"


def die(msg):
    print(f"\nERROR: {msg}", flush=True)
    sys.exit(1)


def host():
    """(os, arch) of this machine: os in windows/linux/macos."""
    osname = {"win32": "windows", "darwin": "macos"}.get(sys.platform,
                                                          "linux" if sys.platform.startswith("linux") else "")
    machine = platform.machine().lower()
    arch = {"amd64": "x86_64", "x86_64": "x86_64", "arm64": "aarch64", "aarch64": "aarch64"}.get(machine, "")
    if not osname or not arch:
        die(f"unsupported system: {sys.platform} {platform.machine()}")
    return osname, arch


def download(url, dest, sha256):
    print(f"Downloading {url}", flush=True)
    tmp = dest.with_suffix(dest.suffix + ".part")
    with urllib.request.urlopen(url) as r, open(tmp, "wb") as f:
        shutil.copyfileobj(r, f)
    digest = hashlib.sha256(tmp.read_bytes()).hexdigest()
    if digest != sha256:
        tmp.unlink()
        die(f"checksum mismatch for {url}")
    tmp.replace(dest)


def get_zig(osname, arch):
    key = f"{arch}-{osname}"
    folder = TC / f"zig-{key}-{ZIG_VERSION}"
    exe = folder / ("zig.exe" if osname == "windows" else "zig")
    if exe.exists():
        return exe
    ext = "zip" if osname == "windows" else "tar.xz"
    archive = TC / f"zig.{ext}"
    download(f"https://ziglang.org/download/{ZIG_VERSION}/zig-{key}-{ZIG_VERSION}.{ext}", archive, ZIG[key])
    print("Extracting Zig...", flush=True)
    if ext == "zip":
        with zipfile.ZipFile(archive) as z:
            z.extractall(TC)
    else:
        with tarfile.open(archive) as t:
            t.extractall(TC)
    archive.unlink()
    if osname != "windows":
        exe.chmod(0o755)
    return exe


def wheel_tag_ok(name, osname, arch):
    if osname == "windows":
        return "win_amd64" in name if arch == "x86_64" else "win_arm64" in name
    if osname == "macos":
        return "macosx" in name and (arch in name or "universal2" in name or
                                     ("arm64" in name and arch == "aarch64"))
    return "manylinux" in name and arch in name


def get_capstone(osname, arch):
    """Unpack the Capstone wheel for this platform into toolchain/pylib."""
    pylib = TC / "pylib"
    if (pylib / "capstone").exists():
        return pylib
    with urllib.request.urlopen(f"https://pypi.org/pypi/capstone/{CAPSTONE_VERSION}/json") as r:
        meta = json.load(r)
    # Windows on ARM runs the x64 Python/wheel under emulation
    want_arch = "x86_64" if osname == "windows" else arch
    files = [f for f in meta["urls"] if f["filename"].endswith(".whl")
             and wheel_tag_ok(f["filename"], osname, want_arch)]
    if not files:
        die(f"no Capstone {CAPSTONE_VERSION} wheel for {osname} {arch}")
    f = files[0]
    wheel = TC / f["filename"]
    download(f["url"], wheel, f["digests"]["sha256"])
    with zipfile.ZipFile(wheel) as z:
        z.extractall(pylib)
    wheel.unlink()
    return pylib


def pkg_config(args):
    try:
        out = subprocess.run(["pkg-config", *args], capture_output=True, text=True, check=True)
        return out.stdout.split()
    except (OSError, subprocess.CalledProcessError):
        return None


def find_sdl(osname):
    """Compiler flags for SDL2 and the target triple for zig."""
    if osname == "windows":
        sdl = SRC / "sdl"
        return (["-target", "x86_64-windows-gnu", f"-I{sdl / 'include' / 'SDL2'}"],
                [str(sdl / "lib" / "libSDL2.dll.a")])
    cflags, libs = pkg_config(["--cflags", "sdl2"]), pkg_config(["--libs", "sdl2"])
    if cflags is not None and libs is not None:
        return cflags, libs
    if osname == "macos":
        for prefix in ("/opt/homebrew", "/usr/local"):
            lib = Path(prefix) / "lib" / "libSDL2.dylib"
            if lib.exists():
                return [f"-I{prefix}/include/SDL2", "-D_THREAD_SAFE"], [str(lib)]
        die("SDL2 not found. Install it with:  brew install sdl2")
    # Linux: the SDL2 runtime library is enough, the bundled headers are used.
    # SDL2_LIBDIR may point to a folder holding libSDL2-2.0.so.0.
    dirs = [os.environ["SDL2_LIBDIR"]] if os.environ.get("SDL2_LIBDIR") else []
    for d in (*dirs, "/usr/lib/x86_64-linux-gnu", "/usr/lib/aarch64-linux-gnu", "/usr/lib64", "/usr/lib"):
        lib = Path(d) / "libSDL2-2.0.so.0"
        if lib.exists():
            return [f"-I{SRC / 'sdl' / 'include' / 'SDL2'}"], [str(lib)]
    die("SDL2 not found. Install it, e.g.:  sudo apt install libsdl2-2.0-0   "
        "(Fedora: sudo dnf install SDL2)")


def zig_cc(zig, args):
    cache = ROOT / "build_tmp" / "zig-cache"
    env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(cache), ZIG_LOCAL_CACHE_DIR=str(cache))
    if subprocess.run([str(zig), "cc", *args], env=env).returncode == 0:
        return
    # A build that was interrupted can leave a broken cache (e.g. a C library
    # without snprintf): start again with an empty one.
    print("Build failed - clearing the compiler cache and trying again ...", flush=True)
    shutil.rmtree(cache, ignore_errors=True)
    subprocess.run([str(zig), "cc", *args], check=True, env=env)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    amiga_only = "--amiga" in sys.argv[1:]
    gamedir = Path(args[0] if args else "files")
    if not gamedir.is_absolute():
        gamedir = ROOT / gamedir
    osname, arch = host()
    print(f"System: {osname} {arch}", flush=True)
    TC.mkdir(exist_ok=True)
    zig = get_zig(osname, arch)
    sys.path.insert(0, str(get_capstone(osname, arch)))
    cflags, libs = find_sdl(osname)
    exe_ext = ".exe" if osname == "windows" else ""
    extra = ["-mwindows"] if osname == "windows" else ["-lm"]
    if osname == "linux":
        extra.append("-ldl")

    rt = SRC / "runtime"
    settings_exe = ROOT / f"settings{exe_ext}"
    # The settings program may be the one that started us (Build game), and
    # Windows cannot overwrite a running .exe. It ships prebuilt on Windows,
    # so it is only built when missing; elsewhere it is built to a temporary
    # name and swapped in, which also works while it runs.
    if not settings_exe.exists() or osname != "windows":
        print("Building the settings program...", flush=True)
        tmp = ROOT / f"settings.new{exe_ext}"
        zig_cc(zig, ["-O2", "-w", *cflags, f"-I{rt}", *extra,
                     str(SRC / "launcher" / "settings.c"),
                     str(SRC / "launcher" / "diskimage.c"), str(rt / "settings86.c"), *libs,
                     "-o", str(tmp)])
        try:
            os.replace(tmp, settings_exe)
        except OSError:
            print(f"(settings program is in use; new build left as {tmp.name})", flush=True)

    exe = find_file(gamedir, "DUCKTALE.EXE")
    drv = find_file(gamedir, "IT.DRV")
    if exe is None or drv is None:
        print(f"\nNo game files yet (DUCKTALE.EXE and IT.DRV not found in {gamedir}).\n"
              "Choose the folder of your DOS copy of the game in the settings program,\n"
              "then 'Build game from game files' (or run the installer again).", flush=True)
        return

    sys.path.insert(0, str(SRC / "tools"))
    gen = ROOT / "build_tmp" / "gen"
    if amiga_only and not amiga_program():
        die("no Amiga game in the Amiga game folder (it needs DT1/DuckTales); set it in the settings program")
    if amiga_only and (gen / "dispatch86.c").exists():
        print("[1/3] Game already converted", flush=True)
        print("[2/3] Amiga sound code ...", flush=True)
    else:
        convert(exe, drv, gen)
    add_amiga_sound(gen)
    compile_game(zig, cflags, libs, extra, exe_ext, gen)


def convert(exe, drv, gen):
    """Unpacks the user's DUCKTALE.EXE and recompiles it to C in gen/."""
    print(f"[1/3] Unpacking {exe} ...", flush=True)
    work = ROOT / "build_tmp" / "unpacked"
    work.mkdir(parents=True, exist_ok=True)
    import unexepack  # noqa: E402
    unpacked, tail, _ = unexepack.unpack(exe.read_bytes())
    (work / "DUCKTALE.EXE").write_bytes(unpacked)
    (work / "DUCKTALE.EXE.tail").write_bytes(tail)
    shutil.copyfile(drv, work / "IT.DRV")

    print("[2/3] Recompiling ...", flush=True)
    gen.mkdir(parents=True, exist_ok=True)
    import recomp  # noqa: E402
    funcs = recomp.Recompiler(str(work / "DUCKTALE.EXE")).write(str(gen))
    print(f"      {len(funcs)} functions", flush=True)


def add_amiga_sound(gen):
    """The Amiga sound code from the Amiga game folder, if set."""
    amiga = amiga_program()
    for f in ("amigasnd_gen.c", "amigasnd_layout.h"):
        (gen / f).unlink(missing_ok=True)
    if amiga:
        print(f"      Amiga sound from {amiga}", flush=True)
        for m in ("recomp", "analyze", "disasm", "m68k"):    # the Amiga tools' own modules
            sys.modules.pop(m, None)
        argv = sys.argv
        sys.argv = ["sndgen.py", str(amiga), str(gen)]
        runpy.run_path(str(SRC / "tools" / "amiga" / "sndgen.py"), run_name="__main__")
        sys.argv = argv
    else:
        print("      (no Amiga game folder: the Amiga sound option is not built)", flush=True)


def compile_game(zig, cflags, libs, extra, exe_ext, gen):
    rt = SRC / "runtime"
    print("[3/3] Compiling the game (the first build takes a few minutes) ...", flush=True)
    sources = sorted(str(p) for p in rt.glob("*.c"))
    sources += sorted(str(p) for p in gen.glob("*.c"))
    game_flags = [f for f in extra if f != "-mwindows"]
    game = ROOT / f"ducktales_dos{exe_ext}"
    tmp = ROOT / f"ducktales_dos.new{exe_ext}"
    zig_cc(zig, ["-O2", "-w", *cflags, f"-I{rt}", f"-I{gen}", *game_flags, *sources, *libs,
                 "-o", str(tmp)])
    try:
        os.replace(tmp, game)
    except OSError:
        die(f"{game.name} is in use - close the game and run the installer again")
    for pdb in ROOT.glob("*.new.pdb"):      # debug info from the Windows build
        pdb.unlink(missing_ok=True)
    print(f"\nDone: {game}\n"
          "Start the game from the settings program ('Save and start game').", flush=True)


def amiga_program():
    """DT1/DuckTales in the Amiga game folder of ducktales_dos.ini, if any."""
    ini = ROOT / "ducktales_dos.ini"
    folder = None
    if ini.exists():
        for line in ini.read_text(errors="replace").splitlines():
            key, _, val = line.partition("=")
            if key.strip() == "amiga_dir" and val.strip():
                folder = Path(val.strip())
    if folder is None:
        folder = Path("amiga")
    if not folder.is_absolute():
        folder = ROOT / folder
    dt1 = find_file(folder, "DT1") if folder.is_dir() else None
    return find_file(dt1, "DUCKTALES") if dt1 else None


def find_file(folder, name):
    """name in folder, in any letter case (DOS files may be lower case)."""
    if not folder.is_dir():
        return None
    for p in folder.iterdir():
        if p.name.upper() == name:
            return p
    return None


if __name__ == "__main__":
    main()
