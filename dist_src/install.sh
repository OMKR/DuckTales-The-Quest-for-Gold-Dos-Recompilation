#!/bin/sh
# Linux / macOS installer: runs src/tools/install.py with the system Python 3.
# Usage: ./install.sh [game folder containing DUCKTALE.EXE]
# Needs: Python 3.8+ and SDL2 (Linux: e.g. libsdl2-2.0-0, macOS: brew install sdl2).
cd "$(dirname "$0")" || exit 1
if [ ! -f src/tools/install.py ]; then
    echo "This is the installer template of the source repository." >&2
    echo "Run it from the release package: python3 make_dist.py creates dist/DuckTalesDOS," >&2
    echo "then run dist/DuckTalesDOS/install.sh." >&2
    exit 1
fi
PY=$(command -v python3 || command -v python)
if [ -z "$PY" ]; then
    echo "Python 3 is needed (e.g. sudo apt install python3, or brew install python)." >&2
    exit 1
fi
exec "$PY" src/tools/install.py "$@"
